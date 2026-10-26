/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Installation snapshots and queue-bound notifications. File presence is not
 * a license, download service or integrity check. Included after chunks.h. */
struct installation_monitor
{
    LONG refs;
    struct installation_monitor *next;
    BOOL closed, refresh_pending, notifying;
    SRWLOCK refresh_lock;
    char identifier[33];
    UINT32 count, interval;
    XPackageChunkSelector *selectors;
    XPackageInstallationProgress progress;
    UINT64 revision;
    XTaskQueueHandle queue, poll_queue;
};
struct installation_registration
{
    LONG refs;
    struct installation_registration *next;
    struct installation_monitor *monitor;
    XPackageInstallationProgressCallback *callback;
    void *context;
    UINT64 token, seen;
    BOOL removed, queued, running;
    DWORD thread;
};
static SRWLOCK installation_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE installation_changed = CONDITION_VARIABLE_INIT;
static struct installation_monitor *installation_monitors;
static struct installation_registration *installation_registrations;
static UINT64 installation_next_token;

static void installation_release( struct installation_monitor *monitor )
{
    UINT i;
    if (InterlockedDecrement(&monitor->refs)) return;
    for (i = 0; i < monitor->count; i++)
        if (monitor->selectors[i].type != XPackageChunkSelectorType_Chunk)
            free((void *)monitor->selectors[i].language);
    free(monitor->selectors);
    if (monitor->poll_queue) IXThreadingImpl_XTaskQueueCloseHandle(x_threading_impl, monitor->poll_queue);
    if (monitor->queue) IXThreadingImpl_XTaskQueueCloseHandle(x_threading_impl, monitor->queue);
    free(monitor);
}

/* Registry lookups never dereference an unrecognised or closed public handle. */
static struct installation_monitor *installation_acquire( XPackageInstallationMonitorHandle handle )
{
    struct installation_monitor *monitor;
    AcquireSRWLockShared(&installation_lock);
    for (monitor = installation_monitors; monitor && (void *)monitor != handle; monitor = monitor->next);
    if (monitor) InterlockedIncrement(&monitor->refs);
    ReleaseSRWLockShared(&installation_lock);
    return monitor;
}

static void installation_registration_release( struct installation_registration *registration )
{
    if (InterlockedDecrement(&registration->refs)) return;
    installation_release(registration->monitor);
    free(registration);
}

/* Called under installation_lock. Running removed registrations remain visible
 * so repeated nonblocking unregisters can observe when title code has drained. */
static BOOL installation_retire( struct installation_registration *registration )
{
    struct installation_registration **link;
    for (link = &installation_registrations; *link && *link != registration; link = &(*link)->next);
    if (!*link) return FALSE;
    *link = registration->next;
    return TRUE;
}

static HRESULT installation_snapshot( struct installation_monitor *monitor, XPackageInstallationProgress *progress )
{
    struct chunk_snapshot *snapshot;
    UINT i, j;
    BOOL ready, matched;
    HRESULT hr;
    memset(progress, 0, sizeof(*progress));
    if (FAILED(hr = chunks_load(monitor->identifier, &snapshot))) return hr;
    ready = snapshot->registration == XPackageChunkAvailability_Ready;
    if (!monitor->count)
    {
        for (j = 0; j < snapshot->count; j++)
            ready &= snapshot->chunks[j].availability == XPackageChunkAvailability_Ready;
    }
    for (i = 0; i < monitor->count; i++)
    {
        const XPackageChunkSelector *selector = &monitor->selectors[i];
        int attribute = selector->type == XPackageChunkSelectorType_Language ? 0 :
                        selector->type == XPackageChunkSelectorType_Tag ? 1 : 2;
        matched = FALSE;
        for (j = 0; j < snapshot->count; j++)
        {
            struct installed_chunk *chunk = &snapshot->chunks[j];
            if (selector->type == XPackageChunkSelectorType_Chunk ? chunk->id == selector->chunkId :
                chunks_contains(chunk->attributes[attribute], selector->language, attribute == 0))
            {
                matched = TRUE;
                ready &= chunk->availability == XPackageChunkAvailability_Ready;
            }
            else if (selector->type != XPackageChunkSelectorType_Chunk && !chunk->attributes[attribute])
                ready &= chunk->availability == XPackageChunkAvailability_Ready;
        }
        ready &= matched;
    }
    /* Extracted layouts do not carry trustworthy installed/download byte totals.
     * Preserve zero (unknown), rather than inventing progress or network work. */
    progress->completed = progress->launchable = ready;
    chunks_free(snapshot);
    return S_OK;
}

static HRESULT installation_refresh( struct installation_monitor *monitor )
{
    XPackageInstallationProgress progress;
    HRESULT hr;
    AcquireSRWLockExclusive(&monitor->refresh_lock);
    hr = installation_snapshot(monitor, &progress);
    AcquireSRWLockExclusive(&installation_lock);
    /* Invalid/disappearing metadata must never retain a stale complete result. */
    if (!monitor->closed && (monitor->progress.completed != progress.completed ||
        monitor->progress.launchable != progress.launchable))
    {
        monitor->progress = progress;
        ++monitor->revision;
    }
    ReleaseSRWLockExclusive(&installation_lock);
    ReleaseSRWLockExclusive(&monitor->refresh_lock);
    return hr;
}

static void installation_notify( struct installation_monitor *monitor );
static void CALLBACK installation_callback( void *arg, BOOLEAN canceled )
{
    struct installation_registration *registration = arg;
    struct installation_monitor *monitor = registration->monitor;
    BOOL deliver, retired = FALSE;
    AcquireSRWLockExclusive(&installation_lock);
    registration->queued = FALSE;
    deliver = !canceled && !registration->removed && !monitor->closed;
    if (deliver)
    {
        registration->running = TRUE;
        registration->thread = GetCurrentThreadId();
        registration->seen = monitor->revision;
    }
    else if (registration->removed) retired = installation_retire(registration);
    ReleaseSRWLockExclusive(&installation_lock);
    if (deliver)
    {
        registration->callback(registration->context, (XPackageInstallationMonitorHandle)monitor);
        AcquireSRWLockExclusive(&installation_lock);
        registration->running = FALSE;
        if (registration->removed) retired = installation_retire(registration);
        WakeAllConditionVariable(&installation_changed);
        ReleaseSRWLockExclusive(&installation_lock);
        installation_notify(monitor);
    }
    if (retired) installation_registration_release(registration); /* registry */
    installation_registration_release(registration); /* queue */
}

/* Submission may execute inline on an Immediate port. Never hold our locks
 * across submission/title code, and retain both registration and monitor. */
static HRESULT installation_submit( struct installation_registration *registration )
{
    struct installation_monitor *monitor = registration->monitor;
    BOOL retired = FALSE;
    HRESULT hr = IXThreadingImpl_XTaskQueueSubmitCallback(x_threading_impl, monitor->queue,
        XTaskQueuePort_Completion, registration, installation_callback);
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&installation_lock);
        registration->queued = FALSE;
        registration->removed = TRUE;
        retired = installation_retire(registration);
        ReleaseSRWLockExclusive(&installation_lock);
        if (retired) installation_registration_release(registration);
        installation_registration_release(registration); /* failed queue ref */
    }
    return hr;
}

static void installation_notify( struct installation_monitor *monitor )
{
    struct installation_registration *registration;
    AcquireSRWLockExclusive(&installation_lock);
    if (monitor->notifying) { ReleaseSRWLockExclusive(&installation_lock); return; }
    monitor->notifying = TRUE;
    ReleaseSRWLockExclusive(&installation_lock);
    for (;;)
    {
        AcquireSRWLockExclusive(&installation_lock);
        for (registration = installation_registrations; registration; registration = registration->next)
            if (registration->monitor == monitor && !monitor->closed && monitor->interval &&
                !registration->removed && !registration->queued && !registration->running &&
                registration->seen != monitor->revision) break;
        if (registration)
        {
            registration->queued = TRUE;
            registration->seen = monitor->revision;
            InterlockedIncrement(&registration->refs);
        }
        if (!registration) monitor->notifying = FALSE;
        ReleaseSRWLockExclusive(&installation_lock);
        if (!registration) break;
        installation_submit(registration);
    }
}

static void installation_schedule( struct installation_monitor *monitor );
static void CALLBACK installation_poll( void *arg, BOOLEAN canceled )
{
    struct installation_monitor *monitor = arg;
    BOOL refresh;
    AcquireSRWLockExclusive(&installation_lock);
    refresh = !canceled && !monitor->closed;
    ReleaseSRWLockExclusive(&installation_lock);
    if (refresh)
    {
        installation_refresh(monitor);
    }
    AcquireSRWLockExclusive(&installation_lock);
    monitor->refresh_pending = FALSE;
    ReleaseSRWLockExclusive(&installation_lock);
    if (refresh)
    {
        installation_notify(monitor);
        installation_schedule(monitor);
    }
    installation_release(monitor);
}

static void installation_schedule( struct installation_monitor *monitor )
{
    BOOL schedule;
    HRESULT hr;
    AcquireSRWLockExclusive(&installation_lock);
    schedule = !monitor->closed && monitor->interval && !monitor->progress.completed && !monitor->refresh_pending;
    if (schedule) { monitor->refresh_pending = TRUE; InterlockedIncrement(&monitor->refs); }
    ReleaseSRWLockExclusive(&installation_lock);
    if (!schedule) return;
    hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback(x_threading_impl, monitor->poll_queue,
        XTaskQueuePort_Work, monitor->interval, monitor, installation_poll);
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&installation_lock);
        monitor->refresh_pending = FALSE;
        ReleaseSRWLockExclusive(&installation_lock);
        installation_release(monitor);
    }
}

static HRESULT installation_create( const char *identifier, UINT32 count, XPackageChunkSelector *selectors,
    UINT32 interval, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *out )
{
    struct installation_monitor *monitor;
    UINT i;
    XTaskQueuePortHandle work, completion;
    HRESULT hr;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!chunks_identifier(identifier) || count > CHUNKS_MAX || (count && !selectors)) return E_INVALIDARG;
    if (!(monitor = calloc(1, sizeof(*monitor)))) return E_OUTOFMEMORY;
    monitor->refs = 1; monitor->interval = interval; monitor->revision = 1;
    strcpy(monitor->identifier, identifier);
    if (count && !(monitor->selectors = calloc(count, sizeof(*selectors))))
    { installation_release(monitor); return E_OUTOFMEMORY; }
    hr = S_OK;
    for (i = 0; i < count; i++)
    {
        XPackageChunkSelector *copy = &monitor->selectors[i];
        copy->type = selectors[i].type;
        if (copy->type == XPackageChunkSelectorType_Chunk) copy->chunkId = selectors[i].chunkId;
        else if (copy->type == XPackageChunkSelectorType_Language || copy->type == XPackageChunkSelectorType_Tag ||
                 copy->type == XPackageChunkSelectorType_Feature)
        {
            if (!selectors[i].language || !*selectors[i].language ||
                strnlen(selectors[i].language, CHUNKS_TEXT_MAX + 1) > CHUNKS_TEXT_MAX) { hr = E_INVALIDARG; break; }
            if (!(copy->language = strdup(selectors[i].language))) { hr = E_OUTOFMEMORY; break; }
        }
        else { hr = E_INVALIDARG; break; }
        monitor->count++;
    }
    if (SUCCEEDED(hr)) hr = installation_snapshot(monitor, &monitor->progress);
    if (SUCCEEDED(hr) && interval)
    {
        if (queue) hr = IXThreadingImpl_XTaskQueueDuplicateHandle(x_threading_impl, queue, &monitor->queue);
        else hr = IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue(x_threading_impl, &monitor->queue) ? S_OK : E_FAIL;
        /* A private composite shares dispatch ports, not cancellation state.
         * Close can wake/cancel a long delayed poll without terminating any
         * title-owned work or requiring its minimum interval to expire. */
        if (SUCCEEDED(hr)) hr = IXThreadingImpl_XTaskQueueGetPort(x_threading_impl, monitor->queue, XTaskQueuePort_Work, &work);
        if (SUCCEEDED(hr)) hr = IXThreadingImpl_XTaskQueueGetPort(x_threading_impl, monitor->queue, XTaskQueuePort_Completion, &completion);
        if (SUCCEEDED(hr)) hr = IXThreadingImpl_XTaskQueueCreateComposite(x_threading_impl, work, completion, &monitor->poll_queue);
    }
    if (FAILED(hr)) { installation_release(monitor); return hr; }
    AcquireSRWLockExclusive(&installation_lock);
    monitor->next = installation_monitors; installation_monitors = monitor;
    ReleaseSRWLockExclusive(&installation_lock);
    *out = (XPackageInstallationMonitorHandle)monitor;
    installation_schedule(monitor);
    return S_OK;
}

static void installation_close( XPackageInstallationMonitorHandle handle )
{
    struct installation_monitor **link, *monitor;
    struct installation_registration *registration;
    AcquireSRWLockExclusive(&installation_lock);
    for (link = &installation_monitors; *link && (void *)*link != handle; link = &(*link)->next);
    if (!(monitor = *link)) { ReleaseSRWLockExclusive(&installation_lock); return; }
    *link = monitor->next; monitor->closed = TRUE;
    for (;;)
    {
        for (registration = installation_registrations; registration; registration = registration->next)
            if (registration->monitor == monitor) break;
        if (!registration) break;
        registration->removed = TRUE;
        installation_retire(registration); /* registry reference becomes local */
        while (registration->running && registration->thread != GetCurrentThreadId())
            SleepConditionVariableSRW(&installation_changed, &installation_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&installation_lock);
        installation_registration_release(registration);
        AcquireSRWLockExclusive(&installation_lock);
    }
    ReleaseSRWLockExclusive(&installation_lock);
    if (monitor->poll_queue) IXThreadingImpl_XTaskQueueTerminate(x_threading_impl, monitor->poll_queue, FALSE, NULL, NULL);
    installation_release(monitor); /* public handle */
}

static HRESULT installation_register( XPackageInstallationMonitorHandle handle, void *context,
    XPackageInstallationProgressCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct installation_monitor *monitor;
    struct installation_registration *registration;
    BOOL submit;
    HRESULT hr = S_OK;
    if (!token) return E_INVALIDARG;
    token->token = 0;
    if (!callback) return E_INVALIDARG;
    if (!(monitor = installation_acquire(handle))) return E_INVALIDARG;
    if (!(registration = calloc(1, sizeof(*registration)))) { installation_release(monitor); return E_OUTOFMEMORY; }
    registration->refs = 1; /* local; monitor reference transferred */
    registration->monitor = monitor; registration->callback = callback; registration->context = context;
    AcquireSRWLockExclusive(&installation_lock);
    if (monitor->closed) hr = E_INVALIDARG;
    else
    {
        if (!++installation_next_token) ++installation_next_token;
        registration->token = token->token = installation_next_token;
        registration->next = installation_registrations; installation_registrations = registration;
        InterlockedIncrement(&registration->refs); /* registry */
    }
    submit = SUCCEEDED(hr) && monitor->interval;
    if (submit)
    {
        registration->queued = TRUE; registration->seen = monitor->revision;
        InterlockedIncrement(&registration->refs); /* queue */
    }
    ReleaseSRWLockExclusive(&installation_lock);
    /* A new subscriber receives the current snapshot once, even if the files
     * were already present. Further events require a genuine state change.
     * A zero-interval monitor remains a one-time, query-only snapshot. */
    if (submit && FAILED(hr = installation_submit(registration))) token->token = 0;
    installation_registration_release(registration);
    return hr;
}

static BOOLEAN installation_unregister( XPackageInstallationMonitorHandle handle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct installation_registration *registration;
    BOOL pending, retired = FALSE;
    AcquireSRWLockExclusive(&installation_lock);
    for (registration = installation_registrations; registration; registration = registration->next)
        if ((void *)registration->monitor == handle && registration->token == token.token) break;
    if (!registration) { ReleaseSRWLockExclusive(&installation_lock); return TRUE; }
    InterlockedIncrement(&registration->refs);
    registration->removed = TRUE;
    /* Queued but not started callbacks are suppressed. Waiting must not need
     * the caller to dispatch its own Manual queue, or wait for itself. */
    while (wait && registration->running && registration->thread != GetCurrentThreadId())
        SleepConditionVariableSRW(&installation_changed, &installation_lock, INFINITE, 0);
    pending = registration->running;
    if (!pending) retired = installation_retire(registration);
    ReleaseSRWLockExclusive(&installation_lock);
    if (retired) installation_registration_release(registration);
    installation_registration_release(registration);
    return !pending;
}
