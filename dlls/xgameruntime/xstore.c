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

#include "xstore_entitled.h"
#include "xstore_package_license.h"

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
    return entitled_store_create(user, storeContextHandle);
}

static void WINAPI x_store_XStoreCloseContextHandle( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle )
{
    entitled_store_close(storeContextHandle);
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    /* Only owned add-ons are known: answer with the entitled subset of the
     * associated products instead of leaving titles waiting on the query. */
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage %u, async %p, using entitled products.\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );
    return entitled_begin(storeContextHandle, productKinds, maxItemsToRetrievePerPage, async);
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    return entitled_result(async, productQueryHandle, FALSE);
}

static const char store_products_identity[] = "XStoreQueryProducts";

/* Catalog metadata for products the game asks about by store id comes from
 * C:\ProgramData\MACxus\store-catalog.txt (written by the launcher), one
 * "storeId<TAB>kind<TAB>title" line each. Requested ids found there are
 * returned as catalog entries that are NOT in the user's collection, like the
 * Windows store does for unowned upsell offers; unknown ids are left out. */
struct store_products_query
{
    XStoreProductKind kinds;
    SIZE_T count;
    char ids[64][13];
};

static BOOL store_catalog_lookup( const char *id, XStoreProductKind *kind, char *title, SIZE_T title_size )
{
    char line[512], *tab1, *tab2, *eol;
    BOOL found = FALSE;
    FILE *file;

    if (!(file = fopen( "C:\\ProgramData\\MACxus\\store-catalog.txt", "r" ))) return FALSE;
    while (!found && fgets( line, sizeof(line), file ))
    {
        if ((eol = strpbrk( line, "\r\n" ))) *eol = 0;
        if (!(tab1 = strchr( line, '\t' )) || !(tab2 = strchr( tab1 + 1, '\t' ))) continue;
        *tab1 = *tab2 = 0;
        if (strcmp( line, id )) continue;
        *kind = strtoul( tab1 + 1, NULL, 0 );
        snprintf( title, title_size, "%s", tab2 + 1 );
        found = TRUE;
    }
    fclose( file );
    return found;
}

static struct entitled_snapshot *store_products_snapshot( const struct store_products_query *query )
{
    struct entitled_snapshot *snapshot;
    XStoreProductKind kind;
    char title[256];
    SIZE_T i;

    if (!(snapshot = calloc( 1, sizeof(*snapshot) ))) return NULL;
    snapshot->refs = 1;
    snapshot->valid_until = ~(UINT64)0;
    if (query && query->count && !(snapshot->products = calloc( query->count, sizeof(*snapshot->products) )))
    {
        free( snapshot );
        return NULL;
    }
    for (i = 0; query && i < query->count; i++)
    {
        XStoreProduct *product = &snapshot->products[snapshot->count];
        if (!store_catalog_lookup( query->ids[i], &kind, title, sizeof(title) ) || !(kind & query->kinds))
        {
            TRACE( "  %s not in catalog, skipped.\n", debugstr_a( query->ids[i] ) );
            continue;
        }
        product->storeId = strdup( query->ids[i] );
        product->title = strdup( title );
        product->description = strdup( "" );
        product->language = strdup( "" );
        product->inAppOfferToken = strdup( "" );
        product->productKind = kind;
        product->hasDigitalDownload = TRUE;
        product->isInUserCollection = FALSE;
        if ((product->skus = calloc( 1, sizeof(*product->skus) )))
        {
            product->skusCount = 1;
            product->skus[0].skuId = strdup( "0010" );
            product->skus[0].title = strdup( title );
            product->skus[0].description = strdup( "" );
            product->skus[0].language = strdup( "" );
        }
        TRACE( "  %s kind %#x %s, not owned.\n", debugstr_a( product->storeId ), kind, debugstr_a( title ) );
        snapshot->count++;
    }
    return snapshot;
}

static HRESULT WINAPI XStoreQueryProductsProvider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct entitled_snapshot *snapshot;
    struct entitled_page *page;
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
            IXThreadingImpl_XAsyncComplete( xthreading, data->async, S_OK, sizeof(void *) );
            hr = S_OK;
            break;

        case XAsyncOp_GetResult:
            if (!data->buffer || data->bufferSize < sizeof(void *)) { hr = E_INVALIDARG; break; }
            snapshot = store_products_snapshot( data->context );
            page = calloc( 1, sizeof(*page) );
            if (!snapshot || !page) { if (snapshot) entitled_snapshot_release( snapshot ); free( page ); hr = E_OUTOFMEMORY; break; }
            page->refs = 1;
            page->snapshot = snapshot;
            page->size = snapshot->count;
            AcquireSRWLockExclusive( &entitled_pages_lock );
            page->next = entitled_pages;
            entitled_pages = page;
            ReleaseSRWLockExclusive( &entitled_pages_lock );
            memcpy( data->buffer, &page, sizeof(void *) );
            hr = S_OK;
            break;

        case XAsyncOp_Cleanup:
            free( data->context );
            hr = S_OK;
            break;

        case XAsyncOp_Cancel:
            hr = S_OK;
            break;

        default:
            hr = E_NOTIMPL;
    }

    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char **storeIds, SIZE_T storeIdsCount, const char **actionFilters, SIZE_T actionFiltersCount, XAsyncBlock *async )
{
    struct store_products_query *query;
    IXThreadingImpl *xthreading;
    HRESULT hr;
    SIZE_T i;

    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, storeIds %p, storeIdsCount %Iu, actionFilters %p, actionFiltersCount %Iu, async %p.\n", iface, storeContextHandle, productKinds, storeIds, storeIdsCount, actionFilters, actionFiltersCount, async );

    if (!storeContextHandle || !async || (storeIdsCount && !storeIds)) return E_INVALIDARG;
    if (!(query = calloc( 1, sizeof(*query) ))) return E_OUTOFMEMORY;
    query->kinds = productKinds;
    for (i = 0; i < storeIdsCount && query->count < ARRAY_SIZE(query->ids); i++)
    {
        TRACE( "  storeIds[%Iu] %s\n", i, debugstr_a( storeIds[i] ) );
        if (entitled_id( storeIds[i] )) memcpy( query->ids[query->count++], storeIds[i], 13 );
    }
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) { free( query ); return hr; }
    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, query, store_products_identity, "XStoreQueryProductsAsync", XStoreQueryProductsProvider );
    if (FAILED(hr)) free( query );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );

    if (!async || !productQueryHandle) return E_INVALIDARG;
    *productQueryHandle = NULL;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, store_products_identity, sizeof(void *), productQueryHandle, NULL );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    return entitled_begin(storeContextHandle, productKinds, maxItemsToRetrievePerPage, async);
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    return entitled_result(async, productQueryHandle, FALSE);
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
    return entitled_enumerate(productQueryHandle, context, callback);
}

static BOOLEAN WINAPI x_store_XStoreProductsQueryHasMorePages( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle )
{
    return entitled_has_more(productQueryHandle);
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, XAsyncBlock *async )
{
    return entitled_next(productQueryHandle, async);
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    return entitled_result(async, productQueryHandle, TRUE);
}

static void WINAPI x_store_XStoreCloseProductsQueryHandle( IXStoreImpl6 *iface, XStoreProductQueryHandle productQueryHandle )
{
    entitled_page_close(productQueryHandle);
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    return package_license_begin((XStoreContextHandle)productQueryHandle, packageIdentifier, async);
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    return package_license_result(async, storeLicenseHandle);
}

static BOOLEAN WINAPI x_store_XStoreIsLicenseValid( IXStoreImpl6 *iface, const XStoreLicenseHandle storeLicenseHandle )
{
    return package_license_valid(storeLicenseHandle);
}

static void WINAPI x_store_XStoreCloseLicenseHandle( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle )
{
    package_license_close(storeLicenseHandle);
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

static const char store_addon_licenses_identity[] = "XStoreQueryAddOnLicenses";

/* No add-on licenses are known locally: complete successfully with an empty
 * result so titles waiting on this query (e.g. at "Start game") can proceed. */
static HRESULT WINAPI XStoreQueryAddOnLicensesProvider( XAsyncOp op, const XAsyncProviderData *data )
{
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
            IXThreadingImpl_XAsyncComplete( xthreading, data->async, S_OK, 0 );
            hr = S_OK;
            break;

        case XAsyncOp_GetResult:
        case XAsyncOp_Cleanup:
        case XAsyncOp_Cancel:
            hr = S_OK;
            break;

        default:
            hr = E_NOTIMPL;
    }

    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, storeContextHandle %p, async %p, returning no add-on licenses.\n", iface, storeContextHandle, async );

    if (!storeContextHandle || !async) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, NULL, store_addon_licenses_identity, "XStoreQueryAddOnLicensesAsync", XStoreQueryAddOnLicensesProvider );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    IXThreadingImpl *xthreading;
    SIZE_T size = 0;
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );

    if (!async || !count) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, async, &size );
    IXThreadingImpl_Release( xthreading );
    *count = SUCCEEDED(hr) ? size / sizeof(XStoreAddonLicense) : 0;
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStoreAddonLicense *addOnLicenses )
{
    IXThreadingImpl *xthreading;
    SIZE_T size = 0;
    HRESULT hr;

    TRACE( "iface %p, async %p, count %u, addOnLicenses %p.\n", iface, async, count, addOnLicenses );

    if (!async || (count && !addOnLicenses)) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    if (SUCCEEDED(hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, async, &size )))
    {
        /* The result never holds licenses; count must match ResultCount. */
        if (count != size / sizeof(XStoreAddonLicense)) hr = E_INVALIDARG;
        else hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, store_addon_licenses_identity,
                                                   count * sizeof(XStoreAddonLicense), addOnLicenses, NULL );
    }
    IXThreadingImpl_Release( xthreading );
    return hr;
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
    TRACE( "iface %p, storeId %s, size %Iu, packageIdentifier %p.\n", iface, debugstr_a( storeId ), size, packageIdentifier );
    return xpackage_query_identifier(storeId, size, packageIdentifier);
}

static HRESULT WINAPI x_store_XStoreRegisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueHandle queue, void *context, XStoreGameLicenseChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    return game_license_register(storeContextHandle, queue, context, callback, token);
}

static BOOLEAN WINAPI x_store_XStoreUnregisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    return game_license_unregister(storeContextHandle, token, wait);
}

static HRESULT WINAPI x_store_XStoreRegisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle, XTaskQueueHandle queue, void *context, XStorePackageLicenseLostCallback *callback, XTaskQueueRegistrationToken *token)
{
    return package_license_register(storeLicenseHandle, queue, context, callback, token);
}

static BOOLEAN WINAPI x_store_XStoreUnregisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle licenseHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    return package_license_unregister(licenseHandle, token, wait);
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
