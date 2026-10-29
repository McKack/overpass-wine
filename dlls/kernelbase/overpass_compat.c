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
 * Rules are INI-style text files, one or more per app, that people can edit without building
 * anything. OVERPASS_COMPAT_RULES (in the creating process's environment) lists Windows paths to
 * folders of *.ini files, or to single files, separated by ';'. Each [section] is one rule:
 *
 *   # GOG Galaxy draws its interface with Chromium, whose GPU path fails under Wine.
 *   [GOG Galaxy: software rendering]
 *   program = GalaxyClient.exe                      a program the rule covers: file name or path ending,
 *   program = GOG Galaxy\GalaxyClient Helper.exe     any case; repeat for more
 *   steam-app = 1234                                or a Steam game, by the SteamGameId Steam sets
 *   beside = libcef.dll                             or any program with this file in its folder
 *   add-arguments = --in-process-gpu --use-gl=swiftshader
 *   unless-argument = --type=crashpad-handler       don't add arguments to command lines containing this
 *   env = NAME=value                                for the program's Windows and Unix environment
 *   dll = xaudio2_7=n,b                             added to WINEDLLOVERRIDES
 *   graphics = dxmt                                 dxmt, d3dmetal, wined3d or auto; OverPass sets
 *                                                   OVERPASS_BACKEND_<NAME> to each backend's DLL folder
 *   d3d9 = mtld3d                                   mtld3d or wined3d for Direct3D 9, after any graphics
 *                                                   line; OverPass sets OVERPASS_D3D9_MTLD3D to mtld3d's folder
 *   replace-path = \Game\start.exe => \Game\game.exe
 *                                                   start another program instead: the first part of the
 *                                                   program's path is replaced by the second, or the whole
 *                                                   path when the second is absolute (C:\...); skipped
 *                                                   when the replacement doesn't exist
 *   source = https://...                            where the fix comes from (not used here)
 *
 * Lines starting with '#' or ';' are comments. A rule applies when any of its program, steam-app or
 * beside lines matches; matching rules are applied in the order they are read.
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

/* Appends a UTF-8 file to *text (a heap UTF-16 buffer), followed by a newline. */
static void append_file( WCHAR **text, size_t *len, const WCHAR *path )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    DWORD size, read, lenW;
    char *utf8 = NULL;
    WCHAR *grown;

    if (file == INVALID_HANDLE_VALUE)
    {
        WARN( "can't open %s\n", debugstr_w(path) );
        return;
    }
    size = GetFileSize( file, NULL );
    if (size != INVALID_FILE_SIZE && size < (4 << 20) && (utf8 = HeapAlloc( GetProcessHeap(), 0, size + 1 ))
        && ReadFile( file, utf8, size, &read, NULL ))
    {
        lenW = MultiByteToWideChar( CP_UTF8, 0, utf8, read, NULL, 0 );
        grown = *text ? HeapReAlloc( GetProcessHeap(), 0, *text, (*len + lenW + 2) * sizeof(WCHAR) )
                      : HeapAlloc( GetProcessHeap(), 0, (lenW + 2) * sizeof(WCHAR) );
        if (grown)
        {
            *text = grown;
            MultiByteToWideChar( CP_UTF8, 0, utf8, read, *text + *len, lenW );
            *len += lenW;
            (*text)[(*len)++] = '\n';
            (*text)[*len] = 0;
        }
    }
    HeapFree( GetProcessHeap(), 0, utf8 );
    CloseHandle( file );
}

/* Every rules file named by OVERPASS_COMPAT_RULES as one UTF-16 text, or NULL when there are none. */
static WCHAR *load_rules(void)
{
    WCHAR list[4096], *entry, *next, path[MAX_PATH];
    WCHAR *text = NULL;
    size_t len = 0;

    if (!GetEnvironmentVariableW( L"OVERPASS_COMPAT_RULES", list, ARRAY_SIZE(list) )) return NULL;
    for (entry = list; entry; entry = next)
    {
        DWORD attr;

        if ((next = wcschr( entry, ';' ))) *next++ = 0;
        if (!*entry) continue;
        attr = GetFileAttributesW( entry );
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            WIN32_FIND_DATAW data;
            HANDLE find;

            if (swprintf( path, ARRAY_SIZE(path), L"%s\\*.ini", entry ) < 0) continue;
            if ((find = FindFirstFileW( path, &data )) == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (swprintf( path, ARRAY_SIZE(path), L"%s\\%s", entry, data.cFileName ) >= 0)
                    append_file( &text, &len, path );
            } while (FindNextFileW( find, &data ));
            FindClose( find );
        }
        else append_file( &text, &len, entry );
    }
    return text;
}

struct rule
{
    const WCHAR *name;
    const WCHAR *lines[MAX_RULE_LINES];  /* "key = value" lines */
    unsigned int count;
};

/* The value of a "key = value" line, or NULL when the line has another key. */
static const WCHAR *value_of( const WCHAR *line, const WCHAR *key )
{
    size_t len = lstrlenW( key );
    if (wcsnicmp( line, key, len )) return NULL;
    line += len;
    while (*line == ' ' || *line == '\t') line++;
    if (*line++ != '=') return NULL;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

/* Case-insensitive wcsstr. */
static const WCHAR *find_nocase( const WCHAR *str, const WCHAR *sub, size_t sub_len )
{
    for (; *str; str++) if (!wcsnicmp( str, sub, sub_len )) return str;
    return NULL;
}

/* Applies a "from => to" replace-path value to *app_name and the program at the start of *cmdline. */
static void replace_path( const struct rule *rule, const WCHAR *value, WCHAR **app_name, WCHAR **cmdline )
{
    const WCHAR *sep = wcsstr( value, L"=>" ), *to, *found, *args;
    size_t from_len, to_len, app_len = lstrlenW( *app_name ), len;
    WCHAR *new_app, *new_cmdline;

    if (!sep) return;
    for (from_len = sep - value; from_len && (value[from_len - 1] == ' ' || value[from_len - 1] == '\t'); from_len--) ;
    for (to = sep + 2; *to == ' ' || *to == '\t'; to++) ;
    to_len = lstrlenW( to );
    if (!from_len || !to_len || !(found = find_nocase( *app_name, value, from_len ))) return;

    if ((to[0] && to[1] == ':') || (to[0] == '\\' && to[1] == '\\')) new_app = heap_strdupW( to, to_len );
    else if ((new_app = HeapAlloc( GetProcessHeap(), 0, (app_len - from_len + to_len + 1) * sizeof(WCHAR) )))
    {
        len = found - *app_name;
        memcpy( new_app, *app_name, len * sizeof(WCHAR) );
        memcpy( new_app + len, to, to_len * sizeof(WCHAR) );
        lstrcpyW( new_app + len + to_len, found + from_len );
    }
    if (!new_app) return;
    if (GetFileAttributesW( new_app ) == INVALID_FILE_ATTRIBUTES)
    {
        WARN( "rule %s: %s doesn't exist\n", debugstr_w(rule->name), debugstr_w(new_app) );
        HeapFree( GetProcessHeap(), 0, new_app );
        return;
    }

    /* The command line's first word names the program; the arguments after it stay as they are. */
    if (**cmdline == '"')
    {
        args = wcschr( *cmdline + 1, '"' );
        args = args ? args + 1 : *cmdline + lstrlenW( *cmdline );
    }
    else for (args = *cmdline; *args && *args != ' ' && *args != '\t'; args++) ;
    len = lstrlenW( new_app ) + lstrlenW( args ) + 3;
    if (!(new_cmdline = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) )))
    {
        HeapFree( GetProcessHeap(), 0, new_app );
        return;
    }
    swprintf( new_cmdline, len, L"\"%s\"%s", new_app, args );

    TRACE( "rule %s: starting %s instead of %s\n", debugstr_w(rule->name), debugstr_w(new_app), debugstr_w(*app_name) );
    HeapFree( GetProcessHeap(), 0, *app_name );
    HeapFree( GetProcessHeap(), 0, *cmdline );
    *app_name = new_app;
    *cmdline = new_cmdline;
}

static BOOL rule_matches( const struct rule *rule, const WCHAR *app_name, const struct env_block *env )
{
    size_t app_len = lstrlenW( app_name );
    unsigned int i;
    const WCHAR *value;

    for (i = 0; i < rule->count; i++)
    {
        if ((value = value_of( rule->lines[i], L"program" )))
        {
            /* Compare with a backslash in front, so "Client.exe" doesn't match "GalaxyClient.exe". */
            WCHAR suffix[MAX_PATH];
            size_t len;
            WCHAR *p;

            while (*value == '\\' || *value == '/') value++;
            if (swprintf( suffix, ARRAY_SIZE(suffix), L"\\%s", value ) < 0) continue;
            for (p = suffix; *p; p++) if (*p == '/') *p = '\\';
            len = lstrlenW( suffix );
            if (len > 1 && len <= app_len && !wcsicmp( app_name + app_len - len, suffix )) return TRUE;
        }
        else if ((value = value_of( rule->lines[i], L"steam-app" )))
        {
            const WCHAR *id = env_get( env, L"SteamGameId" );
            if (id && !wcscmp( id, value )) return TRUE;
        }
        else if ((value = value_of( rule->lines[i], L"beside" )))
        {
            /* A file of that name in the program's folder, e.g. libcef.dll for a Chromium Embedded program. */
            WCHAR path[MAX_PATH];
            const WCHAR *slash = wcsrchr( app_name, '\\' );
            DWORD attrs;

            if (!slash || !*value || wcschr( value, '\\' ) || wcschr( value, '/' )) continue;
            if (swprintf( path, ARRAY_SIZE(path), L"%.*s\\%s", (int)(slash - app_name), app_name, value ) < 0) continue;
            attrs = GetFileAttributesW( path );
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return TRUE;
        }
    }
    return FALSE;
}

static void rule_apply( const struct rule *rule, WCHAR **app_name, WCHAR **cmdline, struct env_block *env )
{
    const WCHAR *unless = NULL, *value;
    unsigned int i;

    for (i = 0; i < rule->count; i++)
        if ((value = value_of( rule->lines[i], L"unless-argument" ))) unless = value;

    for (i = 0; i < rule->count; i++)
    {
        const WCHAR *line = rule->lines[i];

        if ((value = value_of( line, L"replace-path" ))) replace_path( rule, value, app_name, cmdline );
        else if ((value = value_of( line, L"add-arguments" )))
        {
            size_t len = lstrlenW( *cmdline ), add = lstrlenW( value );
            WCHAR *new_cmdline;

            if (unless && wcsstr( *cmdline, unless )) continue;
            if (wcsstr( *cmdline, value )) continue;  /* already there, e.g. passed on by a parent */
            if (!(new_cmdline = HeapAlloc( GetProcessHeap(), 0, (len + add + 2) * sizeof(WCHAR) ))) continue;
            memcpy( new_cmdline, *cmdline, len * sizeof(WCHAR) );
            new_cmdline[len] = ' ';
            memcpy( new_cmdline + len + 1, value, (add + 1) * sizeof(WCHAR) );
            HeapFree( GetProcessHeap(), 0, *cmdline );
            *cmdline = new_cmdline;
        }
        else if ((value = value_of( line, L"env" )))
        {
            const WCHAR *eq = wcschr( value, '=' );
            if (eq && eq != value) env_set_both( env, value, eq - value, eq + 1 );
        }
        else if ((value = value_of( line, L"dll" )))
        {
            const WCHAR *current = env_get( env, L"WINEDLLOVERRIDES" );
            size_t len = current ? lstrlenW( current ) : 0, add = lstrlenW( value );
            WCHAR *overrides = HeapAlloc( GetProcessHeap(), 0, (len + add + 2) * sizeof(WCHAR) );

            if (!overrides) continue;
            /* Later entries win in Wine's parser, so the rule's override takes effect. */
            if (len)
            {
                memcpy( overrides, current, len * sizeof(WCHAR) );
                overrides[len++] = ';';
            }
            memcpy( overrides + len, value, (add + 1) * sizeof(WCHAR) );
            env_set_both( env, L"WINEDLLOVERRIDES", 16, overrides );
            HeapFree( GetProcessHeap(), 0, overrides );
        }
        else if ((value = value_of( line, L"graphics" )))
        {
            WCHAR name[64] = L"OVERPASS_BACKEND_";
            const WCHAR *dir, *v;
            unsigned int n = lstrlenW( name );
            BOOL builtin = !wcsicmp( value, L"wined3d" ) || !wcsicmp( value, L"auto" );

            for (v = value; *v && n < ARRAY_SIZE(name) - 1; v++) name[n++] = towupper( *v );
            name[n] = 0;
            /* wined3d is Wine's own, so it needs no folder in front of Wine's DLLs. */
            if (!(dir = env_get( env, name )) && !builtin)
            {
                WARN( "rule %s wants %s, which isn't installed\n", debugstr_w(rule->name), debugstr_w(value) );
                continue;
            }
            env_set_both( env, L"OVERPASS_DLL_PATH_PREPEND", 25, dir ? dir : L"" );
            env_set_both( env, L"CX_ACTIVE_GRAPHICS_BACKEND", 26, wcsicmp( value, L"auto" ) ? value : L"" );
        }
        else if ((value = value_of( line, L"d3d9" )))
        {
            /* mtld3d's folder goes first in the DLL path, or comes out of it for WineD3D. */
            const WCHAR *dir = env_get( env, L"OVERPASS_D3D9_MTLD3D" );
            const WCHAR *path = env_get( env, L"OVERPASS_DLL_PATH_PREPEND" );
            size_t dir_len, path_len = path ? lstrlenW( path ) : 0;
            BOOL first;
            WCHAR *new_path;

            if (!dir || !*dir)
            {
                WARN( "rule %s wants %s for Direct3D 9, which isn't installed\n", debugstr_w(rule->name), debugstr_w(value) );
                continue;
            }
            dir_len = lstrlenW( dir );
            first = path_len >= dir_len && !wcsncmp( path, dir, dir_len ) && (!path[dir_len] || path[dir_len] == ':');
            if (!wcsicmp( value, L"wined3d" ))
            {
                if (first) env_set_both( env, L"OVERPASS_DLL_PATH_PREPEND", 25, path[dir_len] ? path + dir_len + 1 : L"" );
            }
            else if (!wcsicmp( value, L"mtld3d" ) && !first)
            {
                if (!(new_path = HeapAlloc( GetProcessHeap(), 0, (dir_len + path_len + 2) * sizeof(WCHAR) ))) continue;
                memcpy( new_path, dir, dir_len * sizeof(WCHAR) );
                new_path[dir_len] = path_len ? ':' : 0;
                if (path_len) memcpy( new_path + dir_len + 1, path, (path_len + 1) * sizeof(WCHAR) );
                env_set_both( env, L"OVERPASS_DLL_PATH_PREPEND", 25, new_path );
                HeapFree( GetProcessHeap(), 0, new_path );
            }
        }
    }
}

static void finish_rule( struct rule *rule, WCHAR **app_name, WCHAR **cmdline, struct env_block *env,
                         BOOL *matched )
{
    if (rule->name && rule_matches( rule, *app_name, env ))
    {
        TRACE( "rule %s matches %s\n", debugstr_w(rule->name), debugstr_w(*app_name) );
        rule_apply( rule, app_name, cmdline, env );
        *matched = TRUE;
    }
    memset( rule, 0, sizeof(*rule) );
}

/* Applies every matching rule. When one matched, returns TRUE with *new_app_name, *new_cmdline and
 * *new_env (a Unicode block) set; the caller frees all three with HeapFree. Rules after a replace-path
 * see the replacement program. */
BOOL overpass_compat_apply( const WCHAR *app_name, const WCHAR *cmdline, const void *env_block, DWORD flags,
                            WCHAR **new_app_name, WCHAR **new_cmdline, WCHAR **new_env )
{
    struct env_block env = { 0 };
    struct rule rule = { 0 };
    WCHAR *text, *line, *next, *cmd = NULL, *app = NULL;
    BOOL matched = FALSE;

    *new_app_name = *new_cmdline = *new_env = NULL;
    if (!app_name || !cmdline || !(text = load_rules())) return FALSE;
    if (!env_load( &env, env_block, flags ) || !(cmd = heap_strdupW( cmdline, lstrlenW( cmdline ) ))
        || !(app = heap_strdupW( app_name, lstrlenW( app_name ) )))
    {
        HeapFree( GetProcessHeap(), 0, cmd );
        env_free( &env );
        HeapFree( GetProcessHeap(), 0, text );
        return FALSE;
    }

    for (line = text; line; line = next)
    {
        WCHAR *end;

        if ((next = wcschr( line, '\n' ))) *next++ = 0;
        while (*line == ' ' || *line == '\t' || *line == 0xfeff) line++;
        end = line + lstrlenW( line );
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*line || *line == '#' || *line == ';') continue;

        if (*line == '[' && end[-1] == ']')
        {
            finish_rule( &rule, &app, &cmd, &env, &matched );
            end[-1] = 0;
            rule.name = line + 1;
        }
        else if (rule.name && rule.count < MAX_RULE_LINES) rule.lines[rule.count++] = line;
    }
    finish_rule( &rule, &app, &cmd, &env, &matched );

    if (matched)
    {
        *new_app_name = app;
        *new_cmdline = cmd;
        *new_env = env_build( &env );
    }
    else
    {
        HeapFree( GetProcessHeap(), 0, app );
        HeapFree( GetProcessHeap(), 0, cmd );
    }
    env_free( &env );
    HeapFree( GetProcessHeap(), 0, text );
    return matched;
}
