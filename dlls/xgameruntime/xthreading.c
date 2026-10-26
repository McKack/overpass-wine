/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XAsync, XTaskQueue and XThread
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

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct task_context
{
    struct task_context *next;
    XTaskQueueHandle queue;
    XTaskQueuePortHandle port;
    XTaskQueueCallback *callback;
    void *callbackContext;
    ULONGLONG due;
    XTaskQueuePort logical_port;
    BOOL termination;
};

struct monitor_context
{
    XTaskQueueMonitorCallback *callback;
    void *callbackContext;
    XTaskQueueRegistrationToken token;
};

struct port_binding
{
    struct port_binding *next;
    XTaskQueueHandle queue;
    XTaskQueuePort kind;
};

struct XTaskQueuePortObject
{
    IUnknown IUnknown_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    HANDLE wake;
    BOOL worker_active;
    struct port_binding *bindings;
    struct task_context *head;
    XTaskQueueDispatchMode mode;
};

static inline XTaskQueuePortHandle port_impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct XTaskQueuePortObject, IUnknown_iface );
}

static ULONG WINAPI port_AddRef( IUnknown *iface )
{
    XTaskQueuePortHandle impl = port_impl_from_IUnknown( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI port_Release( IUnknown *iface )
{
    XTaskQueuePortHandle impl = port_impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    if (!ref)
    {
        /* Entries and dispatch workers each retain the port. */
        DeleteCriticalSection( &impl->cs );
        CloseHandle( impl->wake );
        free( impl );
    }
    return ref;
}

static const struct IUnknownVtbl port_vtbl =
{
    NULL,
    port_AddRef,
    port_Release,
};

struct XTaskQueueObject
{
    IUnknown IUnknown_iface;
    LONG ref;
    XTaskQueuePortHandle work;
    XTaskQueuePortHandle completion;
    XTaskQueueTerminatedCallback *terminatedCallback;
    void *terminatedContext;
    BOOL composite;
    LONG terminated;
    CRITICAL_SECTION cs;
    LONG pending;
    HANDLE cancel_event;
    HANDLE finished_event;
    struct task_context *termination_entry;
    struct port_binding work_binding, completion_binding;
    struct
    {
        struct monitor_context *entries;
        CRITICAL_SECTION cs;
        UINT32 capacity;
        UINT32 length;
        UINT64 token;
    } monitors;
};

static inline XTaskQueueHandle queue_impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct XTaskQueueObject, IUnknown_iface );
}

static ULONG WINAPI queue_AddRef( IUnknown *iface )
{
    XTaskQueueHandle impl = queue_impl_from_IUnknown( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI queue_Release( IUnknown *iface )
{
    XTaskQueueHandle impl = queue_impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    if (!ref)
    {
        struct port_binding **link;
        if (impl->work)
        {
            EnterCriticalSection( &impl->work->cs );
            for (link = &impl->work->bindings; *link; link = &(*link)->next)
                if (*link == &impl->work_binding) { *link = (*link)->next; break; }
            LeaveCriticalSection( &impl->work->cs );
        }
        if (impl->completion)
        {
            EnterCriticalSection( &impl->completion->cs );
            for (link = &impl->completion->bindings; *link; link = &(*link)->next)
                if (*link == &impl->completion_binding) { *link = (*link)->next; break; }
            LeaveCriticalSection( &impl->completion->cs );
        }
        DeleteCriticalSection( &impl->monitors.cs );
        DeleteCriticalSection( &impl->cs );
        CloseHandle( impl->cancel_event );
        CloseHandle( impl->finished_event );
        if (impl->work) IUnknown_Release( &impl->work->IUnknown_iface );
        if (impl->completion) IUnknown_Release( &impl->completion->IUnknown_iface );
        if (impl->monitors.entries) free( impl->monitors.entries );
        free( impl );
    }
    return ref;
}

static const struct IUnknownVtbl queue_vtbl =
{
    NULL,
    queue_AddRef,
    queue_Release,
};

struct x_threading
{
    IXThreadingImpl IXThreadingImpl_iface;
    LONG ref;
};

static inline struct x_threading *impl_from_IXThreadingImpl( IXThreadingImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_threading, IXThreadingImpl_iface );
}

static HRESULT WINAPI x_threading_QueryInterface( IXThreadingImpl *iface, REFIID iid, void **out )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXThreadingImpl ))
    {
        IXThreadingImpl_AddRef( *out = &impl->IXThreadingImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_threading_AddRef( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_threading_Release( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

struct async_state
{
    XAsyncProviderData data;
    XAsyncBlock provider_block;
    XAsyncBlock *user_block;
    XAsyncProvider *provider;
    XAsyncCompletionRoutine *callback;
    CRITICAL_SECTION cs;
    LONG active_provider;
    BOOL cleanup_requested;
    BOOL completed;
    HRESULT status;
    LONG cleaned;
    XTaskQueueHandle retained_queue;
    BOOL completion_finished;
    BOOL provider_cleanup_finished;
    DWORD completion_thread;
};

static inline struct async_state *async_state( XAsyncBlock *asyncBlock )
{
    return (struct async_state *)asyncBlock->internal[0];
}

/* Like the native GDK and mixutin's queue-lifetime fix, Begin owns a queue
 * reference until provider cleanup and the completion callback both finish. */
static void async_release_queue( struct async_state *state )
{
    XTaskQueueHandle queue = NULL;
    EnterCriticalSection( &state->cs );
    if (state->provider_cleanup_finished && !state->active_provider && state->completion_finished)
    {
        queue = state->retained_queue;
        state->retained_queue = NULL;
    }
    LeaveCriticalSection( &state->cs );
    if (queue) IUnknown_Release( &queue->IUnknown_iface );
}

static void async_cleanup_state( struct async_state *state, XAsyncProvider *provider )
{
    BOOL cleanup = FALSE;
    EnterCriticalSection( &state->cs );
    state->cleanup_requested = TRUE;
    if (!state->active_provider && !state->cleaned)
    {
        state->cleaned = 1;
        cleanup = TRUE;
    }
    LeaveCriticalSection( &state->cs );
    if (cleanup)
    {
        provider( XAsyncOp_Cleanup, &state->data );
        EnterCriticalSection( &state->cs );
        state->provider_cleanup_finished = TRUE;
        LeaveCriticalSection( &state->cs );
    }
    async_release_queue( state );
}

static HRESULT async_invoke_provider( struct async_state *state, XAsyncOp op )
{
    HRESULT hr;
    BOOL cleanup = FALSE;
    EnterCriticalSection( &state->cs );
    if (state->cleaned)
    {
        LeaveCriticalSection( &state->cs );
        return E_ABORT;
    }
    ++state->active_provider;
    LeaveCriticalSection( &state->cs );
    hr = state->provider( op, &state->data );
    EnterCriticalSection( &state->cs );
    if (!--state->active_provider && state->cleanup_requested && !state->cleaned)
    {
        state->cleaned = 1;
        cleanup = TRUE;
    }
    LeaveCriticalSection( &state->cs );
    if (cleanup)
    {
        state->provider( XAsyncOp_Cleanup, &state->data );
        EnterCriticalSection( &state->cs );
        state->provider_cleanup_finished = TRUE;
        LeaveCriticalSection( &state->cs );
    }
    async_release_queue( state );
    return hr;
}

static void async_cleanup( XAsyncBlock *asyncBlock )
{
    async_cleanup_state( async_state( asyncBlock ), asyncBlock->internal[1] );
}

static HRESULT WINAPI x_threading_XAsyncGetStatus( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, BOOLEAN wait )
{
    struct async_state *state;
    HRESULT result;
    TRACE( "iface %p, asyncBlock %p, wait %d.\n", iface, asyncBlock, wait );
    if (!asyncBlock || !asyncBlock->internal[0] || !asyncBlock->internal[3]) return E_INVALIDARG;
    state = async_state( asyncBlock );
    /* External waiters still wait for callback side effects. A completion
     * callback may query its own result with wait=true (XBL/HC cleanup does
     * this); waiting for that same callback to return would deadlock. */
    EnterCriticalSection( &state->cs );
    BOOL own_callback = state->completion_thread == GetCurrentThreadId();
    LeaveCriticalSection( &state->cs );
    if (wait && !own_callback && WaitForSingleObject( state->provider_block.internal[3], INFINITE ) == WAIT_FAILED)
        return HRESULT_FROM_WIN32( GetLastError() );
    EnterCriticalSection( &state->cs );
    result = state->status;
    LeaveCriticalSection( &state->cs );
    TRACE( "asyncBlock %p -> status %#lx.\n", asyncBlock, result );
    return result;
}

static HRESULT WINAPI x_threading_XAsyncGetResultSize( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *bufferSize )
{
    struct async_state *state;
    HRESULT result;
    TRACE( "iface %p, asyncBlock %p, bufferSize %p.\n", iface, asyncBlock, bufferSize );
    if (!asyncBlock || !asyncBlock->internal[0] || !bufferSize) return E_INVALIDARG;
    state = async_state( asyncBlock );
    EnterCriticalSection( &state->cs );
    *bufferSize = state->data.bufferSize;
    result = state->status;
    LeaveCriticalSection( &state->cs );
    return result;
}

static void WINAPI x_threading_XAsyncCancel( IXThreadingImpl *iface, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );
    if (!asyncBlock || !asyncBlock->internal[0]) return;
    /* Cancellation is best-effort: notify the actual provider, which owns
     * cancellation/completion of its IO. Do not fabricate terminal success or
     * force E_ABORT over a provider's already completed result. Provider call
     * lifetime defers Cleanup if cancellation races in-flight work. */
    async_invoke_provider( async_state( asyncBlock ), XAsyncOp_Cancel );
}

static HRESULT WINAPI run_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    HRESULT hr = S_OK;
    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
        case XAsyncOp_DoWork:
            hr = ((XAsyncWork *)data->context)( data->async );
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, 0 );
            hr = S_OK;
        case XAsyncOp_GetResult:
        case XAsyncOp_Cancel:
        case XAsyncOp_Cleanup:
            break;
    }
    return hr;
}

static HRESULT WINAPI x_threading_XAsyncRun( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, XAsyncWork *work )
{
    TRACE( "iface %p, asyncBlock %p, work %p.\n", iface, asyncBlock, work );
    return IXThreadingImpl_XAsyncBegin( iface, asyncBlock, work, NULL, "XAsyncRun", run_provider );
}

static HRESULT WINAPI x_threading_XAsyncBegin( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, void *context, const void *identity, const char *identityName, XAsyncProvider *provider )
{
    struct async_state *state;
    XAsyncProviderData *data;
    HRESULT hr;
    HANDLE event;
    XTaskQueueHandle queue;

    TRACE( "iface %p, asyncBlock %p, context %p, identity %p, identityName %s, provider %p.\n", iface, asyncBlock, context, identity, identityName, provider );

    if (!asyncBlock || !provider) return E_INVALIDARG;
    queue = asyncBlock->queue;
    if (queue) IUnknown_AddRef( &queue->IUnknown_iface );
    else if (!IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( iface, &queue )) return E_INVALIDARG;
    if (!(state = calloc( 1, sizeof(*state) )))
    {
        IUnknown_Release( &queue->IUnknown_iface );
        return E_OUTOFMEMORY;
    }
    if (!(event = CreateEventA( NULL, TRUE, FALSE, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        free( state );
        IUnknown_Release( &queue->IUnknown_iface );
        return hr;
    }
    InitializeCriticalSection( &state->cs );
    state->status = E_PENDING;
    state->user_block = asyncBlock;
    state->provider = provider;
    state->callback = asyncBlock->callback;
    state->provider_block = *asyncBlock;
    state->provider_block.queue = queue;
    state->retained_queue = queue;
    data = &state->data;
    asyncBlock->internal[0] = data;
    asyncBlock->internal[1] = provider;
    asyncBlock->internal[2] = (void *)identity;
    asyncBlock->internal[3] = event;
    memcpy( state->provider_block.internal, asyncBlock->internal, sizeof(asyncBlock->internal) );
    data->async = &state->provider_block;
    data->context = context;
    hr = async_invoke_provider( state, XAsyncOp_Begin );
    TRACE( "provider %p, op %u, asyncBlock %p -> result %#lx.\n", provider, XAsyncOp_Begin, asyncBlock, hr );
    /* Begin reports state-allocation success. Provider failures complete the
     * operation asynchronously; they are not the return value of Begin. */
    if (FAILED(hr)) IXThreadingImpl_XAsyncComplete( iface, &state->provider_block, hr, 0 );
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXThreadingImpl *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static void CALLBACK async_work_callback( struct async_state *state, BOOLEAN canceled )
{
    HRESULT hr;
    /* Callers may delete/reuse their block inside completion. Providers and
     * queued work must use the runtime-owned copy, never that user memory. */
    if (canceled)
    {
        async_invoke_provider( state, XAsyncOp_Cancel );
        IXThreadingImpl_XAsyncComplete( x_threading_impl, &state->provider_block, E_ABORT, 0 );
        return;
    }
    hr = async_invoke_provider( state, XAsyncOp_DoWork );
    TRACE( "provider %p, op %u, asyncBlock %p -> result %#lx.\n", state->provider, XAsyncOp_DoWork, &state->provider_block, hr );
    if (hr != E_PENDING)
        IXThreadingImpl_XAsyncComplete( x_threading_impl, &state->provider_block, SUCCEEDED(hr) ? E_UNEXPECTED : hr, 0 );
}

static HRESULT WINAPI x_threading_XAsyncSchedule( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, UINT32 delayInMs )
{
    struct async_state *state;
    XTaskQueueHandle queue;
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, delayInMs %d.\n", iface, asyncBlock, delayInMs );
    if (!asyncBlock || !asyncBlock->internal[0]) return E_INVALIDARG;
    state = async_state( asyncBlock );
    EnterCriticalSection( &state->cs );
    /* Native XAsync rejects missing/completed state, preserving the original
     * failure rather than dereferencing a detached block or restarting work. */
    if (state->completed || state->cleaned || !state->retained_queue)
    {
        hr = FAILED(state->status) && state->status != E_PENDING ? state->status : E_INVALIDARG;
        LeaveCriticalSection( &state->cs );
        return hr;
    }
    queue = state->retained_queue;
    IUnknown_AddRef( &queue->IUnknown_iface );
    LeaveCriticalSection( &state->cs );
    /* Immediate completion may release the operation's queue during Submit. */
    hr = IXThreadingImpl_XTaskQueueSubmitDelayedCallback( iface, queue, XTaskQueuePort_Work,
            delayInMs, state, (XTaskQueueCallback *)async_work_callback );
    IUnknown_Release( &queue->IUnknown_iface );
    return hr;
}

static void CALLBACK async_completion_callback( struct async_state *state, BOOLEAN canceled )
{
    if (canceled) async_invoke_provider( state, XAsyncOp_Cancel );
    EnterCriticalSection( &state->cs );
    state->completion_thread = GetCurrentThreadId();
    LeaveCriticalSection( &state->cs );
    if (!canceled) state->callback( state->user_block );
    EnterCriticalSection( &state->cs );
    state->completion_thread = 0;
    state->completion_finished = TRUE;
    LeaveCriticalSection( &state->cs );
    SetEvent( state->provider_block.internal[3] );
    /* The application may delete or reuse XAsyncBlock inside its completion
     * callback. Only the runtime-owned state may be touched after it returns.
     * GetResult may already have cleaned the provider; cleaned makes this
     * idempotent without dereferencing the caller's freed block. */
    if (canceled || FAILED(state->status) || !state->data.bufferSize)
        async_cleanup_state( state, state->provider );
    async_release_queue( state );
}

static void WINAPI x_threading_XAsyncComplete( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, HRESULT result, SIZE_T requiredBufferSize )
{
    struct async_state *state = async_state( asyncBlock );
    TRACE( "iface %p, asyncBlock %p, result %#lx, requiredBufferSize %Iu.\n", iface, asyncBlock, result, requiredBufferSize );
    if (result == E_PENDING) return;
    EnterCriticalSection( &state->cs );
    if (state->completed)
    {
        LeaveCriticalSection( &state->cs );
        return;
    }
    state->completed = TRUE;
    state->data.bufferSize = SUCCEEDED(result) ? requiredBufferSize : 0;
    state->status = result;
    LeaveCriticalSection( &state->cs );
    if (state->callback &&
        SUCCEEDED(IXThreadingImpl_XTaskQueueSubmitCallback( iface, state->provider_block.queue, XTaskQueuePort_Completion, state, (XTaskQueueCallback *)async_completion_callback )))
        return;
    EnterCriticalSection( &state->cs );
    state->completion_finished = TRUE;
    LeaveCriticalSection( &state->cs );
    SetEvent( state->provider_block.internal[3] );
    if (FAILED(result) || !requiredBufferSize) async_cleanup_state( state, state->provider );
    async_release_queue( state );
}

static HRESULT WINAPI x_threading_XAsyncGetResult( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, const void *identity, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    struct async_state *state;
    SIZE_T required;
    HRESULT hr;
    TRACE( "iface %p asyncBlock %p, identity %p, bufferSize %Iu, buffer %p, bufferUsed %p.\n", iface, asyncBlock, identity, bufferSize, buffer, bufferUsed );
    if (FAILED(hr = x_threading_XAsyncGetStatus( iface, asyncBlock, FALSE ))) return hr;
    state = async_state( asyncBlock );
    required = state->data.bufferSize;
    if (!required)
    {
        if (bufferUsed) *bufferUsed = 0;
        return S_OK;
    }
    if (identity != asyncBlock->internal[2] || !buffer) return E_INVALIDARG;
    if (bufferSize < required) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    /* GDK supplies the caller's capacity to GetResult, not the minimum
     * payload size reported at Complete. Keep bufferUsed at that minimum. */
    state->data.bufferSize = bufferSize;
    state->data.buffer = buffer;
    hr = async_invoke_provider( state, XAsyncOp_GetResult );
    state->data.bufferSize = required;
    if (bufferUsed) *bufferUsed = SUCCEEDED(hr) ? required : 0;
    async_cleanup( asyncBlock );
    return hr;
}

/* Monitors are submission notifications, not dispatch callbacks. Serialize
 * notification with unregistration so a removed context cannot be used later.
 * GDK monitor callbacks must be short and must not wait for queue work. */
static void invoke_monitors( XTaskQueueHandle queue, XTaskQueuePort port )
{
    UINT64 next = 0, limit;
    EnterCriticalSection( &queue->monitors.cs );
    limit = queue->monitors.token;
    while (next < limit)
    {
        struct monitor_context monitor;
        BOOL found = FALSE;
        for (UINT32 i = 0; i < queue->monitors.length; ++i)
        {
            if (queue->monitors.entries[i].token.token >= next &&
                queue->monitors.entries[i].token.token < limit)
            {
                monitor = queue->monitors.entries[i];
                found = TRUE;
                break;
            }
        }
        if (!found) break;
        next = monitor.token.token + 1;
        monitor.callback( monitor.callbackContext, queue, port );
    }
    LeaveCriticalSection( &queue->monitors.cs );
}

static HRESULT enqueue_entry( struct task_context *entry );

static void attach_port( XTaskQueuePortHandle port, struct port_binding *binding,
                         XTaskQueueHandle queue, XTaskQueuePort kind )
{
    binding->queue = queue;
    binding->kind = kind;
    EnterCriticalSection( &port->cs );
    binding->next = port->bindings;
    port->bindings = binding;
    LeaveCriticalSection( &port->cs );
}

static void notify_port( XTaskQueuePortHandle port )
{
    struct port_binding *binding, *snapshot;
    UINT32 count = 0, used = 0;
    EnterCriticalSection( &port->cs );
    for (binding = port->bindings; binding; binding = binding->next) ++count;
    snapshot = calloc( count, sizeof(*snapshot) );
    if (snapshot)
    {
        for (binding = port->bindings; binding; binding = binding->next)
        {
            LONG ref;
            /* A final release can be waiting to detach under this lock.
             * Do not resurrect an object whose refcount already reached zero. */
            do { ref = InterlockedCompareExchange( &binding->queue->ref, 0, 0 ); }
            while (ref && InterlockedCompareExchange( &binding->queue->ref, ref + 1, ref ) != ref);
            if (!ref) continue;
            snapshot[used++] = *binding;
        }
    }
    LeaveCriticalSection( &port->cs );
    for (UINT32 i = 0; i < used; ++i)
    {
        invoke_monitors( snapshot[i].queue, snapshot[i].kind );
        IUnknown_Release( &snapshot[i].queue->IUnknown_iface );
    }
    free( snapshot );
}

static void queue_maybe_finish( XTaskQueueHandle queue )
{
    struct task_context *entry = NULL;
    EnterCriticalSection( &queue->cs );
    if (queue->terminated && !queue->pending && queue->termination_entry)
    {
        entry = queue->termination_entry;
        queue->termination_entry = NULL;
    }
    LeaveCriticalSection( &queue->cs );
    if (entry && FAILED(enqueue_entry( entry )))
    {
        /* Never run the final callback on a different dispatch port. */
        ERR( "Cannot schedule queue termination notification.\n" );
        EnterCriticalSection( &queue->cs );
        queue->termination_entry = entry;
        LeaveCriticalSection( &queue->cs );
    }
}

static void execute_entry( struct task_context *entry )
{
    XTaskQueueHandle queue = entry->queue;
    XTaskQueuePortHandle port = entry->port;
    TRACE( "executing queue %p port %p callback %p context %p.\n", queue, port, entry->callback, entry->callbackContext );
    if (entry->termination)
    {
        TRACE( "queue %p drained; notifying on completion port.\n", queue );
        if (queue->terminatedCallback) queue->terminatedCallback( queue->terminatedContext );
        SetEvent( queue->finished_event );
    }
    else
    {
        entry->callback( entry->callbackContext, WaitForSingleObject( queue->cancel_event, 0 ) == WAIT_OBJECT_0 );
        EnterCriticalSection( &queue->cs );
        --queue->pending;
        LeaveCriticalSection( &queue->cs );
        queue_maybe_finish( queue );
    }
    free( entry );
    IUnknown_Release( &port->IUnknown_iface );
    IUnknown_Release( &queue->IUnknown_iface );
}

static void CALLBACK work_handler( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct task_context *entry = context;
    execute_entry( entry );
}

static void CALLBACK delayed_handler( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct task_context *entry = context;
    XTaskQueueHandle queue = entry->queue;
    XTaskQueuePortHandle port = entry->port;
    ULONGLONG now = GetTickCount64();
    if (entry->due > now)
        WaitForSingleObject( queue->cancel_event, (DWORD)(entry->due - now) );
    entry->due = 0;
    if (FAILED(enqueue_entry( entry )))
    {
        ERR( "Cannot publish delayed callback.\n" );
        EnterCriticalSection( &queue->cs );
        --queue->pending;
        LeaveCriticalSection( &queue->cs );
        free( entry );
        queue_maybe_finish( queue );
        IUnknown_Release( &port->IUnknown_iface );
        IUnknown_Release( &queue->IUnknown_iface );
    }
}

/* The port lock protects both the list and the single serialized worker.
 * No callback is invoked with this lock held. Composite queues retain their
 * own cancellation state; terminating one must not cancel its parent's tasks. */
static struct task_context *take_ready_entry( XTaskQueuePortHandle port, DWORD *timeout )
{
    struct task_context **link, *entry;
    ULONGLONG now = GetTickCount64();
    *timeout = INFINITE;
    for (link = &port->head; (entry = *link); link = &entry->next)
    {
        if (entry->termination || entry->due <= now ||
            WaitForSingleObject( entry->queue->cancel_event, 0 ) == WAIT_OBJECT_0)
        {
            *link = entry->next;
            return entry;
        }
        if (entry->due - now < *timeout) *timeout = (DWORD)(entry->due - now);
    }
    return NULL;
}

static void CALLBACK dispatch_handler( TP_CALLBACK_INSTANCE *instance, void *context )
{
    XTaskQueuePortHandle port = context;
    for (;;)
    {
        struct task_context *entry;
        DWORD timeout;
        EnterCriticalSection( &port->cs );
        entry = take_ready_entry( port, &timeout );
        if (!entry && !port->head)
        {
            port->worker_active = FALSE;
            LeaveCriticalSection( &port->cs );
            break;
        }
        LeaveCriticalSection( &port->cs );
        if (entry) execute_entry( entry );
        else WaitForSingleObject( port->wake, timeout );
    }
    IUnknown_Release( &port->IUnknown_iface );
}

static HRESULT enqueue_entry( struct task_context *entry )
{
    XTaskQueuePortHandle port = entry->port;
    struct task_context **tail;
    HRESULT hr = S_OK;
    IUnknown_AddRef( &port->IUnknown_iface );
    if (port->mode == XTaskQueueDispatchMode_Immediate && entry->due <= GetTickCount64())
    {
        notify_port( port );
        execute_entry( entry );
        IUnknown_Release( &port->IUnknown_iface );
        return S_OK;
    }
    if (port->mode == XTaskQueueDispatchMode_ThreadPool)
    {
        if (!TrySubmitThreadpoolCallback( work_handler, entry, NULL ))
            hr = HRESULT_FROM_WIN32( GetLastError() );
        if (SUCCEEDED(hr)) notify_port( port );
        IUnknown_Release( &port->IUnknown_iface );
        return hr;
    }
    EnterCriticalSection( &port->cs );
    if (port->mode != XTaskQueueDispatchMode_Manual && !port->worker_active)
    {
        port->worker_active = TRUE;
        IUnknown_AddRef( &port->IUnknown_iface );
        if (!TrySubmitThreadpoolCallback( dispatch_handler, port, NULL ))
        {
            port->worker_active = FALSE;
            IUnknown_Release( &port->IUnknown_iface );
            LeaveCriticalSection( &port->cs );
            IUnknown_Release( &port->IUnknown_iface );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }
    for (tail = &port->head; *tail; tail = &(*tail)->next) ;
    *tail = entry;
    SetEvent( port->wake );
    LeaveCriticalSection( &port->cs );
    notify_port( port );
    IUnknown_Release( &port->IUnknown_iface );
    return S_OK;
}

static HRESULT create_port( XTaskQueueDispatchMode mode, XTaskQueuePortHandle *out )
{
    XTaskQueuePortHandle port;
    *out = NULL;
    if (mode != XTaskQueueDispatchMode_Manual && mode != XTaskQueueDispatchMode_ThreadPool &&
        mode != XTaskQueueDispatchMode_SerializedThreadPool && mode != XTaskQueueDispatchMode_Immediate)
        return E_INVALIDARG;
    if (!(port = calloc( 1, sizeof(*port) ))) return E_OUTOFMEMORY;
    port->IUnknown_iface.lpVtbl = &port_vtbl;
    port->ref = 1;
    port->mode = mode;
    InitializeCriticalSection( &port->cs );
    port->wake = CreateEventW( NULL, FALSE, FALSE, NULL );
    if (!port->wake)
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        IUnknown_Release( &port->IUnknown_iface );
        return hr;
    }
    *out = port;
    return S_OK;
}

static HRESULT create_queue( XTaskQueueHandle *out )
{
    XTaskQueueHandle queue;
    *out = NULL;
    if (!(queue = calloc( 1, sizeof(*queue) ))) return E_OUTOFMEMORY;
    queue->IUnknown_iface.lpVtbl = &queue_vtbl;
    queue->ref = 1;
    queue->monitors.capacity = 32;
    InitializeCriticalSection( &queue->monitors.cs );
    InitializeCriticalSection( &queue->cs );
    queue->cancel_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    queue->finished_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    queue->monitors.entries = calloc( 32, sizeof(*queue->monitors.entries) );
    if (!queue->cancel_event || !queue->finished_event || !queue->monitors.entries)
    {
        IUnknown_Release( &queue->IUnknown_iface );
        return E_OUTOFMEMORY;
    }
    *out = queue;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueCreate( IXThreadingImpl *iface, XTaskQueueDispatchMode workMode, XTaskQueueDispatchMode completionMode, XTaskQueueHandle *out )
{
    XTaskQueueHandle queue;
    HRESULT hr;
    TRACE( "iface %p, workMode %d, completionMode %d, out %p.\n", iface, workMode, completionMode, out );
    if (!out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = create_queue( &queue ))) return hr;
    if (FAILED(hr = create_port( workMode, &queue->work )) ||
        FAILED(hr = create_port( completionMode, &queue->completion )))
    {
        IUnknown_Release( &queue->IUnknown_iface );
        return hr;
    }
    attach_port( queue->work, &queue->work_binding, queue, XTaskQueuePort_Work );
    attach_port( queue->completion, &queue->completion_binding, queue, XTaskQueuePort_Completion );
    *out = queue;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueCreateComposite( IXThreadingImpl *iface, XTaskQueuePortHandle workPort, XTaskQueuePortHandle completionPort, XTaskQueueHandle *out )
{
    XTaskQueueHandle queue;
    HRESULT hr;
    TRACE( "iface %p, workPort %p, completionPort %p, out %p.\n", iface, workPort, completionPort, out );
    if (!out) return E_POINTER;
    *out = NULL;
    if (!workPort || !completionPort) return E_INVALIDARG;
    if (FAILED(hr = create_queue( &queue ))) return hr;
    queue->work = workPort;
    queue->completion = completionPort;
    queue->composite = TRUE;
    IUnknown_AddRef( &workPort->IUnknown_iface );
    IUnknown_AddRef( &completionPort->IUnknown_iface );
    attach_port( workPort, &queue->work_binding, queue, XTaskQueuePort_Work );
    attach_port( completionPort, &queue->completion_binding, queue, XTaskQueuePort_Completion );
    *out = queue;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueGetPort( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle *out )
{
    TRACE( "iface %p, queue %p, port %d, out %p.\n", iface, queue, port, out );
    if (!queue || !out) return E_POINTER;
    switch (port)
    {
        case XTaskQueuePort_Work: *out = queue->work; return S_OK;
        case XTaskQueuePort_Completion: *out = queue->completion; return S_OK;
        default: return E_INVALIDARG;
    }
}

static HRESULT WINAPI x_threading_XTaskQueueDuplicateHandle( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueHandle *out )
{
    TRACE( "iface %p, queue %p, out %p.\n", iface, queue, out );
    if (!queue || !out) return E_POINTER;
    IUnknown_AddRef( &queue->IUnknown_iface );
    *out = queue;
    return S_OK;
}

static BOOLEAN WINAPI x_threading_XTaskQueueDispatch( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort kind, UINT32 timeoutInMs )
{
    XTaskQueuePortHandle port;
    ULONGLONG start = GetTickCount64();
    BOOL acquired = FALSE, result = FALSE;
    static LONG logged;
    if (InterlockedIncrement( &logged ) <= 50) TRACE( "queue %p, kind %d, timeoutInMs %u, vtbl %p.\n", queue, kind, timeoutInMs, queue ? *(void ***)queue : NULL );
    if (!queue)
    {
        if (!IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( iface, &queue )) return FALSE;
        acquired = TRUE;
    }
    else
    {
        IUnknown_AddRef( &queue->IUnknown_iface );
        acquired = TRUE;
    }
    if (kind != XTaskQueuePort_Work && kind != XTaskQueuePort_Completion) goto done;
    port = kind == XTaskQueuePort_Work ? queue->work : queue->completion;
    if (port->mode != XTaskQueueDispatchMode_Manual) goto done;
    IUnknown_AddRef( &port->IUnknown_iface );
    for (;;)
    {
        struct task_context *entry;
        DWORD timeout;
        EnterCriticalSection( &port->cs );
        entry = take_ready_entry( port, &timeout );
        LeaveCriticalSection( &port->cs );
        if (entry)
        {
            execute_entry( entry );
            result = TRUE;
            break;
        }
        /* INFINITE dispatch is also used to drain a terminating queue. The
         * final callback must finish before returning FALSE. This event is
         * queue-scoped, so terminating a composite does not stop its parent
         * or another queue sharing the same physical port. Keep the queue
         * retained above while other dispatchers may close its last handle. */
        if (WaitForSingleObject( queue->finished_event, 0 ) == WAIT_OBJECT_0) break;
        if (timeoutInMs != INFINITE && GetTickCount64() - start >= timeoutInMs) break;
        /* Multiple manual dispatchers may wait on this port. Poll boundedly
         * so one consumer cannot steal another consumer's wake indefinitely. */
        Sleep( 1 );
    }
    IUnknown_Release( &port->IUnknown_iface );
done:
    if (acquired) IUnknown_Release( &queue->IUnknown_iface );
    return result;
}

static void WINAPI x_threading_XTaskQueueCloseHandle( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    TRACE( "iface %p, queue %p.\n", iface, queue );
    if (queue) IUnknown_Release( &queue->IUnknown_iface );
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, void *context, XTaskQueueCallback *callback )
{
    TRACE( "queue %p, port %d, context %p, callback %p, vtbl %p.\n", queue, port, context, callback, queue ? *(void ***)queue : NULL );
    return IXThreadingImpl_XTaskQueueSubmitDelayedCallback( iface, queue, port, 0, context, callback );
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitDelayedCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort kind, UINT32 delayMs, void *context, XTaskQueueCallback *callback )
{
    struct task_context *entry;
    XTaskQueuePortHandle port;
    HRESULT hr;
    TRACE( "queue %p, kind %d, delayMs %u, context %p, callback %p, workMode %d compMode %d.\n", queue, kind, delayMs, context, callback,
           queue ? queue->work->mode : -1, queue ? queue->completion->mode : -1 );
    if (!queue || !callback) return E_POINTER;
    if (kind != XTaskQueuePort_Work && kind != XTaskQueuePort_Completion) return E_INVALIDARG;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    port = kind == XTaskQueuePort_Work ? queue->work : queue->completion;
    entry->queue = queue;
    entry->port = port;
    entry->logical_port = kind;
    entry->callback = callback;
    entry->callbackContext = context;
    entry->due = GetTickCount64() + delayMs;
    EnterCriticalSection( &queue->cs );
    if (queue->terminated)
    {
        LeaveCriticalSection( &queue->cs );
        free( entry );
        return E_ABORT;
    }
    ++queue->pending;
    IUnknown_AddRef( &queue->IUnknown_iface );
    IUnknown_AddRef( &port->IUnknown_iface );
    LeaveCriticalSection( &queue->cs );
    /* The entry may execute synchronously, or the callback may close the last
     * public handle. Keep a separate reference through monitor notification. */
    IUnknown_AddRef( &queue->IUnknown_iface );
    if (delayMs)
        hr = TrySubmitThreadpoolCallback( delayed_handler, entry, NULL ) ? S_OK : HRESULT_FROM_WIN32( GetLastError() );
    else hr = enqueue_entry( entry );
    if (FAILED(hr))
    {
        EnterCriticalSection( &queue->cs );
        --queue->pending;
        LeaveCriticalSection( &queue->cs );
        free( entry );
        IUnknown_Release( &port->IUnknown_iface );
        queue_maybe_finish( queue );
        IUnknown_Release( &queue->IUnknown_iface );
    }
    IUnknown_Release( &queue->IUnknown_iface );
    return hr;
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, HANDLE waitHandle, void *callbackContext, XTaskQueueCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, port %d, waitHandle %p, callbackContext %p, callback %p, token %p stub!\n", iface, queue, port, waitHandle, callbackContext, callback, token );
    return E_NOTIMPL;
}

static void WINAPI x_threading_XTaskQueueUnregisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    FIXME( "iface %p, queue %p, token %p stub!\n", iface, queue, &token );
}

static HRESULT WINAPI x_threading_XTaskQueueTerminate( IXThreadingImpl *iface, XTaskQueueHandle queue, BOOLEAN wait, void *context, XTaskQueueTerminatedCallback *callback )
{
    struct task_context *entry;
    TRACE( "queue %p, wait %d, context %p, callback %p.\n", queue, wait, context, callback );
    if (!queue) return E_POINTER;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    IUnknown_AddRef( &queue->IUnknown_iface );
    EnterCriticalSection( &queue->cs );
    if (queue->terminated)
    {
        LeaveCriticalSection( &queue->cs );
        free( entry );
        IUnknown_Release( &queue->IUnknown_iface );
        return E_ABORT;
    }
    queue->terminated = TRUE;
    queue->terminatedContext = context;
    queue->terminatedCallback = callback;
    entry->queue = queue;
    entry->port = queue->completion;
    entry->logical_port = XTaskQueuePort_Completion;
    entry->termination = TRUE;
    queue->termination_entry = entry;
    IUnknown_AddRef( &queue->IUnknown_iface );
    IUnknown_AddRef( &queue->completion->IUnknown_iface );
    SetEvent( queue->cancel_event );
    LeaveCriticalSection( &queue->cs );
    SetEvent( queue->work->wake );
    SetEvent( queue->completion->wake );
    queue_maybe_finish( queue );
    /* As on the GDK, manual ports must be pumped by another thread when
     * requesting synchronous termination. Do not dispatch on the wrong thread. */
    if (wait) WaitForSingleObject( queue->finished_event, INFINITE );
    IUnknown_Release( &queue->IUnknown_iface );
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, void *callbackContext, XTaskQueueMonitorCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, callbackContext %p, callback %p, token %p.\n", iface, queue, callbackContext, callback, token );

    if (!queue || !callback || !token) return E_POINTER;
    EnterCriticalSection( &queue->monitors.cs );
    if (queue->monitors.capacity == queue->monitors.length)
    {
        UINT32 capacity = queue->monitors.capacity * 1.5;
        struct monitor_context *entries;
        if (!(entries = realloc( queue->monitors.entries, capacity * sizeof(*entries) )))
        {
            LeaveCriticalSection( &queue->monitors.cs );
            return E_OUTOFMEMORY;
        }
        queue->monitors.capacity = capacity;
        queue->monitors.entries = entries;
    }
    queue->monitors.entries[queue->monitors.length].callback = callback;
    queue->monitors.entries[queue->monitors.length].callbackContext = callbackContext;
    token->token = queue->monitors.token++;
    queue->monitors.entries[queue->monitors.length++].token = *token;
    IUnknown_AddRef( &queue->IUnknown_iface );
    LeaveCriticalSection( &queue->monitors.cs );
    return S_OK;
}

static void WINAPI x_threading_XTaskQueueUnregisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    BOOL removed = FALSE;
    TRACE( "iface %p, queue %p, token %p.\n", iface, queue, &token );

    if (!queue) return;
    EnterCriticalSection( &queue->monitors.cs );
    for (UINT32 i = 0; i < queue->monitors.length; i++)
    {
        if (queue->monitors.entries[i].token.token == token.token)
        {
            memmove( queue->monitors.entries + i, queue->monitors.entries + i + 1, (--queue->monitors.length - i) * sizeof(*queue->monitors.entries) );
            removed = TRUE;
            break;
        }
    }
    LeaveCriticalSection( &queue->monitors.cs );
    if (removed) IUnknown_Release( &queue->IUnknown_iface );
}

static BOOLEAN defaultCreated;

static BOOLEAN WINAPI x_threading_XTaskQueueGetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle *queue )
{
    TRACE( "iface %p, queue %p.\n", iface, queue );
    EnterCriticalSection( &processQueueSection );
    if (!processQueue && !defaultCreated)
    {
        defaultCreated = TRUE;
        if (FAILED(IXThreadingImpl_XTaskQueueCreate( iface, XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &processQueue ))) processQueue = NULL;
    }
    if (!processQueue)
    {
        LeaveCriticalSection( &processQueueSection );
        return FALSE;
    }
    IUnknown_AddRef( &processQueue->IUnknown_iface );
    *queue = processQueue;
    LeaveCriticalSection( &processQueueSection );
    return TRUE;
}

static void WINAPI x_threading_XTaskQueueSetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    TRACE( "iface %p, queue %p.\n", iface, queue );
    EnterCriticalSection( &processQueueSection );
    defaultCreated = TRUE;
    if (queue) IUnknown_AddRef( &queue->IUnknown_iface );
    if (processQueue) IUnknown_Release( &processQueue->IUnknown_iface );
    processQueue = queue;
    LeaveCriticalSection( &processQueueSection );
}

static HRESULT WINAPI x_threading_XThreadSetTimeSensitive( IXThreadingImpl *iface, BOOLEAN isTimeSensitiveThread )
{
    TRACE( "iface %p, isTimeSensitiveThread %d.\n", iface, isTimeSensitiveThread );
    if (!TlsSetValue( tlsIndex, (void *)(UINT_PTR)isTimeSensitiveThread )) return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static void WINAPI x_threading_XThreadAssertNotTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    if (TlsGetValue( tlsIndex )) DebugBreak();
}

static BOOLEAN WINAPI x_threading_XThreadIsTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    return TlsGetValue( tlsIndex ) ? 1 : 0;
}

static const struct IXThreadingImplVtbl x_threading_vtbl =
{
    x_threading_QueryInterface,
    x_threading_AddRef,
    x_threading_Release,
    /* IXThreadingImpl methods */
    x_threading_XAsyncGetStatus,
    x_threading_XAsyncGetResultSize,
    x_threading_XAsyncCancel,
    x_threading_XAsyncRun,
    x_threading_XAsyncBegin,
    __PADDING__,
    x_threading_XAsyncSchedule,
    x_threading_XAsyncComplete,
    x_threading_XAsyncGetResult,
    x_threading_XTaskQueueCreate,
    x_threading_XTaskQueueCreateComposite,
    x_threading_XTaskQueueGetPort,
    x_threading_XTaskQueueDuplicateHandle,
    x_threading_XTaskQueueDispatch,
    x_threading_XTaskQueueCloseHandle,
    x_threading_XTaskQueueSubmitCallback,
    x_threading_XTaskQueueSubmitDelayedCallback,
    x_threading_XTaskQueueRegisterWaiter,
    x_threading_XTaskQueueUnregisterWaiter,
    x_threading_XTaskQueueTerminate,
    x_threading_XTaskQueueRegisterMonitor,
    x_threading_XTaskQueueUnregisterMonitor,
    x_threading_XTaskQueueGetCurrentProcessTaskQueue,
    x_threading_XTaskQueueSetCurrentProcessTaskQueue,
    x_threading_XThreadSetTimeSensitive,
    __PADDING__,
    x_threading_XThreadAssertNotTimeSensitive,
    x_threading_XThreadIsTimeSensitive
};

static struct x_threading x_threading =
{
    {&x_threading_vtbl},
    0,
};

IXThreadingImpl *x_threading_impl = &x_threading.IXThreadingImpl_iface;
