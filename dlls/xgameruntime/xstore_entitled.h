/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Account-backed entitlement queries. No game-specific IDs or ownership grants.
 * Included by xstore.c, sharing the component's SDK ABI and debug channel. */
#include <stdint.h>
#include <time.h>

#define ENTITLED_MAX_PRODUCTS 257
#define ENTITLED_MAX_SKUS 16
static char entitled_related[256][13];
static UINT32 entitled_related_count;

static BOOL entitled_id( const char *text )
{
    UINT32 i;
    if (!text || strlen(text) != 12) return FALSE;
    for (i = 0; i < 12; i++)
        if (!((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= '0' && text[i] <= '9'))) return FALSE;
    return TRUE;
}

static BOOL entitled_sku_id( const char *text )
{
    UINT32 i;
    if (!text || strlen(text) != 4) return FALSE;
    for (i = 0; i < 4; i++)
        if (!((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= '0' && text[i] <= '9'))) return FALSE;
    return TRUE;
}

HRESULT xstore_configure( struct _xmlNode *root )
{
    xmlNodePtr node, child;
    char ids[256][13];
    UINT32 count = 0, i;
    for (node = root->children; node; node = node->next)
    {
        if (node->type != XML_ELEMENT_NODE || strcmp((char *)node->name, "RelatedProducts")) continue;
        for (child = node->children; child; child = child->next)
        {
            xmlChar *value;
            if (child->type != XML_ELEMENT_NODE || strcmp((char *)child->name, "RelatedProduct")) continue;
            value = xmlNodeGetContent(child);
            if (!entitled_id((char *)value)) { xmlFree(value); return E_INVALIDARG; }
            for (i = 0; i < count && strcmp(ids[i], (char *)value); i++);
            if (i == count)
            {
                if (count == 256) { xmlFree(value); return E_INVALIDARG; }
                memcpy(ids[count++], value, 13);
            }
            xmlFree(value);
        }
    }
    memcpy(entitled_related, ids, count * 13);
    entitled_related_count = count;
    return S_OK;
}

struct game_license_sku
{
    char id[5];
    BOOLEAN trial;
    INT64 end;
};
struct entitled_store_context
{
    UINT64 xuid, serial, license_version, license_refresh;
    UINT32 license_count;
    struct game_license_sku license_skus[ENTITLED_MAX_SKUS];
    struct entitled_store_context *next;
};
static SRWLOCK entitled_stores_lock = SRWLOCK_INIT;
static struct entitled_store_context *entitled_stores;
static UINT64 entitled_next_store_serial;
static LONG64 entitled_next_refresh;
static void game_license_close_store( UINT64 serial );

static HRESULT entitled_store_create( XUserHandle user, XStoreContextHandle *out )
{
    struct entitled_store_context *context;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(context = calloc(1, sizeof(*context)))) return E_OUTOFMEMORY;
    /* Do not borrow the caller's XUser pointer. The existing receipt transport
     * uses the service's authenticated Store account, not Xbox profile metadata. */
    context->xuid = user ? xuser_get_xuid(user) : 0;
    if (user && !context->xuid) { free(context); return E_INVALIDARG; }
    AcquireSRWLockExclusive(&entitled_stores_lock);
    if (!++entitled_next_store_serial) ++entitled_next_store_serial;
    context->serial = entitled_next_store_serial;
    context->next = entitled_stores; entitled_stores = context;
    ReleaseSRWLockExclusive(&entitled_stores_lock);
    *out = context;
    return S_OK;
}

static void entitled_store_close( XStoreContextHandle handle )
{
    struct entitled_store_context **link, *context;
    AcquireSRWLockExclusive(&entitled_stores_lock);
    for (link = &entitled_stores; *link && *link != handle; link = &(*link)->next);
    context = *link;
    if (context) *link = context->next;
    ReleaseSRWLockExclusive(&entitled_stores_lock);
    if (context) game_license_close_store(context->serial);
    free(context);
}

static HRESULT entitled_store_identity( XStoreContextHandle handle, UINT64 *xuid, UINT64 *serial )
{
    struct entitled_store_context *context;
    HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&entitled_stores_lock);
    for (context = entitled_stores; context && context != handle; context = context->next);
    if (context)
    {
        *xuid = context->xuid;
        if (serial) *serial = context->serial;
        hr = S_OK;
    }
    ReleaseSRWLockShared(&entitled_stores_lock);
    return hr;
}

static HRESULT entitled_store_user( XStoreContextHandle handle, UINT64 *xuid )
{
    return entitled_store_identity(handle, xuid, NULL);
}

struct entitled_snapshot
{
    LONG refs;
    UINT32 count;
    UINT64 valid_until;
    UINT64 proof_until; /* Only set after actual signed-receipt verification. */
    XStoreProduct *products;
};
struct entitled_page
{
    LONG refs;
    struct entitled_snapshot *snapshot;
    UINT32 offset, size;
    struct entitled_page *next;
};
static SRWLOCK entitled_pages_lock = SRWLOCK_INIT;
static struct entitled_page *entitled_pages;

static void entitled_snapshot_release( struct entitled_snapshot *snapshot )
{
    UINT32 i, j;
    if (InterlockedDecrement(&snapshot->refs)) return;
    for (i = 0; i < snapshot->count; i++)
    {
        XStoreProduct *p = &snapshot->products[i];
        free((char *)p->storeId); free((char *)p->title); free((char *)p->description); free((char *)p->language);
        free((char *)p->inAppOfferToken);
        for (j = 0; j < p->skusCount; j++)
        {
            free((char *)p->skus[j].skuId); free((char *)p->skus[j].title);
            free((char *)p->skus[j].description); free((char *)p->skus[j].language);
        }
        free(p->skus);
    }
    free(snapshot->products); free(snapshot);
}

static void entitled_page_release( struct entitled_page *page )
{
    if (InterlockedDecrement(&page->refs)) return;
    entitled_snapshot_release(page->snapshot); free(page);
}

static struct entitled_page *entitled_page_acquire( XStoreProductQueryHandle handle )
{
    struct entitled_page *page;
    AcquireSRWLockShared(&entitled_pages_lock);
    for (page = entitled_pages; page && page != handle; page = page->next);
    if (page) InterlockedIncrement(&page->refs);
    ReleaseSRWLockShared(&entitled_pages_lock);
    return page;
}

static void entitled_page_close( XStoreProductQueryHandle handle )
{
    struct entitled_page *page, **link;
    AcquireSRWLockExclusive(&entitled_pages_lock);
    for (link = &entitled_pages; *link && *link != handle; link = &(*link)->next);
    page = *link;
    if (page) *link = page->next;
    ReleaseSRWLockExclusive(&entitled_pages_lock);
    if (page) entitled_page_release(page);
}

static BOOL entitled_page_valid( struct entitled_page *page )
{
    return (UINT64)time(NULL) < page->snapshot->valid_until;
}

static xmlNodePtr entitled_child( xmlNodePtr root, const char *name )
{
    xmlNodePtr child, result = NULL;
    for (child = root ? root->children : NULL; child; child = child->next)
        if (child->type == XML_ELEMENT_NODE && !strcmp((char *)child->name, name))
        { if (result) return NULL; result = child; }
    return result;
}

static char *entitled_text( xmlNodePtr root, const char *name )
{
    xmlNodePtr node = entitled_child(root, name);
    xmlChar *value;
    char *result;
    if (!node || !(value = xmlNodeGetContent(node))) return NULL;
    result = strdup((char *)value); xmlFree(value);
    return result;
}

static HRESULT entitled_uint( xmlNodePtr root, const char *name, UINT64 *out )
{
    char *text = entitled_text(root, name), *end;
    UINT64 value;
    SIZE_T i;
    if (!text) return E_UNEXPECTED;
    for (i = 0; text[i] >= '0' && text[i] <= '9'; i++);
    if (!i || text[i] || i > 19) { free(text); return E_UNEXPECTED; }
    value = strtoull(text, &end, 10);
    if (*end) { free(text); return E_UNEXPECTED; }
    free(text); *out = value; return S_OK;
}

static HRESULT entitled_bool( xmlNodePtr root, const char *name, BOOLEAN *out )
{
    char *value = entitled_text(root, name);
    HRESULT hr = E_UNEXPECTED;
    if (value && !strcmp(value, "true")) { *out = TRUE; hr = S_OK; }
    else if (value && !strcmp(value, "false")) { *out = FALSE; hr = S_OK; }
    free(value); return hr;
}

static HRESULT entitled_parse( const char *response, UINT16 size, struct entitled_snapshot **out )
{
    xmlDocPtr doc;
    xmlNodePtr root, node, sku_node;
    struct entitled_snapshot *snapshot = NULL;
    UINT64 value;
    UINT32 i = 0, j, k;
    HRESULT hr = E_UNEXPECTED;
    *out = NULL;
    if (!response || !size || strstr(response, "<!DOCTYPE") || strstr(response, "<!ENTITY")) return hr;
    doc = xmlReadMemory(response, size, NULL, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return hr;
    root = xmlDocGetRootElement(doc);
    if (doc->intSubset || doc->extSubset || !root || strcmp((char *)root->name, "Response")) goto done;
    if (FAILED(hr = entitled_uint(root, "HResult", &value))) goto done;
    if (value > UINT32_MAX) { hr = E_UNEXPECTED; goto done; }
    if (FAILED(hr = (HRESULT)value)) goto done;
    if (!(snapshot = calloc(1, sizeof(*snapshot)))) { hr = E_OUTOFMEMORY; goto done; }
    snapshot->refs = 1;
    if (FAILED(hr = entitled_uint(root, "ValidUntil", &snapshot->valid_until))) goto done;
    if (snapshot->valid_until <= (UINT64)time(NULL) || snapshot->valid_until > (UINT64)time(NULL) + 65)
    { hr = E_ACCESSDENIED; goto done; }
    for (node = root->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && !strcmp((char *)node->name, "Product")) snapshot->count++;
    if (snapshot->count > ENTITLED_MAX_PRODUCTS) { snapshot->count = 0; hr = E_UNEXPECTED; goto done; }
    if (snapshot->count && !(snapshot->products = calloc(snapshot->count, sizeof(*snapshot->products))))
    { snapshot->count = 0; hr = E_OUTOFMEMORY; goto done; }
    for (node = root->children; node; node = node->next)
    {
        XStoreProduct *p;
        if (node->type != XML_ELEMENT_NODE || strcmp((char *)node->name, "Product")) continue;
        p = &snapshot->products[i++];
        p->storeId = entitled_text(node, "StoreId"); p->title = entitled_text(node, "Title");
        p->description = entitled_text(node, "Description"); p->language = entitled_text(node, "Language");
        p->inAppOfferToken = entitled_text(node, "InAppOfferToken");
        if (!p->storeId || !p->title || !p->description || !p->language || !p->inAppOfferToken) { hr = E_UNEXPECTED; goto done; }
        if (!entitled_id(p->storeId)) { hr = E_UNEXPECTED; goto done; }
        for (k = 0; k + 1 < i; k++) if (!strcmp(p->storeId, snapshot->products[k].storeId))
        { hr = E_UNEXPECTED; goto done; }
        if (FAILED(hr = entitled_uint(node, "ProductKind", &value))) goto done;
        if (!value || value & ~31 || value & (value - 1)) { hr = E_UNEXPECTED; goto done; }
        p->productKind = value;
        if (FAILED(hr = entitled_bool(node, "HasDigitalDownload", &p->hasDigitalDownload)) ||
            FAILED(hr = entitled_bool(node, "IsInUserCollection", &p->isInUserCollection))) goto done;
        if (!p->isInUserCollection) { hr = E_UNEXPECTED; goto done; }
        for (sku_node = node->children; sku_node; sku_node = sku_node->next)
            if (sku_node->type == XML_ELEMENT_NODE && !strcmp((char *)sku_node->name, "Sku")) p->skusCount++;
        if (!p->skusCount || p->skusCount > ENTITLED_MAX_SKUS) { p->skusCount = 0; hr = E_UNEXPECTED; goto done; }
        if (!(p->skus = calloc(p->skusCount, sizeof(*p->skus)))) { p->skusCount = 0; hr = E_OUTOFMEMORY; goto done; }
        j = 0;
        for (sku_node = node->children; sku_node; sku_node = sku_node->next)
        {
            XStoreSku *s;
            if (sku_node->type != XML_ELEMENT_NODE || strcmp((char *)sku_node->name, "Sku")) continue;
            s = &p->skus[j++]; s->skuId = entitled_text(sku_node, "SkuId");
            s->title = entitled_text(sku_node, "Title"); s->description = entitled_text(sku_node, "Description");
            s->language = entitled_text(sku_node, "Language");
            if (!entitled_sku_id(s->skuId) || !s->title || !s->description || !s->language)
            { hr = E_UNEXPECTED; goto done; }
            for (k = 0; k + 1 < j; k++) if (!strcmp(s->skuId, p->skus[k].skuId))
            { hr = E_UNEXPECTED; goto done; }
            if (FAILED(hr = entitled_bool(sku_node, "IsTrial", &s->isTrial)) ||
                FAILED(hr = entitled_bool(sku_node, "IsInUserCollection", &s->isInUserCollection))) goto done;
            if (!s->isInUserCollection) { hr = E_UNEXPECTED; goto done; }
            if (FAILED(hr = entitled_uint(sku_node, "EndDate", &value))) goto done;
            if (value <= (UINT64)time(NULL) || value > INT64_MAX) { hr = E_ACCESSDENIED; goto done; }
            s->collectionData.endDate = value;
            s->collectionData.isTrial = s->isTrial;
        }
    }
    *out = snapshot; snapshot = NULL; hr = S_OK;
done:
    if (snapshot) entitled_snapshot_release(snapshot);
    xmlFreeDoc(doc); return hr;
}

struct entitled_async
{
    LONG refs, canceled, started;
    UINT64 store_serial, refresh;
    char *request;
    UINT32 page_size;
    struct entitled_page *page;
    IXThreadingImpl *threading;
    XAsyncBlock *async;
};
static const char entitled_identity;
static const char entitled_next_identity;

#include "xstore_receipts.h"
#include "xstore_game_license.h"

static void entitled_async_release( struct entitled_async *context )
{
    if (InterlockedDecrement(&context->refs)) return;
    if (context->page) entitled_page_release(context->page);
    free(context->request); IXThreadingImpl_Release(context->threading); free(context);
}

static DWORD WINAPI entitled_worker( void *arg )
{
    struct entitled_async *context = arg;
    struct entitled_snapshot *snapshot = NULL;
    HRESULT hr = E_ABORT;
    if (!InterlockedCompareExchange(&context->canceled, 0, 0))
        hr = entitled_fetch(context->request, &context->canceled, &snapshot);
    if (SUCCEEDED(hr))
    {
        if (!InterlockedCompareExchange(&context->canceled, 0, 0))
            game_license_observe(context->store_serial, context->refresh, snapshot);
        if (!(context->page = calloc(1, sizeof(*context->page)))) hr = E_OUTOFMEMORY;
        else
        {
            context->page->refs = 1; context->page->snapshot = snapshot;
            context->page->size = context->page_size; snapshot = NULL;
        }
    }
    if (snapshot) entitled_snapshot_release(snapshot);
    if (InterlockedCompareExchange(&context->canceled, 0, 0)) hr = E_ABORT;
    TRACE("entitled products completed hr=%08lx.\n", hr);
    xodus_store_license_note("store-entitled-result", hr);
    IXThreadingImpl_XAsyncComplete(context->threading, context->async, hr, SUCCEEDED(hr) ? sizeof(void *) : 0);
    entitled_async_release(context);
    return 0;
}

static HRESULT WINAPI entitled_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct entitled_async *context = data->context;
    HANDLE worker;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin: return IXThreadingImpl_XAsyncSchedule(context->threading, data->async, 0);
    case XAsyncOp_DoWork:
        if (InterlockedCompareExchange(&context->started, 1, 0)) return E_PENDING;
        if (InterlockedCompareExchange(&context->canceled, 0, 0)) return E_ABORT;
        if (context->page) /* Next page shares the same verified snapshot. */
        {
            hr = entitled_page_valid(context->page) ? S_OK : E_ACCESSDENIED;
            IXThreadingImpl_XAsyncComplete(context->threading, data->async, hr, SUCCEEDED(hr) ? sizeof(void *) : 0);
            return S_OK;
        }
        context->async = data->async;
        InterlockedIncrement(&context->refs);
        if (!(worker = CreateThread(NULL, 0, entitled_worker, context, 0, NULL)))
        { hr = HRESULT_FROM_WIN32(GetLastError()); entitled_async_release(context); return hr; }
        CloseHandle(worker);
        return E_PENDING;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(void *) || !context->page) return E_INVALIDARG;
        if (!entitled_page_valid(context->page)) return E_ACCESSDENIED;
        AcquireSRWLockExclusive(&entitled_pages_lock);
        context->page->next = entitled_pages; entitled_pages = context->page;
        ReleaseSRWLockExclusive(&entitled_pages_lock);
        memcpy(data->buffer, &context->page, sizeof(void *)); context->page = NULL;
        break;
    case XAsyncOp_Cancel: InterlockedExchange(&context->canceled, TRUE); break;
    case XAsyncOp_Cleanup: entitled_async_release(context); break;
    default: break;
    }
    return S_OK;
}

static HRESULT entitled_start( struct entitled_async *context, XAsyncBlock *async, BOOL next )
{
    HRESULT hr;
    context->refs = 1;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&context->threading)))
    { if (context->page) entitled_page_release(context->page); free(context->request); free(context); return hr; }
    /* Successful Begin transfers ownership, including provider Begin failure.
     * A failed Begin itself has not installed the provider context. */
    hr = IXThreadingImpl_XAsyncBegin(context->threading, async, context,
        next ? &entitled_next_identity : &entitled_identity, "XStoreEntitledProducts", entitled_provider);
    if (FAILED(hr)) entitled_async_release(context);
    return hr;
}

static HRESULT entitled_begin( XStoreContextHandle store, UINT32 kinds, UINT32 page_size, XAsyncBlock *async )
{
    struct entitled_async *context;
    xmlDocPtr doc;
    xmlNodePtr root;
    xmlChar *bytes = NULL;
    char kind[16], language[128] = "en-US", market[16] = "US";
    WCHAR locale[85], country[16];
    UINT64 xuid, serial;
    UINT32 i;
    int size;
    HRESULT hr;
    if (!async || !kinds || (kinds & ~31) || !page_size) return E_INVALIDARG;
    page_size = min(page_size, ENTITLED_MAX_PRODUCTS);
    if (FAILED(hr = entitled_store_identity(store, &xuid, &serial))) return hr;
    if (!entitled_id(storeId)) return E_GAME_MISSING_GAME_CONFIG;
    if (GetUserDefaultLocaleName(locale, ARRAY_SIZE(locale)))
        WideCharToMultiByte(CP_UTF8, 0, locale, -1, language, sizeof(language), NULL, NULL);
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, country, ARRAY_SIZE(country)))
        WideCharToMultiByte(CP_UTF8, 0, country, -1, market, sizeof(market), NULL, NULL);
    if (!(doc = xmlNewDoc(BAD_CAST "1.0"))) return E_OUTOFMEMORY;
    if (!(root = xmlNewNode(NULL, BAD_CAST "Request"))) { xmlFreeDoc(doc); return E_OUTOFMEMORY; }
    xmlDocSetRootElement(doc, root);
    snprintf(kind, sizeof(kind), "%u", kinds);
    xmlNewTextChild(root, NULL, BAD_CAST "ParentProductId", BAD_CAST storeId);
    for (i = 0; i < entitled_related_count; i++)
        if (strcmp(storeId, entitled_related[i])) xmlNewTextChild(root, NULL, BAD_CAST "ProductId", BAD_CAST entitled_related[i]);
    xmlNewTextChild(root, NULL, BAD_CAST "ProductKinds", BAD_CAST kind);
    xmlNewTextChild(root, NULL, BAD_CAST "Market", BAD_CAST market);
    xmlNewTextChild(root, NULL, BAD_CAST "Language", BAD_CAST language);
    xmlDocDumpMemory(doc, &bytes, &size); xmlFreeDoc(doc);
    if (!bytes) return E_OUTOFMEMORY;
    if (size <= 0 || size > 60000) { xmlFree(bytes); return E_INVALIDARG; }
    context = calloc(1, sizeof(*context));
    if (context) { context->request = strdup((char *)bytes); context->page_size = page_size; }
    xmlFree(bytes);
    if (!context) return E_OUTOFMEMORY;
    if (!context->request) { free(context); return E_OUTOFMEMORY; }
    context->store_serial = serial;
    context->refresh = InterlockedIncrement64(&entitled_next_refresh);
    return entitled_start(context, async, FALSE);
}

static HRESULT entitled_result( XAsyncBlock *async, XStoreProductQueryHandle *out, BOOL next )
{
    IXThreadingImpl *threading;
    HRESULT hr;
    if (!async || !out) return E_INVALIDARG;
    *out = NULL;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult(threading, async, next ? &entitled_next_identity : &entitled_identity,
        sizeof(void *), out, NULL);
    IXThreadingImpl_Release(threading); return hr;
}

static HRESULT entitled_enumerate( XStoreProductQueryHandle handle, void *arg, XStoreProductQueryCallback *callback )
{
    struct entitled_page *page;
    UINT32 i, end;
    if (!callback || !(page = entitled_page_acquire(handle))) return E_INVALIDARG;
    if (!entitled_page_valid(page)) { entitled_page_release(page); return E_ACCESSDENIED; }
    end = min(page->snapshot->count, page->offset + page->size);
    TRACE( "handle %p, count %u, offset %u, size %u.\n", handle, page->snapshot->count, page->offset, page->size );
    for (i = page->offset; i < end; i++)
    {
        TRACE( "  product %s kind %#x inCollection %d\n", debugstr_a( page->snapshot->products[i].storeId ), page->snapshot->products[i].productKind, page->snapshot->products[i].isInUserCollection );
        if (!callback(&page->snapshot->products[i], arg)) break;
    }
    entitled_page_release(page); return S_OK;
}

static BOOLEAN entitled_has_more( XStoreProductQueryHandle handle )
{
    struct entitled_page *page = entitled_page_acquire(handle);
    BOOLEAN more;
    if (!page) return FALSE;
    more = entitled_page_valid(page) && page->offset + page->size < page->snapshot->count;
    TRACE( "handle %p -> %d.\n", handle, more );
    entitled_page_release(page); return more;
}

static HRESULT entitled_next( XStoreProductQueryHandle handle, XAsyncBlock *async )
{
    struct entitled_page *page;
    struct entitled_async *context;
    if (!async || !(page = entitled_page_acquire(handle))) return E_INVALIDARG;
    if (!entitled_page_valid(page)) { entitled_page_release(page); return E_ACCESSDENIED; }
    if (page->offset + page->size >= page->snapshot->count)
    { entitled_page_release(page); return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS); }
    if (!(context = calloc(1, sizeof(*context)))) { entitled_page_release(page); return E_OUTOFMEMORY; }
    if (!(context->page = calloc(1, sizeof(*context->page))))
    { free(context); entitled_page_release(page); return E_OUTOFMEMORY; }
    context->page->refs = 1; context->page->snapshot = page->snapshot;
    InterlockedIncrement(&page->snapshot->refs);
    context->page->offset = page->offset + page->size; context->page->size = page->size;
    entitled_page_release(page);
    return entitled_start(context, async, TRUE);
}
