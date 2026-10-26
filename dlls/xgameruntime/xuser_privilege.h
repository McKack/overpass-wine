/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Privilege resolution uses authenticated server claims, never subscription or
 * age guesses. No credential, environment override or title-specific policy. */
#ifndef XUSER_PRIVILEGE_H
#define XUSER_PRIVILEGE_H

static const char privilege_identity;

static BOOL privilege_valid( XUserPrivilege privilege )
{
    switch (privilege)
    {
        case XUserPrivilege_CrossPlay: case XUserPrivilege_Clubs:
        case XUserPrivilege_Sessions: case XUserPrivilege_Broadcast:
        case XUserPrivilege_ManageProfilePrivacy: case XUserPrivilege_GameDvr:
        case XUserPrivilege_MultiplayerParties: case XUserPrivilege_CloudManageSession:
        case XUserPrivilege_CloudJoinSession: case XUserPrivilege_CloudSavedGames:
        case XUserPrivilege_SocialNetworkSharing: case XUserPrivilege_UserGeneratedContent:
        case XUserPrivilege_Communications: case XUserPrivilege_Multiplayer:
        case XUserPrivilege_AddFriends: return TRUE;
        default: return FALSE;
    }
}

/* Lengths come from HSTRING, not strlen. Reject embedded NUL, signs, partial
 * numbers, overflow and malformed suffixes even after a matching privilege. */
static HRESULT privilege_claim( const WCHAR *xid, UINT32 xid_len, UINT64 expected,
                                const WCHAR *prv, UINT32 prv_len, XUserPrivilege privilege,
                                BOOLEAN *granted )
{
    UINT64 actual = 0;
    UINT32 i, number;
    BOOL found = FALSE;
    if (!granted) return E_INVALIDARG;
    *granted = FALSE;
    if (!expected || !xid || !xid_len || xid_len > 20 || !prv || prv_len > 16384 ||
        !privilege_valid( privilege )) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    for (i = 0; i < xid_len; ++i)
    {
        if (xid[i] < '0' || xid[i] > '9' || actual > (~(UINT64)0 - (xid[i] - '0')) / 10)
            return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        actual = actual * 10 + xid[i] - '0';
    }
    if (actual != expected) return E_GAMEUSER_NO_AUTH_USER;
    for (i = 0; i < prv_len;)
    {
        if (prv[i] == ' ') { ++i; continue; }
        number = 0;
        do
        {
            if (prv[i] < '0' || prv[i] > '9' || number > (~(UINT32)0 - (prv[i] - '0')) / 10)
                return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
            number = number * 10 + prv[i++] - '0';
        } while (i < prv_len && prv[i] != ' ');
        if (number == (UINT32)privilege) found = TRUE;
    }
    *granted = found;
    return S_OK;
}

typedef HRESULT (*privilege_verify_fn)( IUser *, XUserPrivilege );
struct privilege_context
{
    LONG refs, canceled;
    IUser *user;
    IXThreadingImpl *threading;
    XUserPrivilege privilege;
    privilege_verify_fn verify;
};
struct privilege_worker
{
    struct privilege_context *context;
    XAsyncBlock *async; /* Runtime-owned provider copy, never caller memory. */
};

static void privilege_release( struct privilege_context *context )
{
    if (InterlockedDecrement( &context->refs )) return;
    IUser_Release( context->user );
    IXThreadingImpl_Release( context->threading );
    free( context );
}

static DWORD WINAPI privilege_worker_thread( void *arg )
{
    struct privilege_worker *worker = arg;
    struct privilege_context *context = worker->context;
    HRESULT hr, com = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if (InterlockedCompareExchange( &context->canceled, 0, 0 )) hr = E_ABORT;
    else if (FAILED(com)) hr = com;
    else hr = context->verify( context->user, context->privilege );
    if (hr == E_PENDING) hr = E_UNEXPECTED;
    if (InterlockedCompareExchange( &context->canceled, 0, 0 )) hr = E_ABORT;
    IXThreadingImpl_XAsyncComplete( context->threading, worker->async, hr, 0 );
    if (SUCCEEDED(com)) CoUninitialize();
    privilege_release( context );
    free( worker );
    return 0;
}

static HRESULT WINAPI privilege_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct privilege_context *context = data->context;
    struct privilege_worker *worker;
    HANDLE thread;
    HRESULT hr;
    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( context->threading, data->async, 0 );
        case XAsyncOp_DoWork:
            if (!(worker = calloc( 1, sizeof(*worker) ))) return E_OUTOFMEMORY;
            worker->context = context;
            worker->async = data->async;
            InterlockedIncrement( &context->refs );
            if (!(thread = CreateThread( NULL, 0, privilege_worker_thread, worker, 0, NULL )))
            {
                hr = HRESULT_FROM_WIN32( GetLastError() );
                privilege_release( context );
                free( worker );
                return hr;
            }
            CloseHandle( thread );
            return E_PENDING;
        case XAsyncOp_Cancel:
            InterlockedExchange( &context->canceled, TRUE );
            IXThreadingImpl_XAsyncComplete( context->threading, data->async, E_ABORT, 0 );
            break;
        case XAsyncOp_Cleanup:
            privilege_release( context );
            break;
        case XAsyncOp_GetResult:
            break;
    }
    return S_OK;
}

/* The verifier is a compile-time internal dependency, not a public runtime
 * hook. The standalone native fixture substitutes synthetic, offline input. */
static HRESULT privilege_begin( IXThreadingImpl *threading, IUser *user,
                                XUserPrivilegeOptions options, XUserPrivilege privilege,
                                XAsyncBlock *async, privilege_verify_fn verify )
{
    struct privilege_context *context;
    HRESULT hr;
    if (!user || !async || !verify || !privilege_valid( privilege ) ||
        (options != XUserPrivilegeOptions_None && options != XUserPrivilegeOptions_AllUsers))
        return E_INVALIDARG;
    /* Reject a pending operation. Reuse from inside a completion callback is
     * valid after terminal status. */
    if (async->internal[0] && IXThreadingImpl_XAsyncGetStatus( threading, async, FALSE ) == E_PENDING)
        return E_INVALIDARG;
    if (!(context = calloc( 1, sizeof(*context) ))) return E_OUTOFMEMORY;
    context->refs = 1;
    context->user = user;
    context->threading = threading;
    context->privilege = privilege;
    context->verify = verify;
    IUser_AddRef( user );
    IXThreadingImpl_AddRef( threading );
    hr = IXThreadingImpl_XAsyncBegin( threading, async, context, &privilege_identity,
                                     "XUserResolvePrivilegeWithUiAsync", privilege_provider );
    if (FAILED(hr)) privilege_release( context );
    return hr;
}

static HRESULT privilege_result( IXThreadingImpl *threading, XAsyncBlock *async )
{
    if (!async || async->internal[2] != &privilege_identity) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetStatus( threading, async, FALSE );
}
#endif
