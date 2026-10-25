/*
 * OverPass compatibility rules: per-program command-line, environment, DLL override and graphics
 * backend tweaks, applied whenever a process is created.
 *
 * Copyright 2026 OverPass contributors
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

/*
 * The rules file is named by OVERPASS_COMPAT_RULES (a Windows path) in the creating process's
 * environment. It is UTF-8 text, one directive per line, '#' starts a comment:
 *
 *   rule <description>            starts a rule
 *   path_suffix \GalaxyClient.exe matches when the program's full path ends with this (any case)
 *   steam_game_id 1234            matches when the new process's SteamGameId is this
 *   append <arguments>            appended to the command line...
 *   forbidden <text>              ...unless the command line already contains this
 *   env NAME=VALUE                set in the new process's Windows and Unix environment
 *   dll name=n,b                  added to WINEDLLOVERRIDES
 *   backend dxmt|d3dmetal|wined3d sets OVERPASS_DLL_PATH_PREPEND from OVERPASS_BACKEND_<NAME>
 *
 * A rule applies when any of its match lines matches. Rules are applied in file order.
 */

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winternl.h"

#include "kernelbase.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(process);

#define MAX_RULE_LINES 64

struct env_block
{
    WCHAR **vars;   /* "NAME=VALUE" strings, heap allocated */
    unsigned int count, size;
};

static WCHAR *heap_strdupW( const WCHAR *s, size_t len )
{
    WCHAR *ret = HeapAlloc( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) );
    if (ret)
    {
        memcpy( ret, s, len * sizeof(WCHAR) );
        ret[len] = 0;
    }
    return ret;
}

static void env_free( struct env_block *env )
{
    unsigned int i;
    for (i = 0; i < env->count; i++) HeapFree( GetProcessHeap(), 0, env->vars[i] );
    HeapFree( GetProcessHeap(), 0, env->vars );
}

static BOOL env_push( struct env_block *env, WCHAR *var )
{
    if (env->count == env->size)
    {
        unsigned int size = env->size ? env->size * 2 : 64;
        WCHAR **vars = env->vars ? HeapReAlloc( GetProcessHeap(), 0, env->vars, size * sizeof(*vars) )
                                 : HeapAlloc( GetProcessHeap(), 0, size * sizeof(*vars) );
        if (!vars) return FALSE;
        env->vars = vars;
        env->size = size;
    }
    env->vars[env->count++] = var;
    return TRUE;
}

/* Loads a double-null-terminated block; the current process's own if block is NULL. */
static BOOL env_load( struct env_block *env, const void *block, DWORD flags )
{
    WCHAR *strings = NULL, *own = NULL;
    const WCHAR *p;

    if (!block) strings = own = GetEnvironmentStringsW();
    else if (flags & CREATE_UNICODE_ENVIRONMENT) strings = (WCHAR *)block;
    else
    {
        const char *e = block;
        DWORD lenW;
        while (*e) e += strlen( e ) + 1;
        e++;
        lenW = MultiByteToWideChar( CP_ACP, 0, block, e - (const char *)block, NULL, 0 );
        if (!(strings = HeapAlloc( GetProcessHeap(), 0, lenW * sizeof(WCHAR) ))) return FALSE;
        MultiByteToWideChar( CP_ACP, 0, block, e - (const char *)block, strings, lenW );
    }
    if (!strings) return FALSE;

    for (p = strings; *p; p += lstrlenW( p ) + 1)
    {
        WCHAR *var = heap_strdupW( p, lstrlenW( p ) );
        if (!var || !env_push( env, var )) break;
    }

    if (own) FreeEnvironmentStringsW( own );
    else if (strings != block) HeapFree( GetProcessHeap(), 0, strings );
    return TRUE;
}

/* Index of NAME (case-insensitive, as on Windows), or -1. Entries starting with '=' are never matched. */
static int env_find( const struct env_block *env, const WCHAR *name, size_t len )
{
    unsigned int i;
    for (i = 0; i < env->count; i++)
        if (!wcsnicmp( env->vars[i], name, len ) && env->vars[i][len] == '=') return i;
    return -1;
}

static const WCHAR *env_get( const struct env_block *env, const WCHAR *name )
{
    int i = env_find( env, name, lstrlenW( name ) );
    return i < 0 ? NULL : env->vars[i] + lstrlenW( name ) + 1;
}

static void env_set( struct env_block *env, const WCHAR *name, size_t name_len, const WCHAR *value )
{
    size_t value_len = lstrlenW( value );
    WCHAR *var = HeapAlloc( GetProcessHeap(), 0, (name_len + value_len + 2) * sizeof(WCHAR) );
    int i;

    if (!var) return;
    memcpy( var, name, name_len * sizeof(WCHAR) );
    var[name_len] = '=';
    memcpy( var + name_len + 1, value, (value_len + 1) * sizeof(WCHAR) );

    if ((i = env_find( env, name, name_len )) >= 0)
    {
        HeapFree( GetProcessHeap(), 0, env->vars[i] );
        env->vars[i] = var;
    }
    else if (!env_push( env, var )) HeapFree( GetProcessHeap(), 0, var );
}

/* Sets a variable for both sides of the new process: the Windows environment, and through CodeWeavers'
 * __CX_UNIX_ promotion (CW Hack 24717 in ntdll's spawn_process) the Unix environment, where Wine itself
 * reads WINEDLLOVERRIDES, OVERPASS_DLL_PATH_PREPEND and similar settings. */
static void env_set_both( struct env_block *env, const WCHAR *name, size_t name_len, const WCHAR *value )
{
    WCHAR prefixed[128] = L"__CX_UNIX_";
    size_t prefix_len = lstrlenW( prefixed );

    env_set( env, name, name_len, value );
    if (prefix_len + name_len >= ARRAY_SIZE(prefixed)) return;
    memcpy( prefixed + prefix_len, name, name_len * sizeof(WCHAR) );
    env_set( env, prefixed, prefix_len + name_len, value );
}

static WCHAR *env_build( const struct env_block *env )
{
    size_t len = 1;
    unsigned int i;
    WCHAR *block, *p;

    for (i = 0; i < env->count; i++) len += lstrlenW( env->vars[i] ) + 1;
    if (!(block = p = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) ))) return NULL;
    for (i = 0; i < env->count; i++)
    {
        size_t l = lstrlenW( env->vars[i] ) + 1;
        memcpy( p, env->vars[i], l * sizeof(WCHAR) );
        p += l;
    }
    *p = 0;
    return block;
}

/* The rules file as UTF-16 text, or NULL when there is none. */
static WCHAR *load_rules(void)
{
    WCHAR path[MAX_PATH], *text = NULL;
    char *utf8 = NULL;
    DWORD size, read, lenW;
    HANDLE file;

    if (!GetEnvironmentVariableW( L"OVERPASS_COMPAT_RULES", path, ARRAY_SIZE(path) )) return NULL;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        WARN( "can't open %s\n", debugstr_w(path) );
        return NULL;
    }
    size = GetFileSize( file, NULL );
    if (size != INVALID_FILE_SIZE && size < (4 << 20) && (utf8 = HeapAlloc( GetProcessHeap(), 0, size + 1 ))
        && ReadFile( file, utf8, size, &read, NULL ))
    {
        lenW = MultiByteToWideChar( CP_UTF8, 0, utf8, read, NULL, 0 );
        if ((text = HeapAlloc( GetProcessHeap(), 0, (lenW + 1) * sizeof(WCHAR) )))
        {
            MultiByteToWideChar( CP_UTF8, 0, utf8, read, text, lenW );
            text[lenW] = 0;
        }
    }
    HeapFree( GetProcessHeap(), 0, utf8 );
    CloseHandle( file );
    return text;
}

struct rule
{
    const WCHAR *desc;
    const WCHAR *lines[MAX_RULE_LINES];  /* directive lines, keyword included */
    unsigned int count;
};

static const WCHAR *directive( const WCHAR *line, const WCHAR *keyword )
{
    size_t len = lstrlenW( keyword );
    if (wcsncmp( line, keyword, len ) || (line[len] != ' ' && line[len] != '\t')) return NULL;
    line += len;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

static BOOL rule_matches( const struct rule *rule, const WCHAR *app_name, const struct env_block *env )
{
    size_t app_len = lstrlenW( app_name );
    unsigned int i;
    const WCHAR *arg;

    for (i = 0; i < rule->count; i++)
    {
        if ((arg = directive( rule->lines[i], L"path_suffix" )))
        {
            size_t len = lstrlenW( arg );
            if (len && len <= app_len && !wcsicmp( app_name + app_len - len, arg )) return TRUE;
        }
        else if ((arg = directive( rule->lines[i], L"steam_game_id" )))
        {
            const WCHAR *id = env_get( env, L"SteamGameId" );
            if (id && !wcscmp( id, arg )) return TRUE;
        }
    }
    return FALSE;
}

static void rule_apply( const struct rule *rule, WCHAR **cmdline, struct env_block *env )
{
    const WCHAR *forbidden = NULL, *arg;
    unsigned int i;

    for (i = 0; i < rule->count; i++)
        if ((arg = directive( rule->lines[i], L"forbidden" ))) forbidden = arg;

    for (i = 0; i < rule->count; i++)
    {
        const WCHAR *line = rule->lines[i];

        if ((arg = directive( line, L"append" )))
        {
            size_t len = lstrlenW( *cmdline ), add = lstrlenW( arg );
            WCHAR *new_cmdline;

            if (forbidden && wcsstr( *cmdline, forbidden )) continue;
            if (wcsstr( *cmdline, arg )) continue;  /* already there, e.g. passed on by a parent */
            if (!(new_cmdline = HeapAlloc( GetProcessHeap(), 0, (len + add + 2) * sizeof(WCHAR) ))) continue;
            memcpy( new_cmdline, *cmdline, len * sizeof(WCHAR) );
            new_cmdline[len] = ' ';
            memcpy( new_cmdline + len + 1, arg, (add + 1) * sizeof(WCHAR) );
            HeapFree( GetProcessHeap(), 0, *cmdline );
            *cmdline = new_cmdline;
        }
        else if ((arg = directive( line, L"env" )))
        {
            const WCHAR *eq = wcschr( arg, '=' );
            if (eq && eq != arg) env_set_both( env, arg, eq - arg, eq + 1 );
        }
        else if ((arg = directive( line, L"dll" )))
        {
            const WCHAR *current = env_get( env, L"WINEDLLOVERRIDES" );
            size_t len = current ? lstrlenW( current ) : 0, add = lstrlenW( arg );
            WCHAR *value = HeapAlloc( GetProcessHeap(), 0, (len + add + 2) * sizeof(WCHAR) );

            if (!value) continue;
            /* Later entries win in Wine's parser, so the rule's override takes effect. */
            if (len)
            {
                memcpy( value, current, len * sizeof(WCHAR) );
                value[len++] = ';';
            }
            memcpy( value + len, arg, (add + 1) * sizeof(WCHAR) );
            env_set_both( env, L"WINEDLLOVERRIDES", 16, value );
            HeapFree( GetProcessHeap(), 0, value );
        }
        else if ((arg = directive( line, L"backend" )))
        {
            WCHAR name[64] = L"OVERPASS_BACKEND_";
            const WCHAR *dir;
            unsigned int n = lstrlenW( name );

            for (; *arg && n < ARRAY_SIZE(name) - 1; arg++) name[n++] = towupper( *arg );
            name[n] = 0;
            if (!(dir = env_get( env, name )))
            {
                WARN( "rule %s wants a backend without %s\n", debugstr_w(rule->desc), debugstr_w(name) );
                continue;
            }
            env_set_both( env, L"OVERPASS_DLL_PATH_PREPEND", 25, dir );
        }
    }
}

static void finish_rule( struct rule *rule, const WCHAR *app_name, WCHAR **cmdline, struct env_block *env,
                         BOOL *matched )
{
    if (rule->desc && rule_matches( rule, app_name, env ))
    {
        TRACE( "rule %s matches %s\n", debugstr_w(rule->desc), debugstr_w(app_name) );
        rule_apply( rule, cmdline, env );
        *matched = TRUE;
    }
    memset( rule, 0, sizeof(*rule) );
}

/* Applies every matching rule. When one matched, returns TRUE with *new_cmdline and *new_env (a Unicode
 * block) set; the caller frees both with HeapFree. */
BOOL overpass_compat_apply( const WCHAR *app_name, const WCHAR *cmdline, const void *env_block, DWORD flags,
                            WCHAR **new_cmdline, WCHAR **new_env )
{
    struct env_block env = { 0 };
    struct rule rule = { 0 };
    WCHAR *text, *line, *next, *cmd;
    BOOL matched = FALSE;

    *new_cmdline = *new_env = NULL;
    if (!app_name || !cmdline || !(text = load_rules())) return FALSE;
    if (!env_load( &env, env_block, flags ) || !(cmd = heap_strdupW( cmdline, lstrlenW( cmdline ) )))
    {
        env_free( &env );
        HeapFree( GetProcessHeap(), 0, text );
        return FALSE;
    }

    for (line = text; line; line = next)
    {
        WCHAR *end;
        const WCHAR *desc;

        if ((next = wcschr( line, '\n' ))) *next++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        end = line + lstrlenW( line );
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*line || *line == '#') continue;

        if (!wcscmp( line, L"rule" ) || (desc = directive( line, L"rule" )))
        {
            finish_rule( &rule, app_name, &cmd, &env, &matched );
            rule.desc = wcscmp( line, L"rule" ) ? desc : L"";
        }
        else if (rule.desc && rule.count < MAX_RULE_LINES) rule.lines[rule.count++] = line;
    }
    finish_rule( &rule, app_name, &cmd, &env, &matched );

    if (matched)
    {
        *new_cmdline = cmd;
        *new_env = env_build( &env );
    }
    else HeapFree( GetProcessHeap(), 0, cmd );
    env_free( &env );
    HeapFree( GetProcessHeap(), 0, text );
    return matched;
}
