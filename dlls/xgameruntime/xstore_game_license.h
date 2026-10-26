/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Running-title license change subscriptions, not license grants.
 * Only proof-backed current-title refresh changes generate events. Registration
 * emits nothing. Failed/filtered/absent receipts are not proof of revocation.
 * No background Store purchase/revocation feed is supplied by this runtime. */
struct game_license_registration
{
    LONG refs;
    struct game_license_registration *next;
    XStoreContextHandle store;
    UINT64 serial, token, version, delivered;
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    void *context;
    XStoreGameLicenseChangedCallback *callback;
    BOOL removed, queued, running;
    DWORD callback_thread;
};
static SRWLOCK game_license_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE game_license_changed = CONDITION_VARIABLE_INIT;
static struct game_license_registration *game_license_registrations;
static UINT64 game_license_next_token;

static void game_license_release( struct game_license_registration *registration )
{
    if (InterlockedDecrement(&registration->refs)) return;
    IXThreadingImpl_XTaskQueueCloseHandle(registration->threading, registration->queue);
    IXThreadingImpl_Release(registration->threading);
    free(registration);
}

/* Registry lock held. Pending unregisters remain visible to later wait=true. */
static BOOL game_license_retire( struct game_license_registration *registration )
{
    struct game_license_registration **link;
    for (link = &game_license_registrations; *link && *link != registration; link = &(*link)->next);
    if (!*link) return FALSE;
    *link = registration->next;
    return TRUE;
}

static void game_license_submit( struct game_license_registration *registration );

static void CALLBACK game_license_callback( void *arg, BOOLEAN canceled )
{
    struct game_license_registration *registration = arg;
    BOOL deliver, again = FALSE, retired = FALSE;
    AcquireSRWLockExclusive(&game_license_lock);
    registration->queued = FALSE;
    deliver = !canceled && !registration->removed && registration->version > registration->delivered;
    if (deliver)
    {
        registration->delivered = registration->version;
        registration->running = TRUE;
        registration->callback_thread = GetCurrentThreadId();
    }
    ReleaseSRWLockExclusive(&game_license_lock);
    if (deliver) registration->callback(registration->context);
    AcquireSRWLockExclusive(&game_license_lock);
    registration->running = FALSE;
    if (registration->removed) retired = game_license_retire(registration);
    else if (deliver && registration->version > registration->delivered)
        again = registration->queued = TRUE;
    WakeAllConditionVariable(&game_license_changed);
    ReleaseSRWLockExclusive(&game_license_lock);
    if (retired) game_license_release(registration);
    /* Reuse the callback reference for a refresh arriving during delivery. */
    if (again) game_license_submit(registration);
    else game_license_release(registration);
}

static void game_license_submit( struct game_license_registration *registration )
{
    BOOL retired = FALSE;
    HRESULT hr = IXThreadingImpl_XTaskQueueSubmitCallback(registration->threading, registration->queue,
        XTaskQueuePort_Completion, registration, game_license_callback);
    if (SUCCEEDED(hr)) return;
    AcquireSRWLockExclusive(&game_license_lock);
    registration->queued = FALSE;
    if (registration->removed) retired = game_license_retire(registration);
    WakeAllConditionVariable(&game_license_changed);
    ReleaseSRWLockExclusive(&game_license_lock);
    if (retired) game_license_release(registration);
    game_license_release(registration);
}

static void game_license_notify( UINT64 serial, UINT64 version )
{
    struct game_license_registration *registration;
    for (;;)
    {
        AcquireSRWLockExclusive(&game_license_lock);
        for (registration = game_license_registrations; registration; registration = registration->next)
            if (registration->serial == serial && !registration->removed && registration->version < version) break;
        if (registration)
        {
            registration->version = version;
            if (!registration->queued && !registration->running)
            {
                registration->queued = TRUE;
                InterlockedIncrement(&registration->refs);
            }
            else
            {
                ReleaseSRWLockExclusive(&game_license_lock);
                continue;
            }
        }
        ReleaseSRWLockExclusive(&game_license_lock);
        if (!registration) break;
        game_license_submit(registration);
    }
}

static int game_license_compare( const void *a, const void *b )
{
    return strcmp(((const struct game_license_sku *)a)->id, ((const struct game_license_sku *)b)->id);
}

static void game_license_observe( UINT64 serial, UINT64 refresh, const struct entitled_snapshot *snapshot )
{
    struct game_license_sku skus[ENTITLED_MAX_SKUS] = {0};
    struct entitled_store_context *store;
    const XStoreProduct *product = NULL;
    UINT32 i, count = 0;
    UINT64 version = 0, now = time(NULL);
    BOOL changed = FALSE;
    if (!serial || !refresh || !storeId || !snapshot || snapshot->proof_until <= now || snapshot->valid_until <= now) return;
    for (i = 0; i < snapshot->count; i++)
        if (!strcmp(snapshot->products[i].storeId, storeId) &&
            snapshot->products[i].productKind == XStoreProductKind_Game) { product = &snapshot->products[i]; break; }
    if (!product || !product->isInUserCollection || !product->skusCount || product->skusCount > ENTITLED_MAX_SKUS) return;
    for (i = 0; i < product->skusCount; i++)
    {
        const XStoreSku *sku = &product->skus[i];
        if (!sku->isInUserCollection || !entitled_sku_id(sku->skuId) || sku->collectionData.endDate <= 0 ||
            (UINT64)sku->collectionData.endDate <= now) return;
        memcpy(skus[count].id, sku->skuId, 5);
        skus[count].trial = sku->isTrial;
        skus[count++].end = sku->collectionData.endDate;
    }
    qsort(skus, count, sizeof(*skus), game_license_compare);
    AcquireSRWLockExclusive(&entitled_stores_lock);
    for (store = entitled_stores; store && store->serial != serial; store = store->next);
    if (store && refresh > store->license_refresh)
    {
        changed = count != store->license_count;
        for (i = 0; !changed && i < count; i++)
            changed = strcmp(skus[i].id, store->license_skus[i].id) ||
                skus[i].trial != store->license_skus[i].trial || skus[i].end != store->license_skus[i].end;
        changed = changed && store->license_version;
        store->license_refresh = refresh;
        store->license_count = count;
        memcpy(store->license_skus, skus, sizeof(skus));
        if (!store->license_version || changed) store->license_version++;
        version = store->license_version;
    }
    ReleaseSRWLockExclusive(&entitled_stores_lock);
    if (changed) game_license_notify(serial, version);
}

static HRESULT game_license_register( XStoreContextHandle handle, XTaskQueueHandle queue, void *context,
    XStoreGameLicenseChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct game_license_registration *registration;
    struct entitled_store_context *store;
    IXThreadingImpl *threading;
    HRESULT hr;
    if (!token) return E_POINTER;
    token->token = 0;
    if (!callback) return E_POINTER;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading))) return hr;
    if (!(registration = calloc(1, sizeof(*registration)))) { IXThreadingImpl_Release(threading); return E_OUTOFMEMORY; }
    AcquireSRWLockShared(&entitled_stores_lock);
    for (store = entitled_stores; store && store != handle; store = store->next);
    if (!store) hr = E_INVALIDARG;
    else if (queue) hr = IXThreadingImpl_XTaskQueueDuplicateHandle(threading, queue, &registration->queue);
    else hr = IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue(threading, &registration->queue) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        registration->refs = 1;
        registration->store = handle; registration->serial = store->serial;
        registration->version = registration->delivered = store->license_version;
        registration->threading = threading;
        registration->context = context; registration->callback = callback;
        AcquireSRWLockExclusive(&game_license_lock);
        if (!++game_license_next_token) ++game_license_next_token;
        registration->token = token->token = game_license_next_token;
        registration->next = game_license_registrations; game_license_registrations = registration;
        ReleaseSRWLockExclusive(&game_license_lock);
    }
    ReleaseSRWLockShared(&entitled_stores_lock);
    if (FAILED(hr)) { IXThreadingImpl_Release(threading); free(registration); }
    return hr;
}

static BOOLEAN game_license_unregister( XStoreContextHandle handle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct game_license_registration *registration;
    BOOL pending, retired = FALSE;
    AcquireSRWLockExclusive(&game_license_lock);
    for (registration = game_license_registrations; registration; registration = registration->next)
        if (registration->store == handle && registration->token == token.token) break;
    if (!registration) { ReleaseSRWLockExclusive(&game_license_lock); return TRUE; }
    InterlockedIncrement(&registration->refs);
    registration->removed = TRUE;
    pending = registration->queued || registration->running;
    /* Suppress queued work without pumping a Manual queue. Never self-wait. */
    while (wait && registration->running && registration->callback_thread != GetCurrentThreadId())
        SleepConditionVariableSRW(&game_license_changed, &game_license_lock, INFINITE, 0);
    if (wait || !pending) retired = game_license_retire(registration);
    ReleaseSRWLockExclusive(&game_license_lock);
    if (retired) game_license_release(registration);
    game_license_release(registration);
    return wait || !pending;
}

static void game_license_close_store( UINT64 serial )
{
    struct game_license_registration *registration;
    BOOL retired;
    AcquireSRWLockExclusive(&game_license_lock);
    for (registration = game_license_registrations; registration; registration = registration->next)
        if (registration->serial == serial) registration->removed = TRUE;
    for (;;)
    {
        for (registration = game_license_registrations; registration; registration = registration->next)
            if (registration->serial == serial) break;
        if (!registration) break;
        InterlockedIncrement(&registration->refs);
        while (registration->running && registration->callback_thread != GetCurrentThreadId())
            SleepConditionVariableSRW(&game_license_changed, &game_license_lock, INFINITE, 0);
        retired = game_license_retire(registration);
        ReleaseSRWLockExclusive(&game_license_lock);
        if (retired) game_license_release(registration);
        game_license_release(registration);
        AcquireSRWLockExclusive(&game_license_lock);
    }
    ReleaseSRWLockExclusive(&game_license_lock);
}
