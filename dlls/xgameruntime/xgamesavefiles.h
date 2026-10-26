/* SPDX-License-Identifier: LGPL-2.1-or-later
 * File-save provider. Included after the connected-storage implementation.
 * Plain UTF-8 file names are never exposed as the encoded blob cache.
 * A restricted-handle child owns the session lock and final post-exit upload.
 */
#define FILES_PATH 2048
#define FILES_MAX_BLOB (64 * 1024 * 1024)
struct files_ipc
{
    HANDLE parent, ready;
    HRESULT hr;
    UINT64 xuid;
    UINT32 title;
    BOOL trust, offline;
    char scid[129], pfn[256], msa[128], folder[FILES_PATH];
};
struct files_session
{
    struct files_session *next;
    UINT64 xuid;
    char scid[129];
    HANDLE mapping, ready, process;
    struct files_ipc *ipc;
};
static struct files_session *files_sessions;
static SRWLOCK files_sessions_lock = SRWLOCK_INIT;

static HRESULT files_wide( const char *text, WCHAR out[FILES_PATH] )
{
    return MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, FILES_PATH ) ? S_OK : HRESULT_FROM_WIN32( GetLastError() );
}
static HRESULT files_normalize_scid( const char *text, char out[129] )
{
    WCHAR wrapped[40], canonical[40];
    GUID guid;
    if (!text || strlen(text) != 36) return E_INVALIDARG;
    if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, text, 36, wrapped + 1, 36 )) return E_INVALIDARG;
    wrapped[0] = '{'; wrapped[37] = '}'; wrapped[38] = 0;
    if (FAILED(CLSIDFromString( wrapped, &guid )) || !StringFromGUID2( &guid, canonical, ARRAY_SIZE(canonical) )) return E_INVALIDARG;
    if (!WideCharToMultiByte( CP_UTF8, 0, canonical + 1, 36, out, 128, NULL, NULL )) return E_INVALIDARG;
    out[36] = 0;
    for (char *p = out; *p; p++) *p = tolower( (BYTE)*p );
    return S_OK;
}
static BOOL files_component( const char *name, BOOL container )
{
    const char *part = name;
    WCHAR wide[FILES_PATH];
    if (!*name || strlen( name ) > (container ? 256 : 260) || FAILED(files_wide( name, wide ))) return FALSE;
    if (!container)
        return wcslen( wide ) <= 65 && !strpbrk( name, "/\\:*?\"<>|" ) && strcmp( name, "." ) && strcmp( name, ".." ) && name[strlen(name)-1] != '.';
    while (*part)
    {
        const char *end = strchr( part, '/' );
        if (!end) end = part + strlen( part );
        if (end == part || *part == '.' || *part == '-' || end[-1] == '.') return FALSE;
        for (const char *p = part; p != end; p++)
            if (!(isalnum( (BYTE)*p ) && (BYTE)*p < 128) && *p != '_' && *p != '.' && *p != '-') return FALSE;
        if (strstr( part, ".." )) return FALSE;
        part = *end ? end + 1 : end;
        if (*end && !*part) return FALSE;
    }
    return TRUE;
}
static HRESULT files_mkdir( const char *path )
{
    WCHAR wide[FILES_PATH];
    HRESULT hr;
    if (FAILED(hr = files_wide( path, wide ))) return hr;
    for (WCHAR *p = wide + 3;; p++)
    {
        WCHAR c = *p;
        if (c != '\\' && c != '/' && c) continue;
        *p = 0;
        if (!CreateDirectoryW( wide, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32( GetLastError() );
        DWORD attrs = GetFileAttributesW( wide );
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
        *p = c;
        if (!c) break;
    }
    return S_OK;
}
static HRESULT files_roots( UINT64 xuid, const char *scid, char *folder, char *cache )
{
    WCHAR localW[512];
    char local[FILES_PATH], *hex;
    DWORD len;
    if (!xuid || !scid || !*scid || strlen( scid ) > 128) return E_INVALIDARG;
    len = GetEnvironmentVariableW( L"LOCALAPPDATA", localW, ARRAY_SIZE(localW) );
    if (!len || len >= ARRAY_SIZE(localW)) return HRESULT_FROM_WIN32( ERROR_BAD_PATHNAME );
    if (!WideCharToMultiByte( CP_UTF8, 0, localW, -1, local, sizeof(local), NULL, NULL )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!(hex = hex_encode( scid ))) return E_OUTOFMEMORY;
    int n = snprintf( folder, FILES_PATH, "%s\\XodusGameSaveFiles\\%I64u\\%s", local, xuid, hex );
    int m = snprintf( cache, FILES_PATH, "%s\\XodusGameSaveFilesCache\\%I64u\\%s", local, xuid, hex );
    free( hex );
    return n >= FILES_PATH || m >= FILES_PATH ? HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE ) : S_OK;
}
static HRESULT files_read( const char *path, BYTE **out, SIZE_T *size )
{
    WCHAR wide[FILES_PATH];
    HANDLE file;
    LARGE_INTEGER length;
    DWORD read;
    HRESULT hr;
    *out = NULL; *size = 0;
    if (FAILED(hr = files_wide( path, wide ))) return hr;
    DWORD attrs = GetFileAttributesW( wide );
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    file = CreateFileW( wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if (!GetFileSizeEx( file, &length )) hr = HRESULT_FROM_WIN32( GetLastError() );
    else if (length.QuadPart < 0 || length.QuadPart > FILES_MAX_BLOB) hr = E_GS_UPDATE_TOO_BIG;
    else if (!(*out = malloc( max( 1, length.QuadPart ) ))) hr = E_OUTOFMEMORY;
    else if (length.QuadPart && (!ReadFile( file, *out, length.QuadPart, &read, NULL ) || read != length.QuadPart)) hr = E_FAIL;
    else { *size = length.QuadPart; hr = S_OK; }
    CloseHandle( file );
    if (FAILED(hr)) { free( *out ); *out = NULL; }
    return hr;
}
static HRESULT files_write( const char *path, const BYTE *data, SIZE_T size )
{
    WCHAR wide[FILES_PATH], temp[FILES_PATH];
    HRESULT hr;
    DWORD count;
    HANDLE file;
    if (FAILED(hr = files_wide( path, wide ))) return hr;
    if (wcslen( wide ) + 5 >= ARRAY_SIZE(temp)) return HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE );
    swprintf( temp, ARRAY_SIZE(temp), L"%s.tmp", wide );
    file = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    hr = WriteFile( file, data, size, &count, NULL ) && count == size && FlushFileBuffers( file ) ? S_OK : E_FAIL;
    CloseHandle( file );
    if (SUCCEEDED(hr) && !MoveFileExW( temp, wide, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH )) hr = HRESULT_FROM_WIN32( GetLastError() );
    if (FAILED(hr)) DeleteFileW( temp );
    return hr;
}

/* Enumerate each complete parent subpath as one container, not just its first
 * component. Top-level files are deliberately ignored by the GDK contract. */
typedef HRESULT (*files_visit_callback)( const char *, const char *, const char *, const WIN32_FIND_DATAW *, void * );
static HRESULT files_walk( const char *root, const char *relative, files_visit_callback visit, void *context )
{
    char pattern[FILES_PATH], child[FILES_PATH];
    WCHAR wide[FILES_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    HRESULT hr;
    if (snprintf( pattern, sizeof(pattern), "%s\\%s%s*", root, relative, *relative ? "\\" : "" ) >= sizeof(pattern)) return E_GS_INVALID_CONTAINER_NAME;
    if (FAILED(hr = files_wide( pattern, wide ))) return hr;
    if ((find = FindFirstFileW( wide, &data )) == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32( GetLastError() );
    hr = S_OK;
    do
    {
        char name[260];
        if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED ); break; }
        if (!WideCharToMultiByte( CP_UTF8, 0, data.cFileName, -1, name, sizeof(name), NULL, NULL )) { hr = E_GS_INVALID_CONTAINER_NAME; break; }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (snprintf( child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", name ) >= sizeof(child) || !files_component( child, TRUE ))
            { hr = E_GS_INVALID_CONTAINER_NAME; break; }
            hr = files_walk( root, child, visit, context );
        }
        else if (*relative)
        {
            if (!files_component( name, FALSE ) || strlen(relative) + strlen(name) + 1 >= 260) { hr = E_GS_INVALID_CONTAINER_NAME; break; }
            hr = visit( root, relative, name, &data, context );
        }
        if (FAILED(hr)) break;
    } while (FindNextFileW( find, &data ));
    if (SUCCEEDED(hr) && GetLastError() != ERROR_NO_MORE_FILES) hr = HRESULT_FROM_WIN32( GetLastError() );
    FindClose( find );
    return hr;
}
struct files_import_context { struct XGameSaveProvider *provider; UINT64 total; };
static HRESULT files_import_one( const char *root, const char *container, const char *name, const WIN32_FIND_DATAW *info, void *arg )
{
    struct files_import_context *ctx = arg;
    struct container_state state;
    struct blob_entry *entry;
    char path[FILES_PATH], cached[FILES_PATH], directory[FILES_PATH];
    BYTE *data = NULL, *old = NULL;
    SIZE_T size, oldSize = 0;
    BOOL replaced = FALSE;
    HRESULT hr;
    if (((UINT64)info->nFileSizeHigh << 32 | info->nFileSizeLow) > FILES_MAX_BLOB) return E_GS_UPDATE_TOO_BIG;
    if (snprintf( path, sizeof(path), "%s\\%s\\%s", root, container, name ) >= sizeof(path)) return E_GS_INVALID_CONTAINER_NAME;
    if (FAILED(hr = files_read( path, &data, &size ))) return hr;
    if ((ctx->total += size) > ctx->provider->quota) { free( data ); return (HRESULT)0x80830006; }
    hr = load_state( ctx->provider, container, &state );
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) goto done;
    if (!blob_path( ctx->provider, container, name, cached, sizeof(cached) ) || !container_dir( ctx->provider, container, directory, sizeof(directory) )) { hr = E_OUTOFMEMORY; goto done; }
    if (FAILED(hr = files_mkdir( directory ))) goto done;
    if (!(entry = add_blob( &state, name ))) { hr = E_OUTOFMEMORY; goto done; }
    if (FAILED(files_read( cached, &old, &oldSize )) || oldSize != size || entry->size != size || memcmp( data, old, size ))
    {
        if (FAILED(hr = files_write( cached, data, size ))) goto done;
        replaced = TRUE;
        entry->size = size; entry->atom[0] = 0; state.dirty = TRUE; state.time = unix_now();
    }
    hr = save_state( ctx->provider, &state );
    if (FAILED(hr) && replaced && old) files_write( cached, old, oldSize );
done:
    free( data ); free( old ); free_state( &state ); return hr;
}
struct files_cache_context { const char *folder; HRESULT hr; BOOL prune; };
static BOOL files_cache_visit( struct XGameSaveProvider *provider, const char *container, void *arg )
{
    struct files_cache_context *ctx = arg;
    struct container_state state;
    char dir[FILES_PATH], path[FILES_PATH], cached[FILES_PATH];
    WCHAR wide[FILES_PATH];
    BYTE *data;
    SIZE_T size;
    HRESULT hr;
    if (!files_component( container, TRUE )) { ctx->hr = E_GS_INVALID_CONTAINER_NAME; return FALSE; }
    if (FAILED(hr = load_state( provider, container, &state ))) { free_state( &state ); ctx->hr = hr; return FALSE; }
    if (snprintf( dir, sizeof(dir), "%s\\%s", ctx->folder, container ) >= sizeof(dir)) { hr = E_GS_INVALID_CONTAINER_NAME; goto done; }
    if (!ctx->prune && FAILED(hr = files_mkdir( dir ))) goto done;
    for (UINT32 i = 0; i < state.count;)
    {
        struct blob_entry *entry = &state.blobs[i];
        if (!files_component( entry->name, FALSE ) || strlen(container) + strlen(entry->name) + 1 >= 260) { hr = E_GS_INVALID_CONTAINER_NAME; break; }
        if (snprintf( path, sizeof(path), "%s\\%s", dir, entry->name ) >= sizeof(path) || FAILED(hr = files_wide( path, wide ))) { hr = E_GS_INVALID_CONTAINER_NAME; break; }
        if (ctx->prune)
        {
            DWORD attrs = GetFileAttributesW( wide );
            if (attrs == INVALID_FILE_ATTRIBUTES && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
            { remove_blob( &state, entry->name ); state.dirty = TRUE; state.time = unix_now(); continue; }
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) { hr = E_FAIL; break; }
        }
        else
        {
            if (!blob_path( provider, container, entry->name, cached, sizeof(cached) )) { hr = E_OUTOFMEMORY; break; }
            if (FAILED(hr = files_read( cached, &data, &size ))) break;
            hr = files_write( path, data, size ); free( data ); if (FAILED(hr)) break;
        }
        i++;
    }
    if (SUCCEEDED(hr) && ctx->prune) hr = save_state( provider, &state );
done:
    free_state( &state ); ctx->hr = hr; return SUCCEEDED(hr);
}
static HRESULT files_import( struct XGameSaveProvider *provider, const char *folder )
{
    struct files_import_context scan = { provider, 0 };
    struct files_cache_context cache = { folder, S_OK, TRUE };
    HRESULT hr = files_walk( folder, "", files_import_one, &scan );
    if (FAILED(hr)) return hr;
    enumerate_local( provider, files_cache_visit, &cache );
    return cache.hr;
}
static HRESULT files_export( struct XGameSaveProvider *provider, const char *folder )
{
    char incoming[FILES_PATH], backup[FILES_PATH];
    WCHAR source[FILES_PATH], target[FILES_PATH], old[FILES_PATH];
    struct files_cache_context ctx = { incoming, S_OK, FALSE };
    HRESULT hr;
    snprintf( incoming, sizeof(incoming), "%s.incoming-%lu", folder, GetCurrentProcessId() );
    snprintf( backup, sizeof(backup), "%s.backup-%I64u-%lu", folder, GetTickCount64(), GetCurrentProcessId() );
    if (FAILED(hr = files_mkdir( incoming ))) return hr;
    enumerate_local( provider, files_cache_visit, &ctx );
    if (FAILED(ctx.hr)) return ctx.hr;
    if (FAILED(hr = files_wide( incoming, source )) || FAILED(hr = files_wide( folder, target )) || FAILED(hr = files_wide( backup, old ))) return hr;
    if (GetFileAttributesW( target ) != INVALID_FILE_ATTRIBUTES && !MoveFileExW( target, old, MOVEFILE_WRITE_THROUGH )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!MoveFileExW( source, target, MOVEFILE_WRITE_THROUGH ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() ); MoveFileExW( old, target, MOVEFILE_WRITE_THROUGH ); return hr;
    }
    /* The pre-sync local view is recoverable; never erase it during promotion. */
    return S_OK;
}

/* Download into a distinct cache tree. A truncated/failed transfer must not
 * erase the last usable cache. Successful swaps retain a recovery backup. */
static HRESULT files_cloud_download( struct XGameSaveProvider *provider, const char *name, const char *display, const char *remoteTime )
{
    char saved[FILES_PATH], incoming[FILES_PATH], target[FILES_PATH], backup[FILES_PATH];
    WCHAR fromW[FILES_PATH], targetW[FILES_PATH], backupW[FILES_PATH];
    struct container_state state;
    HRESULT hr;
    if (!files_component( name, TRUE )) return E_GS_INVALID_CONTAINER_NAME;
    strcpy( saved, provider->root );
    if (snprintf( provider->root, sizeof(provider->root), "%s.incoming-%lu", saved, GetCurrentProcessId() ) >= sizeof(provider->root))
    { strcpy( provider->root, saved ); return E_GS_INVALID_CONTAINER_NAME; }
    hr = cloud_download( provider, name, display, remoteTime );
    if (SUCCEEDED(hr))
    {
        hr = load_state( provider, name, &state );
        for (UINT32 i = 0; SUCCEEDED(hr) && i < state.count; i++)
            if (!files_component( state.blobs[i].name, FALSE ) || state.blobs[i].size > FILES_MAX_BLOB || strlen(name) + strlen(state.blobs[i].name) + 1 >= 260)
                hr = E_GS_INVALID_CONTAINER_NAME;
        free_state( &state );
    }
    if (!container_dir( provider, name, incoming, sizeof(incoming) )) hr = E_OUTOFMEMORY;
    strcpy( provider->root, saved );
    if (FAILED(hr)) return hr;
    if (!container_dir( provider, name, target, sizeof(target) )) return E_OUTOFMEMORY;
    if (snprintf( backup, sizeof(backup), "%s.backup-%I64u", target, GetTickCount64() ) >= sizeof(backup)) return E_GS_INVALID_CONTAINER_NAME;
    if (FAILED(hr = files_wide( incoming, fromW )) || FAILED(hr = files_wide( target, targetW )) || FAILED(hr = files_wide( backup, backupW ))) return hr;
    BOOL exists = GetFileAttributesW( targetW ) != INVALID_FILE_ATTRIBUTES;
    if (exists && !MoveFileExW( targetW, backupW, MOVEFILE_WRITE_THROUGH )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!MoveFileExW( fromW, targetW, MOVEFILE_WRITE_THROUGH ))
    { hr = HRESULT_FROM_WIN32( GetLastError() ); if (exists) MoveFileExW( backupW, targetW, MOVEFILE_WRITE_THROUGH ); return hr; }
    return S_OK;
}
static BOOL files_upload_visit( struct XGameSaveProvider *provider, const char *container, void *arg )
{
    HRESULT *result = arg;
    struct container_state state;
    HRESULT hr = load_state( provider, container, &state );
    if (SUCCEEDED(hr) && state.dirty && provider->cloud)
    {
        hr = cloud_upload( provider, &state );
        if (SUCCEEDED(hr)) { provider->uploaded++; hr = save_state( provider, &state ); }
    }
    free_state( &state );
    if (FAILED(hr)) *result = hr;
    return SUCCEEDED(hr);
}

/* rundll32 entry, private implementation export, never a public GDK API. Only
 * the mapping/parent/event handles cross the child boundary; no token files. */
void WINAPI XodusSaveFilesMonitor( HWND window, HINSTANCE instance, char *command, int show )
{
    char *end, cache[FILES_PATH], mutexName[320];
    UINT64 value = _strtoui64( command, &end, 10 );
    HANDLE mapping = (HANDLE)(ULONG_PTR)value, mutex = NULL;
    struct files_ipc *ipc = NULL;
    struct XGameSaveProvider *provider = NULL;
    IXThreadingImpl *threads = NULL;
    XUserHandle user = NULL;
    XAsyncBlock async = { 0 };
    HRESULT hr = E_INVALIDARG, com;
    if (!value || *end) return;
    if (!(ipc = MapViewOfFile( mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*ipc) ))) return;
    com = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if (!ipc->xuid || !ipc->scid[0] || ipc->scid[128] || ipc->pfn[255] || ipc->msa[127]) goto ready;
    snprintf( mutexName, sizeof(mutexName), "Local\\NexusFileSaves-%I64u-%s", ipc->xuid, ipc->scid );
    mutex = CreateMutexA( NULL, TRUE, mutexName );
    if (!mutex) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto ready; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) { hr = HRESULT_FROM_WIN32( ERROR_BUSY ); goto ready; }
    if (FAILED(hr = files_roots( ipc->xuid, ipc->scid, ipc->folder, cache ))) goto ready;
    WCHAR folderW[FILES_PATH];
    if (FAILED(hr = files_wide( ipc->folder, folderW ))) goto ready;
    BOOL view_existed = GetFileAttributesW( folderW ) != INVALID_FILE_ATTRIBUTES;
    if (FAILED(hr = files_mkdir( ipc->folder )) || FAILED(hr = files_mkdir( cache ))) goto ready;
    if (!(provider = calloc( 1, sizeof(*provider) ))) { hr = E_OUTOFMEMORY; goto ready; }
    InitializeCriticalSection( &provider->cs );
    provider->scid = strdup( ipc->scid ); provider->files = TRUE; provider->quota = SAVE_QUOTA;
    lstrcpynA( provider->root, cache, sizeof(provider->root) );
    if (!provider->scid) { hr = E_OUTOFMEMORY; goto ready; }
    if (!ipc->offline)
    {
        MESSAGE( "nexus-files: phase=account\n" );
        /* Configuration identity is inherited from the initialized game, not
         * inferred from rundll32's install directory. Authentication is real. */
        packageFamilyName = strdup( ipc->pfn ); msaAppId = strdup( ipc->msa );
        titleId = ipc->title; fullTrust = ipc->trust;
        if (!packageFamilyName || !msaAppId) { hr = E_OUTOFMEMORY; goto ready; }
        if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threads ))) goto ready;
        if (FAILED(hr = IXUserImpl_XUserAddAsync( x_user_impl, XUserAddOptions_AddDefaultUserSilently, &async )) ||
            FAILED(hr = IXThreadingImpl_XAsyncGetStatus( threads, &async, TRUE )) ||
            FAILED(hr = IXUserImpl_XUserAddResult( x_user_impl, &async, &user ))) goto ready;
        if (xuser_get_xuid( user ) != ipc->xuid) { hr = (HRESULT)0x80830002; goto ready; }
        provider->user = user; user = NULL;
        provider->cloud = TRUE;
        snprintf( provider->base, sizeof(provider->base), "https://titlestorage.xboxlive.com/connectedstorage/users/xuid(%I64u)/scids/%s", ipc->xuid, ipc->scid );
        if (FAILED(hr = xuser_device_authorization( provider->user, "http://xboxlive.com", &provider->auth ))) goto ready;
        if (!cloud_lock( provider )) { hr = E_GS_CONTAINER_NOT_IN_SYNC; goto ready; }
        provider->session_lock = TRUE;
    }
    if (view_existed && FAILED(hr = files_import( provider, ipc->folder ))) goto ready;
    if (provider->cloud && FAILED(hr = cloud_sync( provider ))) goto ready;
    if (provider->cloud) MESSAGE( "nexus-files: phase=download downloaded=%u\n", provider->downloaded );
    if (provider->cloud)
    {
        enumerate_local( provider, files_upload_visit, &hr );
        if (FAILED(hr)) goto ready;
    }
    if (FAILED(hr = files_export( provider, ipc->folder ))) goto ready;
ready:
    ipc->hr = hr; MemoryBarrier(); SetEvent( ipc->ready );
    if (SUCCEEDED(hr))
    {
        MESSAGE( "nexus-files: provider-ready cloud=%d\n", provider->cloud );
        while (WaitForSingleObject( ipc->parent, 30000 ) == WAIT_TIMEOUT)
        {
            if (provider->cloud)
            {
                provider->locked = FALSE;
                if (!cloud_lock( provider )) { ipc->hr = E_GS_CONTAINER_NOT_IN_SYNC; hr = ipc->hr; break; }
            }
        }
        /* The parent has exited: all save file handles are closed. */
        if (SUCCEEDED(hr)) hr = files_import( provider, ipc->folder );
        if (SUCCEEDED(hr) && provider->cloud) enumerate_local( provider, files_upload_visit, &hr );
        MESSAGE( "nexus-files: final-sync hr=%#lx uploaded=%u\n", hr, provider->uploaded );
    }
    if (FAILED(hr)) ERR( "file save provider failed hr=%#lx; local data is retained\n", hr );
    if (provider) { provider->session_lock = FALSE; close_provider( provider ); }
    if (user) IXUserImpl_XUserCloseHandle( x_user_impl, user );
    if (threads) IXThreadingImpl_Release( threads );
    if (mutex) { ReleaseMutex( mutex ); CloseHandle( mutex ); }
    if (SUCCEEDED(com)) CoUninitialize();
    CloseHandle( ipc->parent ); CloseHandle( ipc->ready ); UnmapViewOfFile( ipc ); CloseHandle( mapping );
}

static HRESULT files_start_session( UINT64 xuid, const char *scid, struct files_session **out )
{
    struct files_session *session;
    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    STARTUPINFOEXW startup = { { sizeof(startup) } };
    PROCESS_INFORMATION process;
    HANDLE handles[3], parent = NULL;
    SIZE_T bytes = 0;
    BOOL attributes_initialized = FALSE;
    WCHAR command[512], system[260];
    char mode[16];
    HRESULT hr = E_OUTOFMEMORY;
    AcquireSRWLockExclusive( &files_sessions_lock );
    for (session = files_sessions; session; session = session->next)
        if (session->xuid == xuid && !strcmp( session->scid, scid ))
        {
            /* Retry genuine initialization failures after the old monitor has
             * exited. Keep old contexts alive for concurrent result readers. */
            if (WaitForSingleObject( session->process, 0 ) == WAIT_OBJECT_0 && FAILED(session->ipc->hr)) continue;
            *out = session; ReleaseSRWLockExclusive( &files_sessions_lock ); return S_OK;
        }
    if (!(session = calloc( 1, sizeof(*session) ))) goto done;
    session->xuid = xuid; strcpy( session->scid, scid );
    session->mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(struct files_ipc), NULL );
    session->ready = CreateEventW( &security, TRUE, FALSE, NULL );
    if (!session->mapping || !session->ready || !(session->ipc = MapViewOfFile( session->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*session->ipc) ))) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto fail; }
    if (!DuplicateHandle( GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE, TRUE, 0 )) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto fail; }
    session->ipc->parent = parent; session->ipc->ready = session->ready; session->ipc->xuid = xuid;
    strcpy( session->ipc->scid, scid ); session->ipc->title = titleId; session->ipc->trust = fullTrust;
    session->ipc->offline = !packageFamilyName || (GetEnvironmentVariableA( "XODUS_SAVE_SYNC", mode, sizeof(mode) ) && !_stricmp( mode, "none" ));
    if ((packageFamilyName && strlen(packageFamilyName) >= sizeof(session->ipc->pfn)) || (msaAppId && strlen(msaAppId) >= sizeof(session->ipc->msa))) { hr = E_INVALIDARG; goto fail; }
    strcpy( session->ipc->pfn, packageFamilyName ? packageFamilyName : "" ); strcpy( session->ipc->msa, msaAppId ? msaAppId : "" );
    handles[0] = session->mapping; handles[1] = session->ready; handles[2] = parent;
    InitializeProcThreadAttributeList( NULL, 1, 0, &bytes );
    if (!(startup.lpAttributeList = malloc( bytes ))) goto fail;
    if (!InitializeProcThreadAttributeList( startup.lpAttributeList, 1, 0, &bytes ))
    { hr = HRESULT_FROM_WIN32( GetLastError() ); goto attributes; }
    attributes_initialized = TRUE;
    if (!UpdateProcThreadAttribute( startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), NULL, NULL ))
    { hr = HRESULT_FROM_WIN32( GetLastError() ); goto attributes; }
    if (!GetSystemDirectoryW( system, ARRAY_SIZE(system) )) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto attributes; }
    swprintf( command, ARRAY_SIZE(command), L"\"%s\\rundll32.exe\" xgameruntime.dll,XodusSaveFilesMonitor %I64u", system, (UINT64)(ULONG_PTR)session->mapping );
    if (!CreateProcessW( NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL, &startup.StartupInfo, &process ))
    { hr = HRESULT_FROM_WIN32( GetLastError() ); goto attributes; }
    CloseHandle( process.hThread ); session->process = process.hProcess;
    session->next = files_sessions; files_sessions = session; *out = session; hr = S_OK;
attributes:
    if (attributes_initialized) DeleteProcThreadAttributeList( startup.lpAttributeList );
    free( startup.lpAttributeList );
    if (SUCCEEDED(hr)) { CloseHandle( parent ); goto done; }
fail:
    if (parent) CloseHandle( parent );
    if (session->ipc) UnmapViewOfFile( session->ipc );
    if (session->ready) CloseHandle( session->ready );
    if (session->mapping) CloseHandle( session->mapping );
    free( session );
done:
    ReleaseSRWLockExclusive( &files_sessions_lock ); return hr;
}

struct files_async { LONG refs, canceled; XUserHandle user; char scid[129], folder[FILES_PATH]; };
static void files_async_release( struct files_async *ctx )
{
    if (!InterlockedDecrement( &ctx->refs )) { IXUserImpl_XUserCloseHandle( x_user_impl, ctx->user ); free( ctx ); }
}
struct files_worker { XAsyncProviderData data; IXThreadingImpl *threads; };
static DWORD WINAPI files_worker_thread( void *arg )
{
    struct files_worker *worker = arg;
    struct files_async *ctx = worker->data.context;
    struct files_session *session;
    HRESULT hr = files_start_session( xuser_get_xuid( ctx->user ), ctx->scid, &session );
    if (SUCCEEDED(hr))
    {
        HANDLE waits[2] = { session->ready, session->process };
        DWORD result = WaitForMultipleObjects( 2, waits, FALSE, 120000 );
        hr = result == WAIT_OBJECT_0 ? session->ipc->hr : HRESULT_FROM_WIN32( result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED );
        if (SUCCEEDED(hr)) strcpy( ctx->folder, session->ipc->folder );
    }
    if (InterlockedCompareExchange( &ctx->canceled, 0, 0 )) hr = E_ABORT;
    IXThreadingImpl_XAsyncComplete( worker->threads, worker->data.async, hr, SUCCEEDED(hr) ? strlen(ctx->folder) + 1 : 0 );
    files_async_release( ctx ); IXThreadingImpl_Release( worker->threads ); free( worker ); return 0;
}
static HRESULT WINAPI files_async_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct files_async *ctx = data->context;
    IXThreadingImpl *threads;
    HRESULT hr = S_OK;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threads ))) return hr;
    switch (op)
    {
        case XAsyncOp_Begin: hr = IXThreadingImpl_XAsyncSchedule( threads, data->async, 0 ); break;
        case XAsyncOp_DoWork:
        {
            struct files_worker *worker;
            HANDLE thread;
            if (!(worker = calloc( 1, sizeof(*worker) ))) { hr = E_OUTOFMEMORY; break; }
            worker->data = *data; worker->threads = threads; IXThreadingImpl_AddRef( threads );
            InterlockedIncrement( &ctx->refs );
            if (!(thread = CreateThread( NULL, 0, files_worker_thread, worker, 0, NULL )))
            { hr = HRESULT_FROM_WIN32( GetLastError() ); files_async_release( ctx ); IXThreadingImpl_Release( threads ); free( worker ); break; }
            CloseHandle( thread ); hr = E_PENDING; break;
        }
        case XAsyncOp_GetResult: memcpy( data->buffer, ctx->folder, strlen(ctx->folder) + 1 ); break;
        case XAsyncOp_Cancel: InterlockedExchange( &ctx->canceled, TRUE ); break;
        case XAsyncOp_Cleanup: files_async_release( ctx ); break;
    }
    IXThreadingImpl_Release( threads ); return hr;
}
static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl3 *iface, XUserHandle user, const char *scid, XAsyncBlock *async )
{
    struct files_async *ctx;
    IXThreadingImpl *threads;
    char normalized[129];
    HRESULT hr;
    if (!user || !async || FAILED(files_normalize_scid( scid, normalized ))) return E_INVALIDARG;
    if (!xuser_get_xuid( user )) return (HRESULT)0x80830002;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threads ))) return hr;
    if (!(ctx = calloc( 1, sizeof(*ctx) ))) { IXThreadingImpl_Release( threads ); return E_OUTOFMEMORY; }
    ctx->refs = 1; strcpy( ctx->scid, normalized );
    if (FAILED(hr = IXUserImpl_XUserDuplicateHandle( x_user_impl, user, &ctx->user ))) { free(ctx); IXThreadingImpl_Release( threads ); return hr; }
    hr = IXThreadingImpl_XAsyncBegin( threads, async, ctx, files_async_provider, "XGameSaveFilesGetFolderWithUiAsync", files_async_provider );
    if (FAILED(hr)) files_async_release( ctx );
    IXThreadingImpl_Release( threads ); return hr;
}
static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T size, char *folder )
{
    IXThreadingImpl *threads;
    HRESULT hr;
    if (!async || !size || !folder) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threads ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( threads, async, files_async_provider, size, folder, NULL );
    IXThreadingImpl_Release( threads ); return hr;
}
static HRESULT files_quota_one( const char *root, const char *container, const char *name, const WIN32_FIND_DATAW *data, void *arg )
{
    UINT64 *total = arg;
    *total += ((UINT64)data->nFileSizeHigh << 32) | data->nFileSizeLow;
    return S_OK;
}
static HRESULT WINAPI x_game_save_XGameSaveFilesGetRemainingQuota( IXGameSaveImpl3 *iface, XUserHandle user, const char *scid, INT64 *quota )
{
    char folder[FILES_PATH], cache[FILES_PATH], normalized[129];
    UINT64 total = 0;
    HRESULT hr;
    if (!user || !quota || FAILED(files_normalize_scid( scid, normalized ))) return E_INVALIDARG;
    if (!xuser_get_xuid( user )) return (HRESULT)0x80830002;
    if (FAILED(hr = files_roots( xuser_get_xuid( user ), normalized, folder, cache ))) return hr;
    if (FAILED(hr = files_walk( folder, "", files_quota_one, &total ))) return hr;
    *quota = total >= SAVE_QUOTA ? 0 : SAVE_QUOTA - total;
    return S_OK;
}
