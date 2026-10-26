/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Package licenses are proof-bound handles, not installed/owned flag stubs.
 * Included after xstore_entitled.h. No test trust overrides or title IDs. */
struct package_license
{
    LONG refs, closed, lost;
    UINT64 until;
    HANDLE stop;
    IXThreadingImpl *threading;
    struct package_license *next;
};
struct package_license_registration
{
    LONG refs;
    struct package_license_registration *next;
    struct package_license *license;
    XTaskQueueHandle queue;
    void *context;
    XStorePackageLicenseLostCallback *callback;
    UINT64 token;
    BOOL removed, notified, queued, running;
    DWORD callback_thread;
};
static SRWLOCK package_license_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE package_license_changed = CONDITION_VARIABLE_INIT;
static struct package_license *package_licenses;
static struct package_license_registration *package_license_registrations;
static UINT64 package_license_next_token;

static void package_license_release( struct package_license *license )
{
    if (InterlockedDecrement(&license->refs)) return;
    CloseHandle(license->stop);
    IXThreadingImpl_Release(license->threading);
    free(license);
}

static void package_license_dispose( struct package_license *license )
{
    InterlockedExchange(&license->closed, TRUE);
    SetEvent(license->stop);
    package_license_release(license);
}

static void package_license_registration_release( struct package_license_registration *registration )
{
    if (InterlockedDecrement(&registration->refs)) return;
    IXThreadingImpl_XTaskQueueCloseHandle(registration->license->threading, registration->queue);
    package_license_release(registration->license);
    free(registration);
}

/* Called with the registry lock held. Pending unregisters stay discoverable
 * until their callback drains, so a later wait=true can really wait. */
static BOOL package_license_retire_registration( struct package_license_registration *registration )
{
    struct package_license_registration **link;
    for (link = &package_license_registrations; *link && *link != registration; link = &(*link)->next);
    if (!*link) return FALSE;
    *link = registration->next;
    return TRUE;
}

static void CALLBACK package_license_lost_callback( void *arg, BOOLEAN canceled )
{
    struct package_license_registration *registration = arg;
    BOOL deliver, retired = FALSE;
    AcquireSRWLockExclusive(&package_license_lock);
    registration->queued = FALSE;
    deliver = !canceled && !registration->removed && !registration->license->closed;
    if (deliver)
    {
        registration->running = TRUE;
        registration->callback_thread = GetCurrentThreadId();
    }
    else if (registration->removed) retired = package_license_retire_registration(registration);
    ReleaseSRWLockExclusive(&package_license_lock);
    if (deliver)
    {
        registration->callback(registration->context);
        AcquireSRWLockExclusive(&package_license_lock);
        registration->running = FALSE;
        if (registration->removed) retired = package_license_retire_registration(registration);
        WakeAllConditionVariable(&package_license_changed);
        ReleaseSRWLockExclusive(&package_license_lock);
    }
    if (retired) package_license_registration_release(registration);
    package_license_registration_release(registration);
}

static void package_license_notify( struct package_license *license )
{
    struct package_license_registration *registration;
    HRESULT hr;
    for (;;)
    {
        AcquireSRWLockExclusive(&package_license_lock);
        for (registration = package_license_registrations; registration; registration = registration->next)
            if (registration->license == license && !registration->notified && !registration->removed) break;
        if (registration && !license->closed)
        {
            registration->notified = registration->queued = TRUE;
            InterlockedIncrement(&registration->refs);
        }
        else registration = NULL;
        ReleaseSRWLockExclusive(&package_license_lock);
        if (!registration) break;
        hr = IXThreadingImpl_XTaskQueueSubmitCallback(license->threading, registration->queue,
            XTaskQueuePort_Completion, registration, package_license_lost_callback);
        if (FAILED(hr))
        {
            AcquireSRWLockExclusive(&package_license_lock);
            registration->queued = FALSE;
            WakeAllConditionVariable(&package_license_changed);
            ReleaseSRWLockExclusive(&package_license_lock);
            package_license_registration_release(registration);
        }
    }
}

static DWORD WINAPI package_license_expiry_worker( void *arg )
{
    struct package_license *license = arg;
    UINT64 now, delay;
    for (;;)
    {
        now = time(NULL);
        if (now >= license->until)
        {
            InterlockedExchange(&license->lost, TRUE);
            package_license_notify(license);
            break;
        }
        delay = min(license->until - now, 60) * 1000;
        if (WaitForSingleObject(license->stop, (DWORD)delay) != WAIT_TIMEOUT) break;
    }
    package_license_release(license);
    return 0;
}

static HRESULT package_license_create( IXThreadingImpl *threading, UINT64 until, struct package_license **out )
{
    struct package_license *license;
    HANDLE thread;
    HRESULT hr;
    *out = NULL;
    if (until <= (UINT64)time(NULL)) return E_ACCESSDENIED;
    if (!(license = calloc(1, sizeof(*license)))) return E_OUTOFMEMORY;
    license->refs = 1; license->until = until;
    license->threading = threading; IXThreadingImpl_AddRef(threading);
    license->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!license->stop) { hr = HRESULT_FROM_WIN32(GetLastError()); package_license_release(license); return hr; }
    InterlockedIncrement(&license->refs);
    thread = CreateThread(NULL, 0, package_license_expiry_worker, license, 0, NULL);
    if (!thread)
    {
        hr = HRESULT_FROM_WIN32(GetLastError()); package_license_release(license); package_license_dispose(license); return hr;
    }
    CloseHandle(thread);
    *out = license;
    return S_OK;
}

static BOOL package_license_valid_object( struct package_license *license )
{
    return !InterlockedCompareExchange(&license->closed, 0, 0) &&
           !InterlockedCompareExchange(&license->lost, 0, 0) && (UINT64)time(NULL) < license->until;
}

static BOOLEAN package_license_valid( XStoreLicenseHandle handle )
{
    struct package_license *license;
    BOOL valid;
    AcquireSRWLockShared(&package_license_lock);
    for (license = package_licenses; license && license != handle; license = license->next);
    valid = license && package_license_valid_object(license);
    ReleaseSRWLockShared(&package_license_lock);
    return valid;
}

static void package_license_close( XStoreLicenseHandle handle )
{
    struct package_license **link, *license;
    struct package_license_registration **r_link, *registration;
    AcquireSRWLockExclusive(&package_license_lock);
    for (link = &package_licenses; *link && *link != handle; link = &(*link)->next);
    if (!(license = *link)) { ReleaseSRWLockExclusive(&package_license_lock); return; }
    *link = license->next;
    InterlockedExchange(&license->closed, TRUE);
    SetEvent(license->stop);
    for (;;)
    {
        for (r_link = &package_license_registrations; *r_link && (*r_link)->license != license; r_link = &(*r_link)->next);
        if (!(registration = *r_link)) break;
        *r_link = registration->next; registration->removed = TRUE;
        while (registration->running && registration->callback_thread != GetCurrentThreadId())
            SleepConditionVariableSRW(&package_license_changed, &package_license_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&package_license_lock);
        package_license_registration_release(registration);
        AcquireSRWLockExclusive(&package_license_lock);
    }
    ReleaseSRWLockExclusive(&package_license_lock);
    package_license_release(license);
}

static HRESULT package_license_register( XStoreLicenseHandle handle, XTaskQueueHandle queue, void *context,
    XStorePackageLicenseLostCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct package_license_registration *registration;
    struct package_license *license;
    HRESULT hr;
    if (!callback || !token) return E_POINTER;
    token->token = 0;
    if (!(registration = calloc(1, sizeof(*registration)))) return E_OUTOFMEMORY;
    AcquireSRWLockExclusive(&package_license_lock);
    for (license = package_licenses; license && license != handle; license = license->next);
    if (!license || !package_license_valid_object(license))
    { ReleaseSRWLockExclusive(&package_license_lock); free(registration); return license ? E_ACCESSDENIED : E_INVALIDARG; }
    if (queue) hr = IXThreadingImpl_XTaskQueueDuplicateHandle(license->threading, queue, &registration->queue);
    else hr = IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue(license->threading, &registration->queue) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        registration->refs = 1;
        registration->license = license; InterlockedIncrement(&license->refs);
        registration->context = context; registration->callback = callback;
        if (!++package_license_next_token) ++package_license_next_token;
        registration->token = token->token = package_license_next_token;
        registration->next = package_license_registrations; package_license_registrations = registration;
    }
    ReleaseSRWLockExclusive(&package_license_lock);
    if (FAILED(hr)) free(registration);
    return hr;
}

static BOOLEAN package_license_unregister( XStoreLicenseHandle handle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct package_license_registration **link, *registration;
    BOOL pending, retired = FALSE;
    AcquireSRWLockExclusive(&package_license_lock);
    for (link = &package_license_registrations; *link; link = &(*link)->next)
        if ((*link)->license == handle && (*link)->token == token.token) break;
    if (!(registration = *link)) { ReleaseSRWLockExclusive(&package_license_lock); return TRUE; }
    InterlockedIncrement(&registration->refs); /* local, including while waiting */
    registration->removed = TRUE;
    pending = registration->running || registration->queued;
    while (wait && registration->running && registration->callback_thread != GetCurrentThreadId())
        SleepConditionVariableSRW(&package_license_changed, &package_license_lock, INFINITE, 0);
    if (wait || !pending) retired = package_license_retire_registration(registration);
    ReleaseSRWLockExclusive(&package_license_lock);
    if (retired) package_license_registration_release(registration); /* list */
    package_license_registration_release(registration);
    return wait || !pending;
}

struct package_license_async
{
    LONG refs, canceled, started;
    char store[13], request[1024];
    struct package_license *license;
    IXThreadingImpl *threading;
    XAsyncBlock *async;
};
static const char package_license_identity;

static void package_license_async_release( struct package_license_async *context )
{
    if (InterlockedDecrement(&context->refs)) return;
    if (context->license) package_license_dispose(context->license);
    IXThreadingImpl_Release(context->threading);
    free(context);
}

static HRESULT package_license_from_snapshot( struct entitled_snapshot *snapshot, const char *store,
    IXThreadingImpl *threading, struct package_license **out )
{
    UINT i, j;
    UINT64 until;
    *out = NULL;
    if (!snapshot || snapshot->proof_until <= (UINT64)time(NULL)) return E_ACCESSDENIED;
    for (i = 0; i < snapshot->count; i++)
    {
        XStoreProduct *product = &snapshot->products[i];
        if (strcmp(product->storeId, store) || product->productKind != XStoreProductKind_Durable ||
            !product->isInUserCollection || !product->hasDigitalDownload) continue;
        for (j = 0; j < product->skusCount; j++)
        {
            XStoreSku *sku = &product->skus[j];
            if (!sku->isInUserCollection || sku->isTrial || sku->collectionData.endDate <= time(NULL)) continue;
            until = min(snapshot->proof_until, (UINT64)sku->collectionData.endDate);
            return package_license_create(threading, until, out);
        }
    }
    return E_ACCESSDENIED;
}

static DWORD WINAPI package_license_acquire_worker( void *arg )
{
    struct package_license_async *context = arg;
    struct entitled_snapshot *snapshot = NULL;
    HRESULT hr = E_ABORT;
    if (!InterlockedCompareExchange(&context->canceled, 0, 0))
        hr = entitled_fetch(context->request, &context->canceled, &snapshot);
    if (SUCCEEDED(hr)) hr = package_license_from_snapshot(snapshot, context->store, context->threading, &context->license);
    if (snapshot) entitled_snapshot_release(snapshot);
    if (InterlockedCompareExchange(&context->canceled, 0, 0)) hr = E_ABORT;
    xodus_store_license_note("store-package-license-result", hr);
    IXThreadingImpl_XAsyncComplete(context->threading, context->async, hr, SUCCEEDED(hr) ? sizeof(void *) : 0);
    package_license_async_release(context);
    return 0;
}

static HRESULT WINAPI package_license_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct package_license_async *context = data->context;
    HANDLE worker;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin: return IXThreadingImpl_XAsyncSchedule(context->threading, data->async, 0);
    case XAsyncOp_DoWork:
        if (InterlockedCompareExchange(&context->started, 1, 0)) return E_PENDING;
        if (InterlockedCompareExchange(&context->canceled, 0, 0)) return E_ABORT;
        context->async = data->async;
        InterlockedIncrement(&context->refs);
        if (!(worker = CreateThread(NULL, 0, package_license_acquire_worker, context, 0, NULL)))
        { hr = HRESULT_FROM_WIN32(GetLastError()); package_license_async_release(context); return hr; }
        CloseHandle(worker);
        return E_PENDING;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(void *) || !context->license) return E_INVALIDARG;
        if (!package_license_valid_object(context->license)) return E_ACCESSDENIED;
        AcquireSRWLockExclusive(&package_license_lock);
        context->license->next = package_licenses; package_licenses = context->license;
        ReleaseSRWLockExclusive(&package_license_lock);
        memcpy(data->buffer, &context->license, sizeof(void *)); context->license = NULL;
        break;
    case XAsyncOp_Cancel: InterlockedExchange(&context->canceled, TRUE); break;
    case XAsyncOp_Cleanup: package_license_async_release(context); break;
    default: break;
    }
    return S_OK;
}

static HRESULT package_license_begin( XStoreContextHandle store, const char *identifier, XAsyncBlock *async )
{
    struct package_license_async *context;
    UINT64 xuid;
    HRESULT hr;
    if (!async || !identifier) return E_INVALIDARG;
    if (FAILED(hr = entitled_store_user(store, &xuid))) return hr;
    if (!entitled_id(storeId)) return E_GAME_MISSING_GAME_CONFIG;
    if (!(context = calloc(1, sizeof(*context)))) return E_OUTOFMEMORY;
    context->refs = 1;
    if (FAILED(hr = xpackage_query_content_store(identifier, sizeof(context->store), context->store)))
    { free(context); return hr; }
    snprintf(context->request, sizeof(context->request),
        "<Request><ParentProductId>%s</ParentProductId><ProductId>%s</ProductId>"
        "<ProductKinds>2</ProductKinds><Market>US</Market><Language>en-US</Language></Request>", storeId, context->store);
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&context->threading)))
    { free(context); return hr; }
    hr = IXThreadingImpl_XAsyncBegin(context->threading, async, context, &package_license_identity,
        "XStoreAcquireLicenseForPackageAsync", package_license_provider);
    if (FAILED(hr)) package_license_async_release(context);
    return hr;
}

static HRESULT package_license_result( XAsyncBlock *async, XStoreLicenseHandle *out )
{
    IXThreadingImpl *threading;
    HRESULT hr;
    if (!out || !async) return E_INVALIDARG;
    *out = NULL;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult(threading, async, &package_license_identity, sizeof(void *), out, NULL);
    IXThreadingImpl_Release(threading);
    return hr;
}
