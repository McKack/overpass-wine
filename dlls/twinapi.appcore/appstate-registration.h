/* App-state and constrained subscriptions for Wine desktop processes. No
 * lifecycle event source exists here, so registration never invents notifications.
 * This file is part of Wine and is licensed under LGPL-2.1-or-later. */

#include <stdlib.h>

struct appstate_registration
{
    struct appstate_registration *next;
    PAPPSTATE_REGISTRATION handle;
    PAPPSTATE_CHANGE_ROUTINE routine;
    void *context;
    BOOL constrained;
};

static SRWLOCK appstate_lock = SRWLOCK_INIT;
static struct appstate_registration *appstate_registrations;
static ULONG_PTR appstate_next_id;

static ULONG appstate_register( PAPPSTATE_CHANGE_ROUTINE routine, void *context,
                                BOOL constrained, PAPPSTATE_REGISTRATION *out )
{
    struct appstate_registration *registration;

    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (!routine) return ERROR_INVALID_PARAMETER;
    if (!(registration = malloc( sizeof(*registration) ))) return ERROR_NOT_ENOUGH_MEMORY;
    registration->routine = routine;
    registration->context = context;
    registration->constrained = constrained;

    AcquireSRWLockExclusive( &appstate_lock );
    if (appstate_next_id == ~(ULONG_PTR)0)
    {
        ReleaseSRWLockExclusive( &appstate_lock );
        free( registration );
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    registration->handle = (PAPPSTATE_REGISTRATION)++appstate_next_id;
    registration->next = appstate_registrations;
    appstate_registrations = registration;
    *out = registration->handle;
    ReleaseSRWLockExclusive( &appstate_lock );
    return ERROR_SUCCESS;
}

ULONG WINAPI RegisterAppStateChangeNotification( PAPPSTATE_CHANGE_ROUTINE routine,
                                                void *context, PAPPSTATE_REGISTRATION *out )
{
    return appstate_register( routine, context, FALSE, out );
}

ULONG WINAPI RegisterAppConstrainedChangeNotification( PAPPCONSTRAIN_CHANGE_ROUTINE routine,
                                                      void *context, PAPPCONSTRAIN_REGISTRATION *out )
{
    PAPPSTATE_REGISTRATION registration;
    ULONG status;

    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    status = appstate_register( routine, context, TRUE, &registration );
    if (!status) *out = (PAPPCONSTRAIN_REGISTRATION)registration;
    return status;
}

void WINAPI UnregisterAppStateChangeNotification( PAPPSTATE_REGISTRATION handle )
{
    struct appstate_registration **cursor, *registration = NULL;

    AcquireSRWLockExclusive( &appstate_lock );
    for (cursor = &appstate_registrations; *cursor; cursor = &(*cursor)->next)
    {
        if ((*cursor)->handle != handle) continue;
        registration = *cursor;
        *cursor = registration->next;
        break;
    }
    ReleaseSRWLockExclusive( &appstate_lock );
    free( registration );
}

void WINAPI UnregisterAppConstrainedChangeNotification( PAPPCONSTRAIN_REGISTRATION handle )
{
    UnregisterAppStateChangeNotification( (PAPPSTATE_REGISTRATION)handle );
}

/* Called only during DLL unload, when callers must no longer use its APIs. */
static void appstate_free_registrations(void)
{
    struct appstate_registration *registration;

    while ((registration = appstate_registrations))
    {
        appstate_registrations = registration->next;
        free( registration );
    }
}
