/*
 * Xbox Game runtime Library
 *  GDK Component: Networking API -> XNetworking
 *
 * Written by Weather
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
#include "winhttp.h"
#include "wincrypt.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_networking
{
    IXNetworkingImpl2 IXNetworkingImpl2_iface;
    LONG ref;
};

static inline struct x_networking *impl_from_IXNetworkingImpl2( IXNetworkingImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_networking, IXNetworkingImpl2_iface );
}

static HRESULT WINAPI x_networking_QueryInterface( IXNetworkingImpl2 *iface, REFIID iid, void **out )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXNetworkingImpl  ) ||
        IsEqualGUID( iid, &IID_IXNetworkingImpl2 ))
    {
        IXNetworkingImpl_AddRef( *out = &impl->IXNetworkingImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_networking_AddRef( IXNetworkingImpl2 *iface )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_networking_Release( IXNetworkingImpl2 *iface )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort( IXNetworkingImpl2 *iface, UINT16 *preferredLocalUdpMultiplayerPort )
{
    FIXME( "iface %p, preferredLocalUdpMultiplayerPort %p stub!\n", iface, preferredLocalUdpMultiplayerPort );
    return E_NOTIMPL;
}

/* The platform has no actual port preference on this runtime; completing
 * with 0 lets the caller's socket bind choose freely, which is what native
 * GDK reports when no preference applies. */
static HRESULT WINAPI XNetworkingPreferredPortProvider( XAsyncOp op, const XAsyncProviderData *data )
{
    IXThreadingImpl *threading;
    HRESULT hr;

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        hr = IXThreadingImpl_XAsyncSchedule( threading, data->async, 0 );
        break;
    case XAsyncOp_DoWork:
        IXThreadingImpl_XAsyncComplete( threading, data->async, S_OK, sizeof(UINT16) );
        hr = S_OK;
        break;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(UINT16)) hr = HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        else { *(UINT16 *)data->buffer = 0; hr = S_OK; }
        break;
    default:
        hr = S_OK;
        break;
    }
    IXThreadingImpl_Release( threading );
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock )
{
    IXThreadingImpl *threading;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );

    if (!asyncBlock) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    hr = IXThreadingImpl_XAsyncBegin( threading, asyncBlock, NULL, NULL, "XNetworkingQueryPreferredLocalUdpMultiplayerPort", XNetworkingPreferredPortProvider );
    IXThreadingImpl_Release( threading );
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, UINT16 *preferredLocalUdpMultiplayerPort )
{
    IXThreadingImpl *threading;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, preferredLocalUdpMultiplayerPort %p.\n", iface, asyncBlock, preferredLocalUdpMultiplayerPort );

    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( threading, asyncBlock, NULL, sizeof(UINT16), preferredLocalUdpMultiplayerPort, NULL );
    IXThreadingImpl_Release( threading );
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (!callback || !token) return E_POINTER;
    token->token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, token %p, wait %d.\n", iface, &token, wait );
    return TRUE;
}

static const char security_information_identity[] = "XNetworkingSecurityInformation";

static HRESULT WINAPI security_information_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    XNetworkingSecurityInformation *information;
    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(*information) );
            break;
        case XAsyncOp_GetResult:
            information = data->buffer;
            memset( information, 0, sizeof(*information) );
            /* Default HTTPS policy used by the community implementations.
             * WinHTTP still validates the server chain and hostname. No
             * title-specific pin policy has been loaded by this runtime. */
            information->enabledHttpSecurityProtocolFlags = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                                                           WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            break;
        default: break;
    }
    return S_OK;
}

static HRESULT begin_security_information( const WCHAR *url, XAsyncBlock *async )
{
    URL_COMPONENTS components = {sizeof(components)};
    WCHAR *https_url = NULL;
    SIZE_T length;
    BOOL valid;
    if (!url || !async) return E_INVALIDARG;
    /* WSS uses the same TLS endpoint/certificate policy as HTTPS. WinHTTP's
     * URL parser does not recognize the websocket scheme; normalize only
     * for parsing the policy query, not for issuing a request. */
    length = wcslen(url);
    if (length >= 6 && CompareStringOrdinal(url,6,L"wss://",6,TRUE) == CSTR_EQUAL)
    {
        if (length > (SIZE_T)-1 / sizeof(*https_url) - 3) return E_INVALIDARG;
        if (!(https_url = malloc((length + 3) * sizeof(*https_url)))) return E_OUTOFMEMORY;
        memcpy(https_url,L"https://",8 * sizeof(*https_url));
        memcpy(https_url + 8,url + 6,(length - 5) * sizeof(*https_url));
        url = https_url;
    }
    components.dwHostNameLength = 1;
    valid = WinHttpCrackUrl(url,0,0,&components) &&
            components.nScheme == INTERNET_SCHEME_HTTPS && components.dwHostNameLength;
    free(https_url);
    if (!valid)
    {
        return E_INVALIDARG;
    }
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL,
                                      security_information_identity, "XNetworkingSecurityInformation",
                                      security_information_provider );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsync( IXNetworkingImpl2 *iface, const char *url, XAsyncBlock *asyncBlock )
{
    WCHAR *wide;
    int length;
    HRESULT hr;
    TRACE( "iface %p, url %p, asyncBlock %p.\n", iface, url, asyncBlock );
    if (!url || !(length = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, NULL, 0 ))) return E_INVALIDARG;
    if (!(wide = malloc( length * sizeof(*wide) ))) return E_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wide, length );
    hr = begin_security_information( wide, asyncBlock );
    free( wide );
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, asyncBlock, securityInformationBufferByteCount );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    HRESULT hr;
    if (!securityInformation) return E_POINTER;
    *securityInformation = NULL;
    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, asyncBlock, security_information_identity,
                                        securityInformationBufferByteCount, securityInformationBuffer,
                                        securityInformationBufferByteCountUsed );
    if (SUCCEEDED(hr)) *securityInformation = (XNetworkingSecurityInformation *)securityInformationBuffer;
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async( IXNetworkingImpl2 *iface, const WCHAR *url, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, url %p, asyncBlock %p.\n", iface, url, asyncBlock );
    return begin_security_information( url, asyncBlock );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    return x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize( iface, asyncBlock, securityInformationBufferByteCount );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    return x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult( iface, asyncBlock,
        securityInformationBufferByteCount, securityInformationBufferByteCountUsed,
        securityInformationBuffer, securityInformation );
}

static BOOL security_pin_matches( PCCERT_CHAIN_CONTEXT chain, const XNetworkingThumbprint *pin )
{
    ALG_ID algorithm;
    DWORD length;
    BYTE digest[32];
    if (!pin->thumbprintBuffer || pin->thumbprintType < XNetworkingThumbprintType_Leaf ||
        pin->thumbprintType > XNetworkingThumbprintType_Root) return FALSE;
    if (pin->thumbprintBufferByteCount == 20) algorithm = CALG_SHA1;
    else if (pin->thumbprintBufferByteCount == 32) algorithm = CALG_SHA_256;
    else return FALSE;
    for (DWORD i = 0; i < chain->cChain; ++i)
    {
        PCERT_SIMPLE_CHAIN simple = chain->rgpChain[i];
        DWORD index;
        PCCERT_CONTEXT certificate;
        if (!simple->cElement) continue;
        if (pin->thumbprintType == XNetworkingThumbprintType_Leaf) index = 0;
        else if (pin->thumbprintType == XNetworkingThumbprintType_Root) index = simple->cElement - 1;
        else
        {
            if (simple->cElement < 2) continue;
            index = 1;
        }
        certificate = simple->rgpElement[index]->pCertContext;
        length = sizeof(digest);
        if (CryptHashCertificate( 0, algorithm, 0, certificate->pbCertEncoded,
                                  certificate->cbCertEncoded, digest, &length ) &&
            length == pin->thumbprintBufferByteCount && !memcmp( digest, pin->thumbprintBuffer, length )) return TRUE;
    }
    return FALSE;
}

static HRESULT WINAPI x_networking_XNetworkingVerifyServerCertificate( IXNetworkingImpl2 *iface, void *requestHandle, const XNetworkingSecurityInformation *securityInformation )
{
    PCCERT_CONTEXT certificate = NULL;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    CERT_CHAIN_PARA chain_parameters = {sizeof(chain_parameters)};
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = {{sizeof(ssl)}, AUTHTYPE_SERVER, 0, NULL};
    CERT_CHAIN_POLICY_PARA parameters = {sizeof(parameters), 0, &ssl};
    CERT_CHAIN_POLICY_STATUS status = {sizeof(status)};
    URL_COMPONENTS components = {sizeof(components)};
    DWORD flags, length = sizeof(flags);
    WCHAR *url = NULL, *host = NULL;
    BOOL match = FALSE;
    HRESULT hr = HRESULT_FROM_WIN32( ERROR_WINHTTP_SECURE_FAILURE );
    TRACE( "iface %p, requestHandle %p, securityInformation %p.\n", iface, requestHandle, securityInformation );
    if (!requestHandle || !securityInformation || securityInformation->thumbprintCount > 128 ||
        (securityInformation->thumbprintCount && !securityInformation->thumbprints)) return E_INVALIDARG;
    /* Never approve an explicitly weakened WinHTTP connection. */
    if (!WinHttpQueryOption( requestHandle, WINHTTP_OPTION_SECURITY_FLAGS, &flags, &length )) goto win32_error;
    if (flags & (SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                 SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE)) goto done;
    length = sizeof(certificate);
    if (!WinHttpQueryOption( requestHandle, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &length )) goto win32_error;
    length = 0;
    if (WinHttpQueryOption( requestHandle, WINHTTP_OPTION_URL, NULL, &length ) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || length > 65536 || !length) goto win32_error;
    if (!(url = malloc( length ))) { hr = E_OUTOFMEMORY; goto done; }
    if (!WinHttpQueryOption( requestHandle, WINHTTP_OPTION_URL, url, &length )) goto win32_error;
    components.dwHostNameLength = (DWORD)-1;
    if (!WinHttpCrackUrl( url, 0, 0, &components )) goto win32_error;
    if (components.nScheme != INTERNET_SCHEME_HTTPS || !components.dwHostNameLength) goto done;
    if (!(host = malloc( (components.dwHostNameLength + 1) * sizeof(*host) ))) { hr = E_OUTOFMEMORY; goto done; }
    memcpy( host, components.lpszHostName, components.dwHostNameLength * sizeof(*host) );
    host[components.dwHostNameLength] = 0;
    ssl.pwszServerName = host;
    if (!CertGetCertificateChain( NULL, certificate, NULL, certificate->hCertStore, &chain_parameters,
                                  0, NULL, &chain )) goto win32_error;
    if (!CertVerifyCertificateChainPolicy( CERT_CHAIN_POLICY_SSL, chain, &parameters, &status )) goto win32_error;
    if (status.dwError) { hr = status.dwError; goto done; }
    if (securityInformation->thumbprintCount)
    {
        for (SIZE_T i = 0; i < securityInformation->thumbprintCount; ++i)
            if (security_pin_matches( chain, &securityInformation->thumbprints[i] )) { match = TRUE; break; }
        if (!match) goto done;
    }
    hr = S_OK;
    goto done;
win32_error:
    hr = HRESULT_FROM_WIN32( GetLastError() );
    if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32( ERROR_WINHTTP_SECURE_FAILURE );
done:
    if (chain) CertFreeCertificateChain( chain );
    if (certificate) CertFreeCertificateContext( certificate );
    free( host );
    free( url );
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingGetConnectivityHint( IXNetworkingImpl2 *iface, XNetworkingConnectivityHint *connectivityHint )
{
    TRACE( "iface %p, connectivityHint %p.\n", iface, connectivityHint );
    if (!connectivityHint) return E_POINTER;
    connectivityHint->connectivityLevel = XNetworkingConnectivityLevelHint_InternetAccess;
    connectivityHint->connectivityCost = XNetworkingConnectivityCostHint_Unrestricted;
    connectivityHint->ianaInterfaceType = 6;
    connectivityHint->networkInitialized = TRUE;
    connectivityHint->approachingDataLimit = FALSE;
    connectivityHint->overDataLimit = FALSE;
    connectivityHint->roaming = FALSE;
    return S_OK;
}

struct connectivity_registration
{
    struct connectivity_registration *next;
    LONG ref;
    UINT64 token;
    XTaskQueueHandle queue;
    void *context;
    XNetworkingConnectivityHintChangedCallback *callback;
    BOOL removed, running;
    DWORD callback_thread;
};

static SRWLOCK connectivity_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE connectivity_changed = CONDITION_VARIABLE_INIT;
static struct connectivity_registration *connectivity_registrations;
static UINT64 connectivity_next_token;

static void connectivity_release( struct connectivity_registration *registration )
{
    if (!InterlockedDecrement( &registration->ref ))
    {
        IXThreadingImpl_XTaskQueueCloseHandle( x_threading_impl, registration->queue );
        free( registration );
    }
}

static void CALLBACK connectivity_initial_callback( void *context, BOOLEAN canceled )
{
    struct connectivity_registration *registration = context;
    XNetworkingConnectivityHint hint;
    BOOL deliver;

    AcquireSRWLockExclusive( &connectivity_lock );
    deliver = !canceled && !registration->removed;
    if (deliver)
    {
        registration->running = TRUE;
        registration->callback_thread = GetCurrentThreadId();
    }
    ReleaseSRWLockExclusive( &connectivity_lock );
    if (deliver)
    {
        x_networking_XNetworkingGetConnectivityHint( NULL, &hint );
        registration->callback( registration->context, &hint );
        AcquireSRWLockExclusive( &connectivity_lock );
        registration->running = FALSE;
        WakeAllConditionVariable( &connectivity_changed );
        ReleaseSRWLockExclusive( &connectivity_lock );
    }
    connectivity_release( registration );
}

static HRESULT WINAPI x_networking_XNetworkingRegisterConnectivityHintChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingConnectivityHintChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct connectivity_registration *registration;
    HRESULT hr;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (!callback || !token) return E_POINTER;
    token->token = 0;
    if (!(registration = calloc( 1, sizeof(*registration) ))) return E_OUTOFMEMORY;
    if (queue)
        hr = IXThreadingImpl_XTaskQueueDuplicateHandle( x_threading_impl, queue, &registration->queue );
    else
        hr = IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( x_threading_impl, &registration->queue ) ? S_OK : E_FAIL;
    if (FAILED(hr)) { free( registration ); return hr; }
    registration->context = context;
    registration->callback = callback;
    /* Local call, registration list and queued notification each own a ref.
     * An Immediate completion may unregister itself before Submit returns. */
    registration->ref = 3;
    AcquireSRWLockExclusive( &connectivity_lock );
    registration->token = token->token = ++connectivity_next_token;
    registration->next = connectivity_registrations;
    connectivity_registrations = registration;
    ReleaseSRWLockExclusive( &connectivity_lock );
    /* GDK guarantees an initial notification on the completion port. Without
     * it middleware can wait indefinitely before making its first request. */
    hr = IXThreadingImpl_XTaskQueueSubmitCallback( x_threading_impl, registration->queue,
            XTaskQueuePort_Completion, registration, connectivity_initial_callback );
    if (FAILED(hr))
    {
        struct connectivity_registration **cursor;
        BOOL removed = FALSE;
        AcquireSRWLockExclusive( &connectivity_lock );
        for (cursor = &connectivity_registrations; *cursor && *cursor != registration; cursor = &(*cursor)->next);
        if (*cursor) { *cursor = registration->next; registration->removed = TRUE; removed = TRUE; }
        ReleaseSRWLockExclusive( &connectivity_lock );
        if (removed) connectivity_release( registration ); /* list */
        connectivity_release( registration ); /* submission failed */
        token->token = 0;
    }
    connectivity_release( registration ); /* local */
    return hr;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterConnectivityHintChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct connectivity_registration **cursor, *registration;
    TRACE( "iface %p, token %p, wait %d.\n", iface, &token, wait );
    AcquireSRWLockExclusive( &connectivity_lock );
    for (cursor = &connectivity_registrations; *cursor && (*cursor)->token != token.token; cursor = &(*cursor)->next);
    if (!(registration = *cursor)) { ReleaseSRWLockExclusive( &connectivity_lock ); return FALSE; }
    *cursor = registration->next;
    registration->removed = TRUE;
    /* Queued notifications are suppressed, so a manual queue need not be
     * pumped to unregister. Wait only for a callback already in progress. */
    while (wait && registration->running && registration->callback_thread != GetCurrentThreadId())
        SleepConditionVariableSRW( &connectivity_changed, &connectivity_lock, INFINITE, 0 );
    ReleaseSRWLockExclusive( &connectivity_lock );
    connectivity_release( registration );
    return TRUE;
}

static HRESULT WINAPI x_networking_XNetworkingQueryConfigurationSetting( IXNetworkingImpl2 *iface, XNetworkingConfigurationSetting configurationSetting, UINT64 *value )
{
    FIXME( "iface %p, configurationSetting %d, value %p stub!\n", iface, configurationSetting, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_networking_XNetworkingSetConfigurationSetting( IXNetworkingImpl2 *iface, XNetworkingConfigurationSetting configurationParameter, UINT64 value )
{
    FIXME( "iface %p, configurationParameter %d, value %llu stub!\n", iface, configurationParameter, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_networking_XNetworkingQueryStatistics( IXNetworkingImpl2 *iface, XNetworkingStatisticsType statisticsType, XNetworkingStatisticsBuffer *statisticsBuffer )
{
    FIXME( "iface %p, statisticsType %d, statisticsBuffer %p stub!\n", iface, statisticsType, statisticsBuffer );
    return E_NOTIMPL;
}

static const struct IXNetworkingImpl2Vtbl x_networking_vtbl =
{
    x_networking_QueryInterface,
    x_networking_AddRef,
    x_networking_Release,
    /* IXNetworkingImpl methods */
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort,
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync,
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult,
    x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged,
    x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsync,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult,
    x_networking_XNetworkingVerifyServerCertificate,
    x_networking_XNetworkingGetConnectivityHint,
    x_networking_XNetworkingRegisterConnectivityHintChanged,
    x_networking_XNetworkingUnregisterConnectivityHintChanged,
    /* IXNetworkingImpl2 methods */
    x_networking_XNetworkingQueryConfigurationSetting,
    x_networking_XNetworkingSetConfigurationSetting,
    x_networking_XNetworkingQueryStatistics,
};

static struct x_networking x_networking =
{
    {&x_networking_vtbl},
    0,
};

IXNetworkingImpl *x_networking_impl = (IXNetworkingImpl *)&x_networking.IXNetworkingImpl2_iface;
