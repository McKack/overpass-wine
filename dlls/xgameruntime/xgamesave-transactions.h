/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Download into a private tree before replacing a usable container. Included
 * after cloud_download_into. Never chooses a winner for a sync conflict. */
/* Structural completeness is checked before the small field iterators run.
 * A partial listing must never be interpreted as authoritative deletions. This
 * does not replace the field/type/duplicate checks performed by each caller. */
static BOOL save_json_object_complete( const char *data, SIZE_T size )
{
    char stack[64];
    UINT depth = 0;
    SIZE_T i;
    BOOL quoted = FALSE, started = FALSE, complete = FALSE;
    if (!data || !size) return FALSE;
    for (i = 0; i < size; i++)
    {
        unsigned char c = data[i];
        if (quoted)
        {
            if (c < 32) return FALSE;
            if (c == '"') quoted = FALSE;
            else if (c == '\\')
            {
                if (++i == size) return FALSE;
                if (data[i] == 'u')
                {
                    UINT j;
                    if (size - i <= 4) return FALSE;
                    for (j = 0; j < 4; j++) if (!isxdigit((unsigned char)data[++i])) return FALSE;
                }
                else if (!data[i] || !strchr("\"\\/bfnrt", data[i])) return FALSE;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (complete || !c) return FALSE;
        if (!started && c != '{') return FALSE;
        started = TRUE;
        if (c == '"') quoted = TRUE;
        else if (c == '{' || c == '[')
        {
            if (depth == ARRAY_SIZE(stack)) return FALSE;
            stack[depth++] = c;
        }
        else if (c == '}' || c == ']')
        {
            if (!depth || stack[--depth] != (c == '}' ? '{' : '[')) return FALSE;
            if (!depth) complete = TRUE;
        }
        else if (c < 32) return FALSE;
    }
    return complete && !quoted && !depth;
}

static HRESULT cloud_download( struct XGameSaveProvider *provider, const char *name,
                               const char *display, const char *remote_time )
{
    struct XGameSaveProvider incoming = {0};
    char source[2048], target[2048], recovery[2048], guid_text[64];
    WCHAR stage_wide[4096], source_wide[4096], target_wide[4096], recovery_wide[4096], guid_wide[40];
    GUID guid;
    DWORD attributes, error;
    BOOL exists;
    HRESULT hr;

    if (FAILED(hr = CoCreateGuid(&guid))) return hr;
    if (!StringFromGUID2(&guid, guid_wide, ARRAY_SIZE(guid_wide)) ||
        !WideCharToMultiByte(CP_UTF8, 0, guid_wide, -1, guid_text, sizeof(guid_text), NULL, NULL))
        return E_FAIL;
    if (snprintf(incoming.root, sizeof(incoming.root), "%s\\.incoming-%s", provider->root, guid_text) >= sizeof(incoming.root) ||
        !container_dir(provider, name, target, sizeof(target)) ||
        snprintf(recovery, sizeof(recovery), "%s\\.recovery-%s-%s", provider->root, strrchr(target, '\\') + 1, guid_text) >= sizeof(recovery))
        return E_GS_INVALID_CONTAINER_NAME;
    if (!save_wide_path(incoming.root, stage_wide) || !save_wide_path(target, target_wide) ||
        !save_wide_path(recovery, recovery_wide)) return E_GS_INVALID_CONTAINER_NAME;
    if (FAILED(hr = files_mkdir(provider->root))) return hr;
    /* Exclusive creation: never clean or reuse a pre-existing staging tree. */
    if (!CreateDirectoryW(stage_wide, NULL)) return HRESULT_FROM_WIN32(GetLastError());
    incoming.user = provider->user;
    incoming.auth = provider->auth;
    incoming.files = provider->files;
    strcpy(incoming.base, provider->base);
    hr = cloud_download_into(&incoming, name, display, remote_time);
    if (FAILED(hr)) goto done;
    if (!container_dir(&incoming, name, source, sizeof(source)) || !save_wide_path(source, source_wide))
    { hr = E_OUTOFMEMORY; goto done; }
    attributes = GetFileAttributesW(target_wide);
    error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (exists && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    { hr = HRESULT_FROM_WIN32(ERROR_DIRECTORY); goto done; }
    if (!exists && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    { hr = HRESULT_FROM_WIN32(error); goto done; }
    if (exists && !MoveFileExW(target_wide, recovery_wide, MOVEFILE_WRITE_THROUGH))
    { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    if (!MoveFileExW(source_wide, target_wide, MOVEFILE_WRITE_THROUGH))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (exists && !MoveFileExW(recovery_wide, target_wide, MOVEFILE_WRITE_THROUGH))
            ERR("save replacement rollback failed; the previous container is retained in its recovery directory.\n");
        goto done;
    }
    /* Keep the previous container for recovery. Leading-dot transaction trees
     * are excluded by both providers' local/cloud enumeration rules. */
done:
    delete_directory(incoming.root);
    return hr;
}
