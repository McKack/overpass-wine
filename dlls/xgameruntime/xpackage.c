/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XPackage
 *
 * Copyright 2026 Olivia Ryan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "private.h"
#include <string.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#include "xpackage_content.h"

/* The legacy package IID shares the same COM prefix with the newer package
 * interfaces. Keep every newer slot in place when supplying its older entries. */
static const GUID legacy_package_iid =
    {0xbe3f3e9f,0xc4b3,0x424d,{0x9a,0x95,0xd1,0xaf,0x06,0x05,0x59,0xa5}};
static HRESULT enumerate_current_package( XPackageKind, XPackageEnumerationScope, void *, XPackageEnumerationCallback * );
static HRESULT WINAPI x_package_XPackageEnumerateFeatures( IXPackageImpl4 *, const char *, void *, XPackageFeatureEnumerationCallback * );

struct x_package
{
    IXPackageImpl4 IXPackageImpl4_iface;
    LONG ref;
};

static inline struct x_package *impl_from_IXPackageImpl4( IXPackageImpl4 *iface )
{
    return CONTAINING_RECORD( iface, struct x_package, IXPackageImpl4_iface );
}

static HRESULT WINAPI x_package_QueryInterface( IXPackageImpl4 *iface, REFIID iid, void **out )
{
    struct x_package *impl = impl_from_IXPackageImpl4( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;
    *out = NULL;
    if (!iid) return E_INVALIDARG;
    if (IsEqualGUID( iid, &IID_IUnknown       ) ||
        IsEqualGUID( iid, &legacy_package_iid ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl  ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl2 ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl3 ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl4 ))
    {
        IXPackageImpl_AddRef( *out = &impl->IXPackageImpl4_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_package_AddRef( IXPackageImpl4 *iface )
{
    struct x_package *impl = impl_from_IXPackageImpl4( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_package_Release( IXPackageImpl4 *iface )
{
    struct x_package *impl = impl_from_IXPackageImpl4( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* The package's own identifier, taken from the layout file beside it.
 *
 * Every package ships a layout_<guid>.xml at its root, and that guid without
 * its dashes is the 32 characters a caller asks for here -- titles pass a
 * bufferSize of 33, which is those characters and a terminator.
 *
 * Refusing left the buffer untouched, so titles built monitors for a package
 * identified as "" and died (the engine's GDKPackageChunkInstall raises a
 * LowLevelFatalError on this failure).
 *
 * The root is not always where the executable is -- Unreal ships its binary
 * several directories down -- so walk up until the layout turns up, the same
 * way the game config is found. */
static HRESULT package_identifier( char *buffer, SIZE_T bufferSize )
{
    WCHAR path[MAX_PATH];
    WCHAR *sep;

    if (!GetModuleFileNameW( NULL, path, ARRAY_SIZE(path) )) return E_FAIL;
    if (!(sep = wcsrchr( path, '\\' ))) return E_FAIL;
    *sep = 0;

    for (;;)
    {
        WCHAR pattern[MAX_PATH];
        WIN32_FIND_DATAW data;
        HANDLE find;

        if (wcslen( path ) + ARRAY_SIZE(L"\\layout_*.xml") > ARRAY_SIZE(pattern)) return E_FAIL;
        wcscpy( pattern, path );
        wcscat( pattern, L"\\layout_*.xml" );

        if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
        {
            const WCHAR *guid = data.cFileName + ARRAY_SIZE(L"layout_") - 1;
            SIZE_T used = 0;

            for (; *guid && *guid != '.'; guid++)
            {
                if (*guid == '-') continue;
                if (used + 1 >= bufferSize) { FindClose( find ); return E_NOT_SUFFICIENT_BUFFER; }
                buffer[used++] = (char)*guid;
            }
            buffer[used] = 0;
            FindClose( find );
            return used ? S_OK : E_FAIL;
        }

        if (!(sep = wcsrchr( path, '\\' ))) return E_FAIL;
        *sep = 0;
    }
}

static HRESULT WINAPI x_package_XPackageGetCurrentProcessPackageIdentifier( IXPackageImpl4 *iface, SIZE_T bufferSize, char *buffer )
{
    HRESULT hr;

    TRACE( "iface %p, bufferSize %Iu, buffer %p.\n", iface, bufferSize, buffer );

    if (!buffer || !bufferSize) return E_INVALIDARG;
    if (FAILED(hr = package_identifier( buffer, bufferSize )))
        FIXME( "no layout file to take a package identifier from: %#lx.\n", hr );
    return hr;
}

static BOOLEAN WINAPI x_package_XPackageIsPackagedProcess( IXPackageImpl4 *iface )
{
    TRACE( "iface %p.\n", iface );

    /* Everything reaching this runtime came out of an MSIX package and is run
     * beside the appxmanifest and MicrosoftGame.config that describe it.
     * Answering FALSE sends a title down its unpackaged path (Deep Rock
     * Galactic asked twice and then put up its own crash dialog). */
    return TRUE;
}

/* A monitor over a title that is already fully installed.
 *
 * Xodus extracts a package in full before it can be launched, so there is never
 * a partial install to watch: every monitor reports complete from the moment it
 * is made. Refusing to make one is not a soft failure -- a title treats it as
 * fatal (Deep Rock Galactic put up "Failed to create installation monitor.
 * 0x80004001" before opening a window). */
struct installation_monitor
{
    UINT64 bytes;
};

static HRESULT WINAPI x_package_XPackageCreateInstallationMonitor( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *installationMonitor )
{
    struct installation_monitor *impl;

    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, "
           "minimumUpdateIntervalMs %u, queue %p, installationMonitor %p.\n",
           iface, debugstr_a( packageIdentifier ), selectorCount, selectors,
           minimumUpdateIntervalMs, queue, installationMonitor );

    if (!installationMonitor) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    /* Byte counts are left at zero. They are only there for a progress bar,
     * and there is no progress to draw: what a title actually reads is
     * completed and launchable, both of which are true from the start. */
    *installationMonitor = (XPackageInstallationMonitorHandle)impl;
    return S_OK;
}

static void WINAPI x_package_XPackageCloseInstallationMonitorHandle( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor )
{
    TRACE( "iface %p, installationMonitor %p.\n", iface, installationMonitor );
    free( installationMonitor );
}

static void WINAPI x_package_XPackageGetInstallationProgress( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor, XPackageInstallationProgress *progress )
{
    struct installation_monitor *impl = (struct installation_monitor *)installationMonitor;

    TRACE( "iface %p, installationMonitor %p, progress %p.\n", iface, installationMonitor, progress );

    if (!progress) return;

    /* Installed in full, and therefore launchable and complete. */
    progress->totalBytes = impl ? impl->bytes : 0;
    progress->installedBytes = progress->totalBytes;
    progress->launchBytes = progress->totalBytes;
    progress->launchable = TRUE;
    progress->completed = TRUE;
}

static BOOLEAN WINAPI x_package_XPackageUpdateInstallationMonitor( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor )
{
    FIXME( "iface %p, installationMonitor %p stub!\n", iface, installationMonitor );
    return TRUE;
}

static LONG64 installation_progress_token;

static HRESULT WINAPI x_package_XPackageRegisterInstallationProgressChanged( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor, void *context, XPackageInstallationProgressCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, installationMonitor %p, context %p, callback %p, token %p: accepted, "
           "progress never changes.\n", iface, installationMonitor, context, callback, token );

    if (!token) return E_INVALIDARG;

    /* Nothing will ever be raised -- the package is already complete -- but the
     * caller still gets a token it can hold and unregister, rather than reading
     * back whatever was on its stack. */
    token->token = InterlockedIncrement64( &installation_progress_token );
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterInstallationProgressChanged( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, installationMonitor %p, token %p, wait %d stub!\n", iface, installationMonitor, &token, wait );
    return TRUE;
}

static HRESULT WINAPI x_package_XPackageGetUserLocale( IXPackageImpl4 *iface, SIZE_T localeSize, char *locale )
{
    char requested[LOCALE_NAME_MAX_LENGTH];
    WCHAR user_locale[LOCALE_NAME_MAX_LENGTH];
    DWORD length;
    INT converted;

    TRACE( "iface %p, localeSize %Iu, locale %p.\n", iface, localeSize, locale );
    if (!locale || !localeSize) return E_INVALIDARG;

    /* The launcher selects a locale declared by the installed package. */
    length = GetEnvironmentVariableA( "XODUS_PACKAGE_LOCALE", requested, sizeof(requested) );
    if (length >= sizeof(requested)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    if (length)
    {
        if (length + 1 > localeSize) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        memcpy( locale, requested, length + 1 );
        return S_OK;
    }

    if (!GetUserDefaultLocaleName( user_locale, ARRAY_SIZE(user_locale) ))
        return HRESULT_FROM_WIN32( GetLastError() );
    converted = WideCharToMultiByte( CP_UTF8, 0, user_locale, -1, NULL, 0, NULL, NULL );
    if (!converted) return HRESULT_FROM_WIN32( GetLastError() );
    if ((SIZE_T)converted > localeSize) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    if (!WideCharToMultiByte( CP_UTF8, 0, user_locale, -1, locale, converted, NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

/* Everything a title can ask about is already on disk.
 *
 * Chunks are how a package is installed in pieces, so a title can start before
 * the rest arrives and wait on what it still needs. Xodus extracts a package in
 * full before it can be launched, so every chunk is Ready and always was.
 * Refusing to say so leaves a title waiting for content that is already there:
 * Resident Evil 2 plays its publisher logos and then sits on a black screen. */
static HRESULT WINAPI x_package_XPackageFindChunkAvailability( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, XPackageChunkAvailability *availability )
{
    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, availability %p.\n",
           iface, debugstr_a( packageIdentifier ), selectorCount, selectors, availability );

    if (!availability) return E_INVALIDARG;
    *availability = XPackageChunkAvailability_Ready;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageEnumerateChunkAvailability( IXPackageImpl4 *iface, const char *packageIdentifier, XPackageChunkSelectorType type, void *context, XPackageChunkAvailabilityCallback *callback )
{
    TRACE( "iface %p, packageIdentifier %s, type %d, context %p, callback %p.\n",
           iface, debugstr_a( packageIdentifier ), type, context, callback );

    if (!callback) return E_INVALIDARG;

    /* A package installed in one piece has no chunks to enumerate, and an
     * enumeration that visits nothing is the true answer rather than a
     * failure. What a title actually waits on is the availability above. */
    return S_OK;
}

/* Installation evidence only, never an ownership or download assertion. Content
 * lookup validates its layout and declared payload. For the current executable,
 * also require matching on-disk config/Appx identity and a readable layout. */
static HRESULT installed_chunk_package( const char *identifier )
{
    struct content_game current;
    struct content_package *package;
    WCHAR path[MAX_PATH], file[MAX_PATH], *sep;
    char store[13];
    HRESULT hr;
    UINT i;

    if (!identifier || strlen(identifier) != 32) return E_INVALIDARG;
    for (i = 0; i < 32; i++)
        if (!((identifier[i] >= '0' && identifier[i] <= '9') ||
              (identifier[i] >= 'a' && identifier[i] <= 'f'))) return E_INVALIDARG;

    hr = xpackage_query_content_store( identifier, sizeof(store), store );
    if (hr != E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER) return hr;
    if (!GetModuleFileNameW( NULL, path, ARRAY_SIZE(path) ) || !(sep = wcsrchr(path, '\\')))
        return E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER;
    *sep = 0;
    AcquireSRWLockShared( &content_config_lock );
    current = content_current;
    ReleaseSRWLockShared( &content_config_lock );
    if (!(package = calloc( 1, sizeof(*package) ))) return E_OUTOFMEMORY;
    for (;;)
    {
        if (!content_join( path, L"MicrosoftGame.config", file )) break;
        if (GetFileAttributesW( file ) != INVALID_FILE_ATTRIBUTES)
        {
            if (content_load( path, package ) && !package->content &&
                !strcmp( current.identity, package->identity ) &&
                !strcmp( current.publisher, package->publisher_id ) &&
                !strcmp( current.store, package->store ) && content_load_layout( package ) &&
                !strcmp( identifier, package->identifier )) hr = S_OK;
            break;
        }
        if (!(sep = wcsrchr( path, '\\' )) || sep <= path + 2) break;
        *sep = 0;
    }
    free( package );
    return hr;
}

static HRESULT WINAPI x_package_XPackageChangeChunkInstallOrder( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    UINT i;

    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p.\n",
           iface, debugstr_a( packageIdentifier ), selectorCount, selectors );
    if (selectorCount && !selectors) return E_INVALIDARG;
    for (i = 0; i < selectorCount; i++)
    {
        switch (selectors[i].type)
        {
        case XPackageChunkSelectorType_Chunk:
            break;
        case XPackageChunkSelectorType_Language:
        case XPackageChunkSelectorType_Tag:
        case XPackageChunkSelectorType_Feature:
            if (!selectors[i].language || !selectors[i].language[0]) return E_INVALIDARG;
            break;
        default:
            return E_INVALIDARG;
        }
    }
    /* Extracted packages are installed in full before launch. The SDK ignores
     * chunks already installed or not scheduled by intelligent delivery, so no
     * queue mutation is needed. Unknown installations must still fail. */
    return installed_chunk_package( packageIdentifier );
}

static HRESULT WINAPI x_package_XPackageInstallChunks( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *installationMonitor )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, minimumUpdateIntervalMs %u, suppressUserConfirmation %d, queue %p, installationMonitor %p stub!\n", iface, debugstr_a( packageIdentifier ), selectorCount, selectors, minimumUpdateIntervalMs, suppressUserConfirmation, queue, installationMonitor );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageInstallChunksAsync( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, minimumUpdateIntervalMs %u, suppressUserConfirmation %d, asyncBlock %p stub!\n", iface, packageIdentifier, selectorCount, selectors, minimumUpdateIntervalMs, suppressUserConfirmation, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageInstallChunksResult( IXPackageImpl4 *iface, XAsyncBlock *asyncBlock, XPackageInstallationMonitorHandle *installationMonitor )
{
    FIXME( "iface %p, asyncBlock %p, installationMonitor %p stub!\n", iface, asyncBlock, installationMonitor );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEstimateDownloadSize( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT64 *downloadSize, BOOLEAN *shouldPresentUserConfirmation )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, downloadSize %p, shouldPresentUserConfirmation %p stub!\n", iface, packageIdentifier, selectorCount, selectors, downloadSize, shouldPresentUserConfirmation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageUninstallChunks( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectores %p stub!\n", iface, packageIdentifier, selectorCount, selectors );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackagesLegacy( IXPackageImpl4 *iface,
    XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current_package( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalledLegacy( IXPackageImpl4 *iface,
    XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p: install events unsupported.\n",
           iface, queue, context, callback, token );
    if (!token) return E_INVALIDARG;
    token->token = 0;
    if (!callback) return E_INVALIDARG;
    /* Discovery of on-disk packages is not a package-install event service. */
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterPackageInstalled( IXPackageImpl4 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return TRUE;
}

static HRESULT WINAPI x_package_XPackageMountLegacy( IXPackageImpl4 *iface,
    const char *identifier, XPackageMountHandle *mount )
{
    FIXME( "iface %p, identifier %s, mount %p: content mounting unsupported.\n", iface, debugstr_a(identifier), mount );
    if (!mount) return E_INVALIDARG;
    *mount = NULL;
    if (!identifier || !identifier[0]) return E_INVALIDARG;
    /* Do not turn an installation lookup into a licensed mount. */
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageGetMountPathSize( IXPackageImpl4 *iface, XPackageMountHandle mount, SIZE_T *pathSize )
{
    FIXME( "iface %p, mount %p, pathSize %p stub!\n", iface, mount, pathSize );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageGetMountPath( IXPackageImpl4 *iface, XPackageMountHandle mount, SIZE_T pathSize, char *path )
{
    FIXME( "iface %p, mount %p, pathSize %Iu, path %p stub!\n", iface, mount, pathSize, path );
    return E_NOTIMPL;
}

static void WINAPI x_package_XPackageCloseMountHandle( IXPackageImpl4 *iface, XPackageMountHandle mount )
{
    FIXME( "iface %p, mount %p stub!\n", iface, mount );
}

static HRESULT WINAPI x_package_XPackageGetIdentifierLegacy( IXPackageImpl4 *iface,
    const char *store, SIZE_T size, char *identifier )
{
    TRACE( "iface %p, store %s, size %Iu, identifier %p.\n", iface, debugstr_a(store), size, identifier );
    return xpackage_query_identifier( store, size, identifier );
}

/* The one package this process runs from: Xodus extracts a single MSIX
 * package in full, so an enumeration either visits that package (when the
 * caller asks for its kind) or visits nothing. Fields we have no data for
 * are empty rather than invented; installing is FALSE because there is
 * nothing left to install. */

/* Titles read their own record out of XPackageDetails -- Forza prints the
 * package version on its title screen and gates initialization steps on it.
 * The layout file carries no version, but MicrosoftGame.Config -- the GDK
 * standard file shipped beside the executable -- carries Identity/Version,
 * the display metadata and TitleId/StoreId. It is read once, on demand, by
 * walking up from the executable directory the same way the layout file is
 * found; nothing here knows which title is running. */
static struct
{
    XVersion version;
    char displayName[128];
    char description[256];
    char publisher[128];
    char titleID[16];
    char storeId[64];
} package_fields;
static BOOL package_fields_loaded;

static void load_package_fields( void )
{
    WCHAR path[MAX_PATH];
    char version[32];
    xmlDocPtr doc = NULL;
    xmlNodePtr root, visuals;
    WCHAR *sep;

    if (package_fields_loaded) return;
    package_fields_loaded = TRUE;

    if (!GetModuleFileNameW( NULL, path, ARRAY_SIZE(path) )) return;
    if (!(sep = wcsrchr( path, '\\' ))) return;
    *sep = 0;

    for (;;)
    {
        WCHAR config_path[MAX_PATH];

        if (wcslen( path ) + ARRAY_SIZE(L"\\MicrosoftGame.Config") > ARRAY_SIZE(config_path)) break;
        wcscpy( config_path, path );
        wcscat( config_path, L"\\MicrosoftGame.Config" );

        if (GetFileAttributesW( config_path ) != INVALID_FILE_ATTRIBUTES)
        {
            doc = content_read_xml( config_path );
            break;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) break;
        if (!(sep = wcsrchr( path, '\\' ))) break;
        *sep = 0;
    }

    if (!doc) return;
    root = xmlDocGetRootElement( doc );
    if (!root || strcmp( (char *)root->name, "Game" )) { xmlFreeDoc( doc ); return; }

    /* Share the bounded, DTD-free XML reader with content packages. Only active
     * direct elements can supply metadata, never commented examples, CDATA
     * markup, similarly named attributes or unrelated nested elements. */
    if (content_text( content_child( root, "Identity" ), "Version", version, sizeof(version) ))
        content_version( version, &package_fields.version );
    visuals = content_child( root, "ShellVisuals" );
    content_text( visuals, "DefaultDisplayName", package_fields.displayName, sizeof(package_fields.displayName) );
    content_text( visuals, "Description", package_fields.description, sizeof(package_fields.description) );
    content_text( visuals, "PublisherDisplayName", package_fields.publisher, sizeof(package_fields.publisher) );
    content_text( content_child( root, "TitleId" ), NULL, package_fields.titleID, sizeof(package_fields.titleID) );
    content_text( content_child( root, "StoreId" ), NULL, package_fields.storeId, sizeof(package_fields.storeId) );
    TRACE( "package version %u.%u.%u.%u, titleId %s, storeId %s, displayName %s.\n",
           package_fields.version.major, package_fields.version.minor,
           package_fields.version.build, package_fields.version.revision,
           debugstr_a( package_fields.titleID ), debugstr_a( package_fields.storeId ),
           debugstr_a( package_fields.displayName ) );
    xmlFreeDoc( doc );
}

static HRESULT enumerate_current_package( XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    char identifier[33]; /* 32 characters and a terminator, as titles ask for. */
    XPackageDetails details;
    HRESULT hr;

    if (!callback || (UINT)kind > XPackageKind_PublisherContent || (UINT)scope > XPackageEnumerationScope_ThisPublisher) return E_INVALIDARG;
    if (kind == XPackageKind_Content) return enumerate_content_packages(scope, context, callback);
    if (kind == XPackageKind_PublisherContent) return E_NOTIMPL;
    if (FAILED(hr = package_identifier( identifier, sizeof(identifier) ))) return hr;

    load_package_fields();

    memset( &details, 0, sizeof(details) );
    details.packageIdentifier = identifier;
    details.kind = XPackageKind_Game;
    details.installing = FALSE;
    details.index = 0;
    details.count = 1;
    details.version = package_fields.version;
    details.displayName = package_fields.displayName[0] ? package_fields.displayName : NULL;
    details.description = package_fields.description[0] ? package_fields.description : NULL;
    details.publisher = package_fields.publisher[0] ? package_fields.publisher : NULL;
    details.storeId = package_fields.storeId[0] ? package_fields.storeId : NULL;
    details.titleID = package_fields.titleID[0] ? package_fields.titleID : NULL;
    callback( context, &details );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages( IXPackageImpl4 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current_package( kind, scope, context, callback );
}

HRESULT xpackage_query_identifier( const char *requested_store, SIZE_T size, char *identifier )
{
    HRESULT hr;
    if (identifier && size) identifier[0] = 0;
    if (!content_id(requested_store) || !identifier) return E_INVALIDARG;
    if (size < 33) return E_NOT_SUFFICIENT_BUFFER;
    if (storeId && !strcmp(requested_store, storeId))
    {
        hr = package_identifier(identifier, size);
        return hr == E_FAIL ? E_INVALIDARG : hr;
    }
    return content_query_identifier(requested_store, size, identifier);
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled( IXPackageImpl4 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageGetWriteStats( IXPackageImpl4 *iface, XPackageWriteStats *writeStats )
{
    FIXME( "iface %p, writeStats %p stub!\n", iface, writeStats );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEnumerateFeaturesLegacy( IXPackageImpl4 *iface,
    const char *identifier, void *context, XPackageFeatureEnumerationCallback *callback )
{
    return x_package_XPackageEnumerateFeatures( iface, identifier, context, callback );
}

static HRESULT WINAPI x_package_XPackageUninstallUWPInstance( IXPackageImpl4 *iface, const char *packageName )
{
    FIXME( "iface %p, packageName %s stub!\n", iface, debugstr_a( packageName ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEnumerateFeatures( IXPackageImpl4 *iface, const char *packageIdentifier, void *context, XPackageFeatureEnumerationCallback *callback )
{
    TRACE( "iface %p, packageIdentifier %s, context %p, callback %p.\n",
           iface, debugstr_a( packageIdentifier ), context, callback );

    if (!callback) return E_INVALIDARG;

    /* Optional features are declared in MicrosoftGame.config, and nothing
     * installed here declares any: an enumeration that visits nothing is the
     * true answer, not a failure. Refusing was enough to end Deep Rock
     * Galactic, which put up a crash dialog carrying no message at all. */
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageUninstallPackage( IXPackageImpl4 *iface, const char *packageIdentifier )
{
    FIXME( "iface %p, packageIdentifier %s", iface, debugstr_a( packageIdentifier ) );
    return FALSE;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages2( IXPackageImpl4 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current_package( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled2( IXPackageImpl4 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageMountWithUiAsync( IXPackageImpl4 *iface, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, packageIdentifier %s, async %p stub!\n", iface, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageMountWithUiResult( IXPackageImpl4 *iface, XAsyncBlock *async, XPackageMountHandle *mount )
{
    FIXME( "iface %p, async %p, mount %p stub!\n", iface, async, mount );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages3( IXPackageImpl4 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current_package( kind, scope, context, callback );
}

static LONG64 package_installed_token;

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled3( IXPackageImpl4 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p: accepted, nothing will be raised -- "
           "packages are fully installed before they can run.\n", iface, queue, context, callback, token );

    if (!token) return E_INVALIDARG;

    token->token = InterlockedIncrement64( &package_installed_token );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageGetPackageKind( IXPackageImpl4 *iface, const char *packageIdentifier, XPackageKind *kind )
{
    TRACE( "iface %p, packageIdentifier %s, kind %p.\n", iface, debugstr_a( packageIdentifier ), kind );

    if (!packageIdentifier || !kind) return E_INVALIDARG;

    /* This runtime only ever runs game packages extracted by Xodus; there is
     * no package database that could honestly answer for related content or
     * publisher packages, so the kind of the package a title asks about --
     * in every observed call its own -- is Game. */
    *kind = XPackageKind_Game;
    return S_OK;
}

static const struct IXPackageImpl4Vtbl x_package_vtbl =
{
    x_package_QueryInterface,
    x_package_AddRef,
    x_package_Release,
    /* IXPackageImpl methods */
    x_package_XPackageGetCurrentProcessPackageIdentifier,
    x_package_XPackageIsPackagedProcess,
    x_package_XPackageCreateInstallationMonitor,
    x_package_XPackageCloseInstallationMonitorHandle,
    x_package_XPackageGetInstallationProgress,
    x_package_XPackageUpdateInstallationMonitor,
    x_package_XPackageRegisterInstallationProgressChanged,
    x_package_XPackageUnregisterInstallationProgressChanged,
    x_package_XPackageGetUserLocale,
    x_package_XPackageFindChunkAvailability,
    x_package_XPackageEnumerateChunkAvailability,
    x_package_XPackageChangeChunkInstallOrder,
    x_package_XPackageInstallChunks,
    x_package_XPackageInstallChunksAsync,
    x_package_XPackageInstallChunksResult,
    x_package_XPackageEstimateDownloadSize,
    x_package_XPackageUninstallChunks,
    x_package_XPackageEnumeratePackagesLegacy,
    x_package_XPackageRegisterPackageInstalledLegacy,
    x_package_XPackageUnregisterPackageInstalled,
    x_package_XPackageMountLegacy,
    x_package_XPackageGetMountPathSize,
    x_package_XPackageGetMountPath,
    x_package_XPackageCloseMountHandle,
    x_package_XPackageGetIdentifierLegacy,
    x_package_XPackageEnumeratePackages,
    x_package_XPackageRegisterPackageInstalled,
    x_package_XPackageGetWriteStats,
    x_package_XPackageEnumerateFeaturesLegacy,
    x_package_XPackageUninstallUWPInstance,
    x_package_XPackageEnumerateFeatures,
    x_package_XPackageUninstallPackage,
    /* IXPackageImpl2 methods */
    x_package_XPackageEnumeratePackages2,
    x_package_XPackageRegisterPackageInstalled2,
    x_package_XPackageMountWithUiAsync,
    x_package_XPackageMountWithUiResult,
    /* IXPackageImpl3 methods */
    x_package_XPackageEnumeratePackages3,
    x_package_XPackageRegisterPackageInstalled3,
    /* IXPackageImpl4 methods */
    x_package_XPackageGetPackageKind,
};

static struct x_package x_package =
{
    {&x_package_vtbl},
    0,
};

IXPackageImpl *x_package_impl = (IXPackageImpl *)&x_package.IXPackageImpl4_iface;
