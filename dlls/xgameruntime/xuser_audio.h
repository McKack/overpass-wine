/* SPDX-License-Identifier: LGPL-2.1-or-later
 * The runtime exposes one signed-in user. Its communication audio endpoints
 * are the genuine Wine MMDevice defaults, not fabricated device associations.
 * Each subscription owns an MTA worker for COM registration and teardown. */
#include <initguid.h>
#include <mmdeviceapi.h>

struct audio_registration;
struct audio_event
{
    struct audio_event *next;
    struct audio_registration *registration;
    DWORD thread;
    XUserLocalId user;
    XUserDefaultAudioEndpointKind kind;
    WCHAR *endpoint;
};
struct audio_registration
{
    IMMNotificationClient IMMNotificationClient_iface;
    struct audio_registration *next;
    LONG refs;
    UINT64 token;
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    void *context;
    XUserDefaultAudioEndpointUtf16ChangedCallback *callback;
    HANDLE ready, stop, worker;
    HRESULT started;
    BOOL removed;
    struct audio_event *running;
};
static SRWLOCK audio_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE audio_changed = CONDITION_VARIABLE_INIT;
static struct audio_registration *audio_registrations;
static UINT64 audio_next_token;

static struct audio_registration *audio_from_client(IMMNotificationClient *client)
{ return CONTAINING_RECORD(client, struct audio_registration, IMMNotificationClient_iface); }
static ULONG WINAPI audio_addref(IMMNotificationClient *client)
{ return InterlockedIncrement(&audio_from_client(client)->refs); }
static ULONG WINAPI audio_release(IMMNotificationClient *client)
{
    struct audio_registration *r = audio_from_client(client);
    ULONG refs = InterlockedDecrement(&r->refs);
    if (refs) return refs;
    if (r->queue) IXThreadingImpl_XTaskQueueCloseHandle(r->threading, r->queue);
    if (r->threading) IXThreadingImpl_Release(r->threading);
    if (r->worker) CloseHandle(r->worker);
    if (r->ready) CloseHandle(r->ready);
    if (r->stop) CloseHandle(r->stop);
    free(r);
    return 0;
}
static HRESULT WINAPI audio_query(IMMNotificationClient *client, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_IMMNotificationClient)) return E_NOINTERFACE;
    *out = client; audio_addref(client); return S_OK;
}
/* Under audio_lock. Retired tokens are idempotent once title code has drained. */
static BOOL audio_retire(struct audio_registration *r)
{
    struct audio_registration **p;
    for (p = &audio_registrations; *p && *p != r; p = &(*p)->next);
    if (!*p) return FALSE;
    *p = r->next; return TRUE;
}
static void CALLBACK audio_deliver(void *context, BOOLEAN canceled)
{
    struct audio_event *event = context, **link;
    struct audio_registration *r = event->registration;
    BOOL deliver, retired = FALSE;
    AcquireSRWLockExclusive(&audio_lock);
    deliver = !canceled && !r->removed;
    if (deliver)
    {
        event->thread = GetCurrentThreadId();
        event->next = r->running; r->running = event;
    }
    ReleaseSRWLockExclusive(&audio_lock);
    if (deliver)
    {
        r->callback(r->context, event->user, event->kind, event->endpoint);
        AcquireSRWLockExclusive(&audio_lock);
        for (link = &r->running; *link && *link != event; link = &(*link)->next);
        if (*link) *link = event->next;
        if (r->removed && !r->running) retired = audio_retire(r);
        WakeAllConditionVariable(&audio_changed);
        ReleaseSRWLockExclusive(&audio_lock);
    }
    free(event->endpoint); free(event);
    if (retired) audio_release(&r->IMMNotificationClient_iface);
    audio_release(&r->IMMNotificationClient_iface);
}
static HRESULT audio_queue_event(IMMNotificationClient *client, EDataFlow flow, ERole role, const WCHAR *endpoint)
{
    struct audio_registration *r = audio_from_client(client);
    struct audio_event *event;
    XTaskQueueHandle queue = NULL;
    XUserLocalId user;
    HRESULT hr;
    SIZE_T count = endpoint ? wcsnlen(endpoint, 32768) + 1 : 0;
    if (role != eCommunications || (flow != eRender && flow != eCapture)) return S_OK;
    if (!audio_current_user(&user)) return S_OK;
    if (count > 32768) return E_INVALIDARG;
    if (!(event = calloc(1, sizeof(*event)))) return E_OUTOFMEMORY;
    if (endpoint && !(event->endpoint = malloc(count * sizeof(WCHAR)))) { free(event); return E_OUTOFMEMORY; }
    if (endpoint) memcpy(event->endpoint, endpoint, count * sizeof(WCHAR));
    event->registration = r; event->user = user;
    event->kind = flow == eRender ? XUserDefaultAudioEndpointKind_CommunicationRender : XUserDefaultAudioEndpointKind_CommunicationCapture;
    AcquireSRWLockExclusive(&audio_lock);
    hr = r->removed || !r->token || !r->queue ? E_ABORT : IXThreadingImpl_XTaskQueueDuplicateHandle(r->threading, r->queue, &queue);
    if (SUCCEEDED(hr)) audio_addref(client);
    ReleaseSRWLockExclusive(&audio_lock);
    if (FAILED(hr)) { free(event->endpoint); free(event); return hr == E_ABORT ? S_OK : hr; }
    /* Immediate queues may execute title code inside SubmitCallback. Keep a
     * separate queue handle until it returns; never hold our lock across it. */
    hr = IXThreadingImpl_XTaskQueueSubmitCallback(r->threading, queue, XTaskQueuePort_Completion, event, audio_deliver);
    IXThreadingImpl_XTaskQueueCloseHandle(r->threading, queue);
    if (FAILED(hr)) { free(event->endpoint); free(event); audio_release(client); }
    return hr;
}
static HRESULT WINAPI audio_default_changed(IMMNotificationClient *client, EDataFlow flow, ERole role, const WCHAR *endpoint)
{
    HRESULT hr;
    /* Self-unregister on an Immediate queue can release every other owner. */
    audio_addref(client);
    hr = audio_queue_event(client, flow, role, endpoint);
    audio_release(client);
    return hr;
}
static HRESULT WINAPI audio_state_changed(IMMNotificationClient *client, const WCHAR *id, DWORD state) { return S_OK; }
static HRESULT WINAPI audio_added(IMMNotificationClient *client, const WCHAR *id) { return S_OK; }
static HRESULT WINAPI audio_removed(IMMNotificationClient *client, const WCHAR *id) { return S_OK; }
static HRESULT WINAPI audio_property_changed(IMMNotificationClient *client, const WCHAR *id, const PROPERTYKEY key) { return S_OK; }
static const IMMNotificationClientVtbl audio_vtable =
{
    audio_query, audio_addref, audio_release, audio_state_changed, audio_added,
    audio_removed, audio_default_changed, audio_property_changed
};
static DWORD WINAPI audio_worker(void *context)
{
    struct audio_registration *r = context;
    IMMDeviceEnumerator *enumerator = NULL;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED), hr = initialized;
    if (SUCCEEDED(hr)) hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                            &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (SUCCEEDED(hr) && WaitForSingleObject(r->stop, 0) == WAIT_OBJECT_0) hr = E_ABORT;
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(enumerator, &r->IMMNotificationClient_iface);
    r->started = hr; SetEvent(r->ready);
    if (SUCCEEDED(hr))
    {
        WaitForSingleObject(r->stop, INFINITE);
        hr = IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(enumerator, &r->IMMNotificationClient_iface);
        /* A provider that fails to unregister can still hold the raw client
         * pointer. Retain its worker reference rather than permit UAF. */
        if (FAILED(hr)) WARN("audio notification teardown failed, hr %#lx.\n", hr);
    }
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (SUCCEEDED(hr)) audio_release(&r->IMMNotificationClient_iface);
    else if (FAILED(r->started)) audio_release(&r->IMMNotificationClient_iface);
    return 0;
}
static HRESULT audio_register(XTaskQueueHandle queue, void *context, XUserDefaultAudioEndpointUtf16ChangedCallback *callback,
                              XTaskQueueRegistrationToken *token)
{
    struct audio_registration *r;
    HRESULT hr;
    DWORD ready = WAIT_FAILED;
    if (!token) return E_INVALIDARG;
    token->token = 0;
    if (!callback) return E_INVALIDARG;
    if (!(r = calloc(1, sizeof(*r)))) return E_OUTOFMEMORY;
    r->IMMNotificationClient_iface.lpVtbl = &audio_vtable; r->refs = 1;
    r->context = context; r->callback = callback;
    if (FAILED(hr = QueryApiImpl(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&r->threading))) goto failed;
    hr = queue ? IXThreadingImpl_XTaskQueueDuplicateHandle(r->threading, queue, &r->queue) :
        IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue(r->threading, &r->queue) ? S_OK : HRESULT_FROM_WIN32(ERROR_NO_TASK_QUEUE);
    if (FAILED(hr)) goto failed;
    r->ready = CreateEventW(NULL, TRUE, FALSE, NULL); r->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!r->ready || !r->stop) { hr = HRESULT_FROM_WIN32(GetLastError()); goto failed; }
    audio_addref(&r->IMMNotificationClient_iface);
    r->worker = CreateThread(NULL, 0, audio_worker, r, 0, NULL);
    if (!r->worker) { hr = HRESULT_FROM_WIN32(GetLastError()); audio_release(&r->IMMNotificationClient_iface); goto failed; }
    ready = WaitForSingleObject(r->ready, 15000);
    if (ready != WAIT_OBJECT_0)
    { hr = HRESULT_FROM_WIN32(ready == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()); goto failed; }
    if (FAILED(hr = r->started)) goto failed;
    AcquireSRWLockExclusive(&audio_lock);
    if (audio_next_token == ~(UINT64)0) hr = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    else
    {
        token->token = r->token = ++audio_next_token;
        r->next = audio_registrations; audio_registrations = r;
    }
    ReleaseSRWLockExclusive(&audio_lock);
    if (SUCCEEDED(hr)) return S_OK;
failed:
    AcquireSRWLockExclusive(&audio_lock); r->removed = TRUE; ReleaseSRWLockExclusive(&audio_lock);
    if (r->stop) SetEvent(r->stop);
    /* Timed-out COM initialization keeps only its worker reference. It will
     * see stop and tear down if the underlying service eventually returns. */
    if (r->worker && ready == WAIT_OBJECT_0) WaitForSingleObject(r->worker, INFINITE);
    audio_release(&r->IMMNotificationClient_iface); return hr;
}
static BOOLEAN audio_unregister(XTaskQueueRegistrationToken token, BOOLEAN wait)
{
    struct audio_registration *r;
    struct audio_event *event;
    XTaskQueueHandle queue;
    BOOL self = FALSE, pending, retired = FALSE;
    if (!token.token) return FALSE;
    AcquireSRWLockExclusive(&audio_lock);
    for (r = audio_registrations; r && r->token != token.token; r = r->next);
    if (!r)
    {
        BOOL issued = token.token <= audio_next_token;
        ReleaseSRWLockExclusive(&audio_lock); return issued;
    }
    audio_addref(&r->IMMNotificationClient_iface); r->removed = TRUE;
    queue = r->queue; r->queue = NULL;
    SetEvent(r->stop);
    for (event = r->running; event; event = event->next) if (event->thread == GetCurrentThreadId()) self = TRUE;
    while (wait && r->running && !self) SleepConditionVariableSRW(&audio_changed, &audio_lock, INFINITE, 0);
    pending = r->running != NULL;
    if (!pending) retired = audio_retire(r);
    ReleaseSRWLockExclusive(&audio_lock);
    if (queue) IXThreadingImpl_XTaskQueueCloseHandle(r->threading, queue);
    if (wait && !self && r->worker) WaitForSingleObject(r->worker, INFINITE);
    if (retired) audio_release(&r->IMMNotificationClient_iface);
    audio_release(&r->IMMNotificationClient_iface);
    return !pending;
}
static HRESULT audio_get_default(XUserLocalId requested, XUserDefaultAudioEndpointKind kind, SIZE_T capacity, WCHAR *out, SIZE_T *used)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    WCHAR *id = NULL;
    XUserLocalId current;
    HRESULT initialized, hr;
    SIZE_T count;
    if (used) *used = 0;
    if (out && capacity) *out = 0;
    if (!out || (kind != XUserDefaultAudioEndpointKind_CommunicationRender && kind != XUserDefaultAudioEndpointKind_CommunicationCapture)) return E_INVALIDARG;
    if (!audio_current_user(&current) || current.value != requested.value) return E_GAMEUSER_USER_NOT_FOUND;
    initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return initialized;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,
        kind == XUserDefaultAudioEndpointKind_CommunicationRender ? eRender : eCapture, eCommunications, &device);
    if (SUCCEEDED(hr)) hr = IMMDevice_GetId(device, &id);
    if (SUCCEEDED(hr))
    {
        if (!id || (count = wcsnlen(id, 32768)) == 32768) hr = E_UNEXPECTED;
        else
        {
            ++count; if (used) *used = count;
            if (capacity < count) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            else memcpy(out, id, count * sizeof(WCHAR));
        }
    }
    CoTaskMemFree(id);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return hr;
}
