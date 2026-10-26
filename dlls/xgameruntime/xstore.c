/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XStore
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

#include <libxml/parser.h>
#include <libxml/tree.h>
#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_store
{
    IXStoreImpl6 IXStoreImpl6_iface;
    LONG ref;
};

static inline struct x_store *impl_from_IXStoreImpl6( IXStoreImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_store, IXStoreImpl6_iface );
}

static HRESULT WINAPI x_store_QueryInterface( IXStoreImpl6 *iface, REFIID iid, void **out )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown     ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl  ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl2 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl3 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl4 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl5 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl6 ))
    {
        IXStoreImpl6_AddRef( *out = &impl->IXStoreImpl6_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_store_AddRef( IXStoreImpl6 *iface )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_store_Release( IXStoreImpl6 *iface )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_store_XStoreCreateContext( IXStoreImpl6 *iface, const XUserHandle user, XStoreContextHandle *storeContextHandle )
{
    TRACE( "iface %p, user %p, storeContextHandle %p.\n", iface, user, storeContextHandle );
    if (!storeContextHandle) return E_POINTER;
    if (!(*storeContextHandle = calloc( 1, sizeof(LONG) ))) return E_OUTOFMEMORY;
    return S_OK;
}

static void WINAPI x_store_XStoreCloseContextHandle( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle )
{
    TRACE( "iface %p, storeContextHandle %p.\n", iface, storeContextHandle );
    free( storeContextHandle );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage %u, async %p stub!\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char **storeIds, SIZE_T storeIdsCount, const char **actionFilters, SIZE_T actionFiltersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, storeIds %p, storeIdsCount %Iu, actionFilters %p, actionFiltersCount %Iu, async %p stub!\n", iface, storeContextHandle, productKinds, storeIds, storeIdsCount, actionFilters, actionFiltersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage, %u, async %p stub!\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, packageIdentifier %s, async %p stub!\n", iface, storeContextHandle, productKinds, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreEnumerateProductsQuery( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, void *context, XStoreProductQueryCallback *callback )
{
    FIXME( "iface %p, productQueryHandle %p, context %p, callback %p stub!\n", iface, productQueryHandle, context, callback );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_store_XStoreProductsQueryHasMorePages( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle )
{
    FIXME( "iface %p, productQueryHandle %p stub!\n", iface, productQueryHandle );
    return FALSE;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, productQueryHandle %p, async %p stub!\n", iface, productQueryHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static void WINAPI x_store_XStoreCloseProductsQueryHandle( IXStoreImpl6 *iface, XStoreProductQueryHandle productQueryHandle )
{
    FIXME( "iface %p, productQueryHandle %p stub!\n", iface, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, productQueryHandle %p, packageIdentifier %s, async %p stub!\n", iface, productQueryHandle, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    FIXME( "iface %p, async %p, storeLicenseHandle %p stub!\n", iface, async, storeLicenseHandle );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_store_XStoreIsLicenseValid( IXStoreImpl6 *iface, const XStoreLicenseHandle storeLicenseHandle )
{
    FIXME( "iface %p, storeLicenseHandle %p stub!\n", iface, storeLicenseHandle );
    return FALSE;
}

static void WINAPI x_store_XStoreCloseLicenseHandle( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle )
{
    FIXME( "iface %p, storeLicenseHandle %p stub!\n", iface, storeLicenseHandle );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    FIXME( "iface %p, async %p, storeCanAcquireLicense %p stub!\n", iface, async, storeCanAcquireLicense );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifier %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    FIXME( "iface %p, async %p, storeCanAcquireLicense %p stub!\n", iface, async, storeCanAcquireLicense );
    return E_NOTIMPL;
}

static HRESULT WINAPI XStoreQueryGameLicenseProvider( XAsyncOp op, const XAsyncProviderData *data )
{
    XStoreGameLicense *license;
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "op %d, data %p.\n", op, data );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;

    switch (op)
    {
        case XAsyncOp_Begin:
            hr = IXThreadingImpl_XAsyncSchedule( xthreading, data->async, 0 );
            break;

        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( xthreading, data->async, S_OK, sizeof(XStoreGameLicense) );
            break;

        case XAsyncOp_GetResult:
            license = data->buffer;
            memset( license, 0, sizeof(*license) );
            if (storeId) snprintf( license->skuStoreId, sizeof(license->skuStoreId), "%s/0010", storeId );
            license->isActive = TRUE;
            license->expirationDate = 4102444800;
            break;

        case XAsyncOp_Cleanup:
        case XAsyncOp_Cancel:
            break;
    }

    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, storeContextHandle %p, async %p, storeId %s.\n", iface, storeContextHandle, async, debugstr_a( storeId ) );

    if (!storeContextHandle || !async) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, NULL, x_store_XStoreQueryGameLicenseAsync, "XStoreQueryGameLicenseAsync", XStoreQueryGameLicenseProvider );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreGameLicense *license )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, license %p.\n", iface, async, license );

    if (!async || !license) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, x_store_XStoreQueryGameLicenseAsync, sizeof(*license), license, NULL );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStoreAddonLicense *addOnLicenses )
{
    FIXME( "iface %p, async %p, count %u, addOnLicenses %p stub!\n", iface, async, count, addOnLicenses );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    FIXME( "iface %p, async %p, consumableResult %p stub!\n", iface, async, consumableResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, UINT32 quantity, GUID trackingId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, quantity %u, trackingId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), quantity, debugstr_guid( &trackingId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    FIXME( "iface %p, async %p, consumableResult %p stub!\n", iface, async, consumableResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, serviceTicket %s, userId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( serviceTicket ), debugstr_a( publisherUserId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    FIXME( "iface %p, async %p, size %p stub!\n", iface, async, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    FIXME( "iface %p, async %p, size %Iu, result %p stub!\n", iface, async, size, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, serviceTicket %s, publisherUserId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( serviceTicket ), debugstr_a( publisherUserId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    FIXME( "iface %p, async %p, size %p stub!\n", iface, async, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    FIXME( "iface %p, async %p, size %Iu, result %p stub!\n", iface, async, size, result );
    return E_NOTIMPL;
}

struct store_license_context
{
    char *request, *token;
    SIZE_T size;
};
static const char store_license_identity;

static HRESULT WINAPI store_license_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct store_license_context *context = data->context;
    IXThreadingImpl *threading;
    HRESULT hr;

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        hr = IXThreadingImpl_XAsyncSchedule( threading, data->async, 0 );
        break;
    case XAsyncOp_DoWork:
        hr = xodus_store_license_xml( context->request, strlen(context->request), &context->token );
        if (SUCCEEDED(hr)) context->size = strlen(context->token) + 1;
        /* Completion may synchronously consume/free context. */
        IXThreadingImpl_XAsyncComplete( threading, data->async, hr, SUCCEEDED(hr) ? context->size : 0 );
        hr = S_OK;
        break;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < context->size) hr = HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        else { memcpy( data->buffer, context->token, context->size ); hr = S_OK; }
        break;
    case XAsyncOp_Cleanup:
        if (context->token) { SecureZeroMemory( context->token, context->size ); free(context->token); }
        free(context->request);
        free(context);
        hr = S_OK;
        break;
    case XAsyncOp_Cancel:
        hr = S_OK;
        break;
    default:
        hr = E_NOTIMPL;
    }
    IXThreadingImpl_Release( threading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **productIds, SIZE_T productIdsCount, const char *customDeveloperString, XAsyncBlock *async )
{
    struct store_license_context *context;
    IXThreadingImpl *threading;
    HRESULT hr;
    SIZE_T i;

    TRACE( "iface %p, storeContextHandle %p, idsCount %Iu, async %p.\n", iface, storeContextHandle, productIdsCount, async );
    if (!storeContextHandle || !async || !productIds || !productIdsCount ||
        productIdsCount > 64 || !customDeveloperString || !*customDeveloperString) return E_INVALIDARG;
    for (i = 0; i < productIdsCount; i++) if (!productIds[i] || !*productIds[i]) return E_INVALIDARG;
    if (!(context = calloc( 1, sizeof(*context) ))) return E_OUTOFMEMORY;
    hr = xodus_store_license_body( productIds, productIdsCount, customDeveloperString, &context->request );
    if (FAILED(hr)) goto error;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) goto error;
    hr = IXThreadingImpl_XAsyncBegin( threading, async, context, &store_license_identity,
                                     "XStoreQueryLicenseTokenAsync", store_license_provider );
    xodus_store_license_note( "store-license-begin", hr );
    IXThreadingImpl_Release( threading );
    if (SUCCEEDED(hr)) return hr;
error:
    free(context->request);
    free(context);
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    IXThreadingImpl *threading;
    HRESULT hr;
    if (!async || !size) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResultSize( threading, async, size );
    xodus_store_license_note( "store-license-size", hr );
    IXThreadingImpl_Release( threading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    IXThreadingImpl *threading;
    HRESULT hr;
    if (!async || !result) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( threading, async, &store_license_identity, size, result, NULL );
    xodus_store_license_note( "store-license-result", hr );
    IXThreadingImpl_Release( threading );
    return hr;
}

static HRESULT WINAPI __PADDING__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_3__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreRateAndReviewResult *result )
{
    FIXME( "iface %p, async %p, result %p stub!\n", iface, async, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *token, const char **allowedStoreIds, SIZE_T allowedStoreIdsCount, BOOLEAN disallowCsvRedemption, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, token %s, allowedStoreIds %p, allowedStoreIdsCount %Iu, disallowCsvRedemption %d, async %p stub!\n", iface, storeContextHandle, debugstr_a( token ), allowedStoreIds, allowedStoreIdsCount, disallowCsvRedemption, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const char store_updates_identity[] = "XStorePackageUpdates";

struct store_updates_context
{
    LONG refs;
    char request[256];
    BOOL package_found;
    IXThreadingImpl *threading;
    XAsyncBlock *async;
};

static void store_updates_release( struct store_updates_context *context )
{
    if (!InterlockedDecrement(&context->refs))
    {
        IXThreadingImpl_Release(context->threading);
        free(context);
    }
}

static BOOLEAN WINAPI store_updates_package( void *arg, const XPackageDetails *details )
{
    struct store_updates_context *context = arg;
    SIZE_T i;
    if (!details->storeId || strlen(details->storeId) != 12 || context->package_found) return FALSE;
    for (i = 0; i < 12; ++i)
        if (!((details->storeId[i] >= 'A' && details->storeId[i] <= 'Z') ||
              (details->storeId[i] >= 'a' && details->storeId[i] <= 'z') ||
              (details->storeId[i] >= '0' && details->storeId[i] <= '9'))) return FALSE;
    snprintf(context->request,sizeof(context->request),
        "<Request><ProductId>%s</ProductId><InstalledVersion>%u.%u.%u.%u</InstalledVersion></Request>",
        details->storeId,details->version.major,details->version.minor,details->version.build,details->version.revision);
    context->package_found = TRUE;
    return FALSE; /* One installed package is exposed by this runtime. */
}

static xmlNodePtr store_updates_child( xmlNodePtr root, const char *name )
{
    xmlNodePtr child;
    for (child = root ? root->children : NULL; child; child = child->next)
        if (child->type == XML_ELEMENT_NODE && !strcmp((char *)child->name,name)) return child;
    return NULL;
}

static HRESULT store_updates_response( const char *response, UINT16 size )
{
    xmlDocPtr document;
    xmlNodePtr root;
    xmlChar *value;
    char *end;
    HRESULT hr = E_FAIL;
    if (!(document = xmlReadMemory(response,size,NULL,NULL,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING))) return E_FAIL;
    root = xmlDocGetRootElement(document);
    if (!(value = xmlNodeGetContent(store_updates_child(root,"HResult")))) goto done;
    hr = strtoul((char *)value,&end,10);
    if (end == (char *)value || *end) hr = E_FAIL;
    xmlFree(value);
    if (FAILED(hr)) goto done;
    hr = E_FAIL;
    if (!(value = xmlNodeGetContent(store_updates_child(root,"Checked")))) goto done;
    if (strcmp((char *)value,"true")) { xmlFree(value); goto done; }
    xmlFree(value);
    if (!(value = xmlNodeGetContent(store_updates_child(root,"Count")))) goto done;
    if (!strcmp((char *)value,"0")) hr = S_OK;
    xmlFree(value);
done:
    xmlFreeDoc(document);
    return hr;
}

static DWORD WINAPI XStorePackageUpdatesWorker( void *arg )
{
    struct store_updates_context *context = arg;
    IXThreadingImpl *threading = context->threading;
    XAsyncBlock *async = context->async;
    char *response = NULL;
    UINT16 size = 0;
    HRESULT hr = xodus_store_package_updates_xml(context->request,strlen(context->request),&response,&size);
    if (SUCCEEDED(hr)) hr = response && size ? store_updates_response(response,size) : E_FAIL;
    free(response);
    TRACE("package update query completed, hr=%08lx.\n",hr);
    store_updates_release(context);
    /* Completion can synchronously clean context: do not touch it afterward. */
    IXThreadingImpl_XAsyncComplete(threading,async,hr,0);
    IXThreadingImpl_Release(threading);
    return 0;
}

static HRESULT WINAPI store_updates_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct store_updates_context *context = data->context;
    HANDLE worker;
    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule(context->threading,data->async,0);
        case XAsyncOp_DoWork:
            context->async = data->async;
            InterlockedIncrement(&context->refs);
            IXThreadingImpl_AddRef(context->threading);
            if (!(worker = CreateThread(NULL,0,XStorePackageUpdatesWorker,context,0,NULL)))
            {
                HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
                IXThreadingImpl_Release(context->threading);
                store_updates_release(context);
                return hr;
            }
            CloseHandle(worker);
            return E_PENDING;
        case XAsyncOp_Cleanup:
            store_updates_release(context);
            break;
        default: break;
    }
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    struct store_updates_context *context;
    IXPackageImpl *package;
    HRESULT hr;
    TRACE("iface %p, storeContextHandle %p, async %p.\n",iface,storeContextHandle,async);
    if (!storeContextHandle || !async) return E_INVALIDARG;
    if (!(context = calloc(1,sizeof(*context)))) return E_OUTOFMEMORY;
    context->refs = 1;
    if (FAILED(hr = QueryApiImpl(&CLSID_XPackageImpl,&IID_IXPackageImpl,(void **)&package))) goto error;
    hr = IXPackageImpl_XPackageEnumeratePackages(package,XPackageKind_Game,XPackageEnumerationScope_ThisOnly,context,store_updates_package);
    IXPackageImpl_Release(package);
    if (FAILED(hr)) goto error;
    if (!context->package_found) { hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); goto error; }
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&context->threading))) goto error;
    hr = IXThreadingImpl_XAsyncBegin(context->threading,async,context,store_updates_identity,"XStoreQueryGameAndDlcPackageUpdatesAsync",store_updates_provider);
    if (SUCCEEDED(hr)) return hr;
    IXThreadingImpl_Release(context->threading);
error:
    free(context);
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    IXThreadingImpl *threading;
    SIZE_T size;
    HRESULT hr;
    TRACE("iface %p, async %p, count %p.\n",iface,async,count);
    if (!async || !count) return E_INVALIDARG;
    *count = 0;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&threading))) return hr;
    hr = IXThreadingImpl_XAsyncGetResultSize(threading,async,&size);
    IXThreadingImpl_Release(threading);
    if (SUCCEEDED(hr) && size) return E_UNEXPECTED;
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    IXThreadingImpl *threading;
    HRESULT hr;
    if (!async || count) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&threading))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult(threading,async,store_updates_identity,0,NULL,NULL);
    IXThreadingImpl_Release(threading);
    return hr;
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **storeIds, SIZE_T storeIdsCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeIds %p, storeIdsCount %Iu, async %p stub!\n", iface, storeContextHandle, storeIds, storeIdsCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, char **packageIdentifiers )
{
    FIXME( "iface %p, async %p, count %u, packageIdentifiers %p stub!\n", iface, async, count, packageIdentifiers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageIdentifier( IXStoreImpl6 *iface, const char *storeId, SIZE_T size, char *packageIdentifier )
{
    FIXME( "iface %p, storeId %s, size %Iu, packageIdentifier %p stub!\n", iface, debugstr_a( storeId ), size, packageIdentifier );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreRegisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueHandle queue, void *context, XStoreGameLicenseChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, storeContextHandle %p, queue %p, context %p, callback %p, token %p stub!\n", iface, storeContextHandle, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, storeContextHandle %p, token %p, wait %d stub!\n", iface, storeContextHandle, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_store_XStoreRegisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle, XTaskQueueHandle queue, void *context, XStorePackageLicenseLostCallback *callback, XTaskQueueRegistrationToken *token)
{
    FIXME( "iface %p, storeLicenseHandle %p, queue %p, context %p, callback %p, token %p stub!\n", iface, storeLicenseHandle, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle licenseHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, licenseHandle %p, token %p, wait %d stub!\n", iface, licenseHandle, &token, wait );
    return FALSE;
}

static BOOLEAN WINAPI x_store_XStoreIsAvailabilityPurchasable( IXStoreImpl6 *iface, const XStoreAvailability availability )
{
    FIXME( "iface %p, availability %p stub!\n", iface, &availability );
    return FALSE;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    FIXME( "iface %p, async %p, storeLicenseHandle %p stub!\n", iface, async, storeLicenseHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XStoreProductKind productKinds, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, maxItemsToRetrievePerPage %u, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, maxItemsToRetrievePerPage, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    FIXME( "iface %p, async %p, count %u, packageUpdates %p stub!\n", iface, async, count, packageUpdates );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXStoreImpl6Vtbl x_store_vtbl =
{
    x_store_QueryInterface,
    x_store_AddRef,
    x_store_Release,
    /* IXStoreImpl methods */
    x_store_XStoreCreateContext,
    x_store_XStoreCloseContextHandle,
    x_store_XStoreQueryAssociatedProductsAsync,
    x_store_XStoreQueryAssociatedProductsResult,
    x_store_XStoreQueryProductsAsync,
    x_store_XStoreQueryProductsResult,
    x_store_XStoreQueryEntitledProductsAsync,
    x_store_XStoreQueryEntitledProductsResult,
    x_store_XStoreQueryProductForCurrentGameAsync,
    x_store_XStoreQueryProductForCurrentGameResult,
    x_store_XStoreQueryProductForPackageAsync,
    x_store_XStoreQueryProductForPackageResult,
    x_store_XStoreEnumerateProductsQuery,
    x_store_XStoreProductsQueryHasMorePages,
    x_store_XStoreProductsQueryNextPageAsync,
    x_store_XStoreProductsQueryNextPageResult,
    x_store_XStoreCloseProductsQueryHandle,
    x_store_XStoreAcquireLicenseForPackageAsync,
    x_store_XStoreAcquireLicenseForPackageResult,
    x_store_XStoreIsLicenseValid,
    x_store_XStoreCloseLicenseHandle,
    x_store_XStoreCanAcquireLicenseForStoreIdAsync,
    x_store_XStoreCanAcquireLicenseForStoreIdResult,
    x_store_XStoreCanAcquireLicenseForPackageAsync,
    x_store_XStoreCanAcquireLicenseForPackageResult,
    x_store_XStoreQueryGameLicenseAsync,
    x_store_XStoreQueryGameLicenseResult,
    x_store_XStoreQueryAddOnLicensesAsync,
    x_store_XStoreQueryAddOnLicensesResultCount,
    x_store_XStoreQueryAddOnLicensesResult,
    x_store_XStoreQueryConsumableBalanceRemainingAsync,
    x_store_XStoreQueryConsumableBalanceRemainingResult,
    x_store_XStoreReportConsumableFulfillmentAsync,
    x_store_XStoreReportConsumableFulfillmentResult,
    x_store_XStoreGetUserCollectionsIdAsync,
    x_store_XStoreGetUserCollectionsIdResultSize,
    x_store_XStoreGetUserCollectionsIdResult,
    x_store_XStoreGetUserPurchaseIdAsync,
    x_store_XStoreGetUserPurchaseIdResultSize,
    x_store_XStoreGetUserPurchaseIdResult,
    x_store_XStoreQueryLicenseTokenAsync,
    x_store_XStoreQueryLicenseTokenResultSize,
    x_store_XStoreQueryLicenseTokenResult,
    __PADDING__,
    __PADDING_2__,
    __PADDING_3__,
    x_store_XStoreShowPurchaseUIAsync,
    x_store_XStoreShowPurchaseUIResult,
    x_store_XStoreShowRateAndReviewUIAsync,
    x_store_XStoreShowRateAndReviewUIResult,
    x_store_XStoreShowRedeemTokenUIAsync,
    x_store_XStoreShowRedeemTokenUIResult,
    x_store_XStoreQueryGameAndDlcPackageUpdatesAsync,
    x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount,
    x_store_XStoreQueryGameAndDlcPackageUpdatesResult,
    x_store_XStoreDownloadPackageUpdatesAsync,
    x_store_XStoreDownloadPackageUpdatesResult,
    x_store_XStoreDownloadAndInstallPackageUpdatesAsync,
    x_store_XStoreDownloadAndInstallPackageUpdatesResult,
    x_store_XStoreDownloadAndInstallPackagesAsync,
    x_store_XStoreDownloadAndInstallPackagesResultCount,
    x_store_XStoreDownloadAndInstallPackagesResult,
    x_store_XStoreQueryPackageIdentifier,
    x_store_XStoreRegisterGameLicenseChanged,
    x_store_XStoreUnregisterGameLicenseChanged,
    x_store_XStoreRegisterPackageLicenseLost,
    x_store_XStoreUnregisterPackageLicenseLost,
    /* IXStoreImpl2 methods */
    x_store_XStoreIsAvailabilityPurchasable,
    /* IXStoreImpl3 methods */
    x_store_XStoreAcquireLicenseForDurablesAsync,
    x_store_XStoreAcquireLicenseForDurablesResult,
    /* IXStoreImpl4 methods */
    x_store_XStoreShowAssociatedProductsUIAsync,
    x_store_XStoreShowAssociatedProductsUIResult,
    x_store_XStoreShowProductPageUIAsync,
    x_store_XStoreShowProductPageUIResult,
    /* IXStoreImpl5 methods */
    x_store_XStoreQueryAssociatedProductsForStoreIdAsync,
    x_store_XStoreQueryAssociatedProductsForStoreIdResult,
    x_store_XStoreQueryPackageUpdatesAsync,
    x_store_XStoreQueryPackageUpdatesResultCount,
    x_store_XStoreQueryPackageUpdatesResult,
    /* IXStoreImpl6 methods */
    x_store_XStoreShowGiftingUIAsync,
    x_store_XStoreShowGiftingUIResult,
};

static struct x_store x_store =
{
    {&x_store_vtbl},
    0,
};

IXStoreImpl *x_store_impl = (IXStoreImpl *)&x_store.IXStoreImpl6_iface;
