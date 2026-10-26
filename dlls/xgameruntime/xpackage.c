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

    if (IsEqualGUID( iid, &IID_IUnknown       ) ||
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

static HRESULT WINAPI x_package_XPackageChangeChunkInstallOrder( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p stub!\n", iface, debugstr_a( packageIdentifier ), selectorCount, selectors );
    return E_NOTIMPL;
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

static HRESULT WINAPI __PADDING__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterPackageInstalled( IXPackageImpl4 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return TRUE;
}

static HRESULT WINAPI __PADDING_3__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
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

static HRESULT WINAPI __PADDING_4__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
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

static BOOL copy_xml_attribute( const char *text, const char *attribute, char *out, SIZE_T size )
{
    const char *p, *q;
    SIZE_T length;

    if (!(p = strstr( text, attribute ))) return FALSE;
    p += strlen( attribute );
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return FALSE;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return FALSE;
    p++;
    if (!(q = strchr( p, '"' ))) return FALSE;
    length = min( q - p, size - 1 );
    memcpy( out, p, length );
    out[length] = 0;
    return TRUE;
}

static BOOL copy_xml_element( const char *text, const char *tag, char *out, SIZE_T size )
{
    char open[32];
    const char *p, *q;
    SIZE_T length;

    snprintf( open, sizeof(open), "<%s>", tag );
    if (!(p = strstr( text, open ))) return FALSE;
    p += strlen( open );
    if (!(q = strchr( p, '<' ))) return FALSE;
    length = min( q - p, size - 1 );
    memcpy( out, p, length );
    out[length] = 0;
    return TRUE;
}

static void load_package_fields( void )
{
    WCHAR path[MAX_PATH];
    char *text, version[32];
    unsigned int major = 0, minor = 0, build = 0, revision = 0;
    WCHAR *sep;
    HANDLE file;

    if (package_fields_loaded) return;
    package_fields_loaded = TRUE;

    if (!GetModuleFileNameW( NULL, path, ARRAY_SIZE(path) )) return;
    if (!(sep = wcsrchr( path, '\\' ))) return;
    *sep = 0;

    text = NULL;
    for (;;)
    {
        WCHAR config_path[MAX_PATH];
        LARGE_INTEGER size;
        DWORD read;

        if (wcslen( path ) + ARRAY_SIZE(L"\\MicrosoftGame.Config") > ARRAY_SIZE(config_path)) break;
        wcscpy( config_path, path );
        wcscat( config_path, L"\\MicrosoftGame.Config" );

        if ((file = CreateFileW( config_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL )) != INVALID_HANDLE_VALUE)
        {
            if (GetFileSizeEx( file, &size ) && size.QuadPart && size.QuadPart <= 64 * 1024)
            {
                if ((text = malloc( size.QuadPart + 1 )))
                {
                    if (ReadFile( file, text, size.QuadPart, &read, NULL ) && read == size.QuadPart)
                        text[read] = 0;
                    else
                    {
                        free( text );
                        text = NULL;
                    }
                }
            }
            CloseHandle( file );
            break;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) break;
        if (!(sep = wcsrchr( path, '\\' ))) break;
        *sep = 0;
    }

    if (!text) return;

    /* configVersion="1" also matches "Version"; the package version is the
     * one on the Identity element. */
    {
        const char *identity = strstr( text, "<Identity" );
        if (identity && copy_xml_attribute( identity, "Version", version, sizeof(version) ))
        {
            sscanf( version, "%u.%u.%u.%u", &major, &minor, &build, &revision );
            package_fields.version.major = major;
            package_fields.version.minor = minor;
            package_fields.version.build = build;
            package_fields.version.revision = revision;
        }
    }
    copy_xml_attribute( text, "DefaultDisplayName", package_fields.displayName, sizeof(package_fields.displayName) );
    copy_xml_attribute( text, "Description", package_fields.description, sizeof(package_fields.description) );
    copy_xml_attribute( text, "PublisherDisplayName", package_fields.publisher, sizeof(package_fields.publisher) );
    copy_xml_element( text, "TitleId", package_fields.titleID, sizeof(package_fields.titleID) );
    copy_xml_element( text, "StoreId", package_fields.storeId, sizeof(package_fields.storeId) );
    TRACE( "package version %u.%u.%u.%u, titleId %s, storeId %s, displayName %s.\n",
           major, minor, build, revision,
           debugstr_a( package_fields.titleID ), debugstr_a( package_fields.storeId ),
           debugstr_a( package_fields.displayName ) );
    free( text );
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

static HRESULT WINAPI __PADDING_5__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
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
    __PADDING__,
    __PADDING_2__,
    x_package_XPackageUnregisterPackageInstalled,
    __PADDING_3__,
    x_package_XPackageGetMountPathSize,
    x_package_XPackageGetMountPath,
    x_package_XPackageCloseMountHandle,
    __PADDING_4__,
    x_package_XPackageEnumeratePackages,
    x_package_XPackageRegisterPackageInstalled,
    x_package_XPackageGetWriteStats,
    __PADDING_5__,
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
