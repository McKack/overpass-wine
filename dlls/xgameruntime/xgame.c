/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGame
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

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_game
{
    IXGameImpl3 IXGameImpl3_iface;
    LONG ref;
};

static inline struct x_game *impl_from_IXGameImpl3( IXGameImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game, IXGameImpl3_iface );
}

static HRESULT WINAPI x_game_QueryInterface( IXGameImpl3 *iface, REFIID iid, void **out )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXGameImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameImpl3 ))
    {
        IXGameImpl3_AddRef( *out = &impl->IXGameImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_AddRef( IXGameImpl3 *iface )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_Release( IXGameImpl3 *iface )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_game_XGameGetXboxTitleId( IXGameImpl3 *iface, UINT32 *out )
{
    TRACE( "iface %p, out %p, titleId %#x.\n", iface, out, titleId );
    if (!out) return E_POINTER;
    if (!(*out = titleId)) return E_GAME_MISSING_GAME_CONFIG;
    return S_OK;
}

static void WINAPI x_game_XLaunchNewGame( IXGameImpl3 *iface, const char *exePath, const char *args, XUserHandle defaultUser )
{
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process;
    WCHAR *executable = NULL, *arguments = NULL, *command = NULL;
    SIZE_T length;
    int exe_count, args_count = 1;
    DWORD error = ERROR_INVALID_PARAMETER;

    /* The backend supplies one real signed-in account to the process tree.
     * A selected user must be that retained account, never a guessed handle
     * or a request to switch identities in the child. NULL keeps that account. */
    if (defaultUser && !xuser_is_current_user( defaultUser ))
    {
        error = ERROR_NOT_SUPPORTED;
        goto done;
    }
    if (!exePath || !*exePath ||
        !(exe_count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, exePath, -1, NULL, 0 ))) goto done;
    if (args && !(args_count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, args, -1, NULL, 0 ))) goto done;
    /* CreateProcessW's limit includes the quoted executable and terminator. */
    length = (SIZE_T)exe_count + args_count + 2;
    if (length > 32767) goto done;
    if (!(executable = malloc( exe_count * sizeof(WCHAR) )) ||
        !(arguments = calloc( args_count, sizeof(WCHAR) )) ||
        !(command = malloc( length * sizeof(WCHAR) )))
    {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, exePath, -1, executable, exe_count ) ||
        (args && !MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, args, -1, arguments, args_count ))) goto done;
    /* Quotes are not valid filename characters and would corrupt argv[0]. */
    if (wcschr( executable, '"' )) goto done;
    command[0] = '"';
    memcpy( command + 1, executable, (exe_count - 1) * sizeof(WCHAR) );
    command[exe_count] = '"';
    command[exe_count + 1] = ' ';
    memcpy( command + exe_count + 2, arguments, args_count * sizeof(WCHAR) );

    /* No shell or new authentication flow. Keep the current directory and
     * environment (package view, account bridge and runtime) in the new tree,
     * but do not leak the caller's unrelated inheritable Windows handles. */
    if (!CreateProcessW( executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process ))
    {
        error = GetLastError();
        goto done;
    }
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
    error = ERROR_SUCCESS;
done:
    free( command );
    free( arguments );
    free( executable );
    if (error) ERR( "Game relaunch failed, error %lu.\n", error );
    /* The public contract never returns, including invalid-input/launch errors.
     * Normal process teardown retains the runtime's existing save cleanup. */
    ExitProcess( error ? 1 : 0 );
}

static HRESULT WINAPI x_game_XLaunchRestartOnCrash( IXGameImpl3 *iface, const char *args, UINT32 reserved )
{
    FIXME( "iface %p, args %s, reserved %u stub!\n", iface, debugstr_a( args ), reserved );
    return E_NOTIMPL;
}

static const struct IXGameImpl3Vtbl x_game_vtbl =
{
    x_game_QueryInterface,
    x_game_AddRef,
    x_game_Release,
    /* IXGameImpl methods */
    x_game_XGameGetXboxTitleId,
    /* IXGameImpl2 methods */
    x_game_XLaunchNewGame,
    /* IXGameImpl3 methods */
    x_game_XLaunchRestartOnCrash,
};

static struct x_game x_game =
{
    {&x_game_vtbl},
    0,
};

IXGameImpl *x_game_impl = (IXGameImpl *)&x_game.IXGameImpl3_iface;
