/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Device-association subscription ownership only. This platform has no provider
 * of device-to-Xbox-user associations, so registration never fabricates a change
 * or binds a controller to an account. Lookup and selection remain unsupported.
 * No callbacks are queued or running; both unregister modes complete at once. */
struct device_association_registration
{
    struct device_association_registration *next;
    UINT64 token;
    IXThreadingImpl *threading;
    XTaskQueueHandle queue;
    void *context;
    XUserDeviceAssociationChangedCallback *callback;
};

static SRWLOCK device_association_lock = SRWLOCK_INIT;
static struct device_association_registration *device_association_registrations;
static UINT64 device_association_next_token;

static void device_association_free( struct device_association_registration *registration )
{
    /* Queue cleanup can re-enter other providers; never do it under our lock. */
    IXThreadingImpl_XTaskQueueCloseHandle( registration->threading, registration->queue );
    IXThreadingImpl_Release( registration->threading );
    free( registration );
}

static HRESULT device_association_register( XTaskQueueHandle queue, void *context,
        XUserDeviceAssociationChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct device_association_registration *registration;
    IXThreadingImpl *threading;
    HRESULT hr;

    if (!token) return E_INVALIDARG;
    token->token = 0;
    if (!callback) return E_INVALIDARG;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ))) return hr;
    if (!(registration = calloc( 1, sizeof(*registration) )))
    {
        IXThreadingImpl_Release( threading );
        return E_OUTOFMEMORY;
    }
    registration->threading = threading;
    if (queue) hr = IXThreadingImpl_XTaskQueueDuplicateHandle( threading, queue, &registration->queue );
    else hr = IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( threading, &registration->queue )
            ? S_OK : HRESULT_FROM_WIN32( ERROR_NO_TASK_QUEUE );
    if (FAILED(hr))
    {
        IXThreadingImpl_Release( threading );
        free( registration );
        return hr;
    }
    registration->context = context;
    registration->callback = callback;
    AcquireSRWLockExclusive( &device_association_lock );
    if (device_association_next_token == ~(UINT64)0)
        hr = HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW );
    else
    {
        token->token = registration->token = ++device_association_next_token;
        registration->next = device_association_registrations;
        device_association_registrations = registration;
    }
    ReleaseSRWLockExclusive( &device_association_lock );
    if (FAILED(hr)) device_association_free( registration );
    return hr;
}

static BOOLEAN device_association_unregister( XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct device_association_registration **cursor, *registration = NULL;

    (void)wait; /* No association-event source, therefore no callback to await. */
    AcquireSRWLockExclusive( &device_association_lock );
    for (cursor = &device_association_registrations; *cursor; cursor = &(*cursor)->next)
    {
        if ((*cursor)->token != token.token) continue;
        registration = *cursor;
        *cursor = registration->next;
        break;
    }
    ReleaseSRWLockExclusive( &device_association_lock );
    if (!registration) return FALSE;
    device_association_free( registration );
    return TRUE;
}
