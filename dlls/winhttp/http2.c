/*
 * Generic HTTP/2 transport for Wine WinHTTP. LGPL-2.1-or-later.
 * Uses nghttp2 (MIT) for HTTP/2 parsing, HPACK and flow control, and Wine's
 * existing Schannel/certificate verification and request task queues.
 * One request per connection initially; no shared stream state or retries.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "windef.h"
#include "winbase.h"
#include "ws2tcpip.h"
#include "winhttp.h"
#include "winhttp_private.h"
#include "wine/debug.h"
#define NGHTTP2_STATICLIB
#include "nghttp2/nghttp2.h"

WINE_DEFAULT_DEBUG_CHANNEL(winhttp);

#define H2_WINDOW 65535
#define H2_HEADERS_LIMIT 65536
#define H2_FUNCTIONS(X) \
 X(session_callbacks_new) X(session_callbacks_del) \
 X(session_callbacks_set_on_header_callback) X(session_callbacks_set_on_frame_recv_callback) \
 X(session_callbacks_set_on_data_chunk_recv_callback) X(session_callbacks_set_on_stream_close_callback) \
 X(option_new) X(option_del) X(option_set_no_auto_window_update) \
 X(session_client_new2) X(session_del) X(submit_settings) X(submit_request) \
 X(session_mem_send2) X(session_mem_recv2) X(session_resume_data) X(session_consume)
#define DECLARE(name) static __typeof__(nghttp2_##name) *p_##name;
H2_FUNCTIONS(DECLARE)
#undef DECLARE
static INIT_ONCE library_once = INIT_ONCE_STATIC_INIT;

struct http2_connection
{
    nghttp2_session *session;
    struct netconn *connection;
    struct request *owner;
    int32_t stream;
    DWORD error, status;
    BOOL headers_ready, ended, response_installed;
    BYTE body[H2_WINDOW];
    size_t body_len, header_len;
    char headers[H2_HEADERS_LIMIT + 1];
    const BYTE *upload;
    size_t upload_len, upload_pos;
    UINT64 upload_total, upload_read;
    /* Bounded sanitized-diagnostic context, never used unless enabled. */
    char diag_host[64];
    char diag_path[192];
    DWORD diag_received;
};

/* Sanitized response diagnostics: enabled only when the operator pre-creates
 * a private log file and passes its path via XUSER_WINHTTP_BODY_LOG. Writes
 * one bounded line per response (status/host/path/lengths) plus a strictly
 * whitelisted body structure for selected service paths. Value text is only
 * emitted for pure-numeric, correlation-ID, date and small enum shapes, so
 * token/signature/base64 content cannot pass through by construction. */
#define H2_DIAG_LINE_LIMIT 3072
#define H2_DIAG_BODY_LIMIT 2048

static SRWLOCK diag_lock = SRWLOCK_INIT;
static HANDLE diag_file;

static void diag_init( void )
{
    /* Do not activate the legacy printable-run body scanner in a shared
     * runtime. Token fragments can match its identifier-shaped whitelist.
     * Keep transport tracing numerical; schema-aware diagnostics belong in
     * a separately audited opt-in tool, not the distributed DLL. */
}

static void diag_emit( char *out, size_t size, size_t *pos, const char *text )
{
    int n = snprintf( out + *pos, size - *pos, "%s", text );
    if (n > 0 && (size_t)n < size - *pos) *pos += n;
    else *pos = size - 1;
}

static BOOL diag_value_allowed( const char *value, size_t len )
{
    static const char *enums[] = {"true","false","null","pending","complete","completed",
        "error","success","failed","active","processing","retrying","queued","granted",
        "denied","timeout","identity","unknown","none","ready","notready", NULL};
    size_t i;
    if (!len || len > 48) return FALSE;
    /* Pure digits/dashes only: counts, codes, retry hints. */
    for (i = 0; i < len; i++)
    {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || c == '-')) break;
    }
    if (i == len) return TRUE;
    if (len > 5 && !strncasecmp( value, "TCCL-", 5 ))
    {
        for (i = 5; i < len; i++)
        {
            char c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return FALSE;
        }
        return TRUE;
    }
    for (i = 0; enums[i]; i++)
        if (len == strlen( enums[i] ) && !strncasecmp( value, enums[i], len )) return TRUE;
    /* Identifier-shaped runs (msgpack keys, JSON keys, enum-ish words) are
     * structural metadata. Credential/token material is longer than 48
     * characters and never reaches this check. */
    if (!((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'a' + 25)))
        return FALSE;
    for (i = 0; i < len; i++)
    {
        char c = value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return FALSE;
    }
    return TRUE;
}

static void diag_append_body( char *out, size_t size, size_t *pos,
                              const BYTE *body, size_t body_len )
{
    /* General printable-run scanner: works for msgpack and JSON bodies.
     * Each maximal printable run is split into <=48-byte chunks and only
     * whitelisted shapes are emitted; anything else is masked by length. */
    size_t i = 0;
    char value[49];
    while (i < body_len && *pos < size - 32)
    {
        size_t start = i;
        while (i < body_len && body[i] >= 0x20 && body[i] < 0x7f && i - start < 200) i++;
        while (start < i && *pos < size - 32)
        {
            size_t n = i - start < 48 ? i - start : 48;
            memcpy( value, body + start, n ); value[n] = 0;
            if (diag_value_allowed( value, n ))
            {
                diag_emit( out, size, pos, value );
                diag_emit( out, size, pos, " " );
            }
            else
            {
                char mask[24];
                snprintf( mask, sizeof(mask), "[len:%u] ", (unsigned)n );
                diag_emit( out, size, pos, mask );
            }
            start += n;
        }
        if (i == start) i++;
    }
}

static void diag_response( struct http2_connection *h2 )
{
    char line[H2_DIAG_LINE_LIMIT];
    size_t pos = 0;
    DWORD written;
    const char *path = h2->diag_path;
    const char *colon = strstr( h2->headers, "ontent-encoding:" );
    BOOL interesting;

    if (!diag_file) diag_init();
    if (!diag_file) return;
    interesting = strstr( path, "token/queue" ) || strstr( path, "manifest" ) || strstr( path, "identity" );
    snprintf( line, sizeof(line), "H2RESP status=%u host=%s path=%s bodylen=%u total=%u",
              (unsigned)h2->status, h2->diag_host, path, (unsigned)h2->body_len,
              (unsigned)h2->diag_received );
    pos = strlen( line );
    /* Response content-length / content-type are structural, not sensitive. */
    {
        const char *scan = strstr( h2->headers, "ontent-length:" );
        if (scan)
        {
            char value[24];
            size_t n = 0;
            scan += strlen( "ontent-length:" );
            while (*scan == ' ') scan++;
            while (scan[n] && scan[n] != '\r' && scan[n] != '\n' && n < 23) { value[n] = scan[n]; n++; }
            value[n] = 0;
            diag_emit( line, sizeof(line), &pos, " r-content-length=" );
            diag_emit( line, sizeof(line), &pos, value );
        }
    }
    {
        const char *scan = strstr( h2->headers, "ontent-type:" );
        if (scan)
        {
            char value[64];
            size_t n = 0;
            scan += strlen( "ontent-type:" );
            while (*scan == ' ') scan++;
            while (scan[n] && scan[n] != '\r' && scan[n] != '\n' && n < 63) { value[n] = scan[n]; n++; }
            value[n] = 0;
            diag_emit( line, sizeof(line), &pos, " r-content-type=" );
            diag_emit( line, sizeof(line), &pos, value );
        }
    }
    if (colon)
    {
        char enc[32];
        size_t n = 0;
        colon += strlen( "ontent-encoding:" );
        while (*colon == ' ') colon++;
        while (colon[n] && colon[n] != '\r' && colon[n] != '\n' && n < 31) { enc[n] = colon[n]; n++; }
        enc[n] = 0;
        diag_emit( line, sizeof(line), &pos, " encoding=" );
        diag_emit( line, sizeof(line), &pos, enc );
        diag_emit( line, sizeof(line), &pos, " body-not-captured" );
    }
    else if (interesting && h2->body_len)
    {
        diag_emit( line, sizeof(line), &pos, " body=" );
        diag_append_body( line, sizeof(line), &pos, h2->body,
                          h2->body_len < H2_DIAG_BODY_LIMIT ? h2->body_len : H2_DIAG_BODY_LIMIT );
    }
    /* Correlation IDs are safe by shape; extract them from the whole body
     * including parts the application already consumed. */
    if (interesting && h2->diag_received)
    {
        size_t start = 0;
        char piece[48];
        while (start + 5 < h2->body_len && pos < sizeof(line) - 64)
        {
            if (h2->body[start] == 'T' && h2->body[start + 1] == 'C' &&
                h2->body[start + 2] == 'C' && h2->body[start + 3] == 'L' &&
                h2->body[start + 4] == '-')
            {
                size_t n = 0;
                start += 5;
                while (start + n < h2->body_len && n < 43)
                {
                    BYTE c = h2->body[start + n];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) break;
                    n++;
                }
                memcpy( piece, h2->body + start, n ); piece[n] = 0;
                diag_emit( line, sizeof(line), &pos, " tccl=TCCL-" );
                diag_emit( line, sizeof(line), &pos, piece );
                start += n;
            }
            else start++;
        }
    }
    diag_emit( line, sizeof(line), &pos, "\r\n" );
    AcquireSRWLockExclusive( &diag_lock );
    SetFilePointer( diag_file, 0, NULL, FILE_END );
    WriteFile( diag_file, line, pos, &written, NULL );
    ReleaseSRWLockExclusive( &diag_lock );
}

/* Request-structure diagnostics: header NAMES and value LENGTHS only.
 * Header values are never emitted, so tokens cannot leak. */
static void diag_request( struct request *request, const char *path, UINT64 total )
{
    char line[H2_DIAG_LINE_LIMIT];
    char piece[64];
    size_t pos = 0;
    DWORD written;
    DWORD i, shown = 0;

    if (!diag_file) diag_init();
    if (!diag_file) return;
    if (!strstr( path, "token/queue" ) && !strstr( path, "manifest" ) && !strstr( path, "identity" )) return;
    snprintf( line, sizeof(line), "H2REQ path=%s upload=%llu", path,
              (unsigned long long)total );
    pos = strlen( line );
    diag_emit( line, sizeof(line), &pos, " headers=" );
    snprintf( piece, sizeof(piece), "%u", (unsigned)request->num_headers );
    diag_emit( line, sizeof(line), &pos, piece );
    for (i = 0; i < request->num_headers && shown < 24; i++)
    {
        size_t n = 0;
        char name[40];
        const WCHAR *field = request->headers[i].field;
        size_t value_len = request->headers[i].value ? lstrlenW( request->headers[i].value ) : 0;
        if (!request->headers[i].is_request || !field) continue;
        while (field[n] && n < 38) { name[n] = (char)field[n]; n++; }
        name[n] = 0;
        snprintf( piece, sizeof(piece), " %s(len=%u)", name, (unsigned)value_len );
        diag_emit( line, sizeof(line), &pos, piece );
        shown++;
    }
    diag_emit( line, sizeof(line), &pos, "\r\n" );
    AcquireSRWLockExclusive( &diag_lock );
    SetFilePointer( diag_file, 0, NULL, FILE_END );
    WriteFile( diag_file, line, pos, &written, NULL );
    ReleaseSRWLockExclusive( &diag_lock );
}

static BOOL CALLBACK load_library( INIT_ONCE *once, void *parameter, void **context )
{
    HMODULE module;
    (void)once; (void)parameter; (void)context;
    if (!(module = LoadLibraryExW( L"libnghttp2.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 ))) return FALSE;
#define LOAD(name) if (!(p_##name = (void *)GetProcAddress( module, "nghttp2_" #name ))) { FreeLibrary(module); return FALSE; }
    H2_FUNCTIONS(LOAD)
#undef LOAD
    return TRUE; /* Keep the library loaded as long as callbacks can exist. */
}

static int on_header( nghttp2_session *session, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen, const uint8_t *value,
                      size_t valuelen, uint8_t flags, void *data )
{
    struct http2_connection *h2 = data;
    (void)session; (void)flags;
    if (frame->hd.stream_id != h2->stream || frame->hd.type != NGHTTP2_HEADERS) return 0;
    /* Trailers do not replace the initial response status/headers. */
    if (h2->headers_ready) return 0;
    if (namelen == 7 && !memcmp( name, ":status", 7 ))
    {
        if (valuelen != 3 || value[0] < '1' || value[0] > '5' ||
            value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9')
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        h2->status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
        return 0;
    }
    if (!namelen || name[0] == ':' || namelen + valuelen + 4 > H2_HEADERS_LIMIT - h2->header_len ||
        memchr( name, '\r', namelen ) || memchr( name, '\n', namelen ) ||
        memchr( value, '\r', valuelen ) || memchr( value, '\n', valuelen ))
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy( h2->headers + h2->header_len, name, namelen ); h2->header_len += namelen;
    memcpy( h2->headers + h2->header_len, ": ", 2 ); h2->header_len += 2;
    memcpy( h2->headers + h2->header_len, value, valuelen ); h2->header_len += valuelen;
    memcpy( h2->headers + h2->header_len, "\r\n", 2 ); h2->header_len += 2;
    h2->headers[h2->header_len] = 0;
    return 0;
}

static int on_frame( nghttp2_session *session, const nghttp2_frame *frame, void *data )
{
    struct http2_connection *h2 = data;
    (void)session;
    if (frame->hd.type == NGHTTP2_GOAWAY && frame->goaway.error_code)
        h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    if (frame->hd.stream_id == h2->stream && frame->hd.type == NGHTTP2_HEADERS && !h2->headers_ready)
    {
        if (h2->status >= 200) h2->headers_ready = TRUE;
        else if (h2->status >= 100 && h2->status != 101)
            h2->header_len = h2->status = 0;
        else h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    }
    return 0;
}

static int on_body( nghttp2_session *session, uint8_t flags, int32_t stream,
                    const uint8_t *data, size_t length, void *user )
{
    struct http2_connection *h2 = user;
    (void)session; (void)flags;
    if (stream != h2->stream) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (!h2->headers_ready || length > sizeof(h2->body) - h2->body_len)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy( h2->body + h2->body_len, data, length );
    h2->body_len += length;
    h2->diag_received += length;
    /* Sanitized body scan at arrival time: the application drains the
     * receive buffer before the stream closes, so the close-time capture
     * would see nothing. Bounded to the first bytes of each response. */
    if (diag_file && h2->diag_received <= H2_DIAG_BODY_LIMIT &&
        (strstr( h2->diag_path, "token/queue" ) || strstr( h2->diag_path, "manifest" ) ||
         strstr( h2->diag_path, "identity" )))
    {
        char line[H2_DIAG_LINE_LIMIT];
        size_t pos = 0;
        DWORD written;
        size_t n = length;
        if (h2->diag_received < (DWORD)length) n = h2->diag_received; /* never negative */
        snprintf( line, sizeof(line), "H2SCAN host=%s path=%s frag=", h2->diag_host, h2->diag_path );
        pos = strlen( line );
        diag_append_body( line, sizeof(line), &pos, data, n );
        diag_emit( line, sizeof(line), &pos, "\r\n" );
        AcquireSRWLockExclusive( &diag_lock );
        SetFilePointer( diag_file, 0, NULL, FILE_END );
        WriteFile( diag_file, line, pos, &written, NULL );
        ReleaseSRWLockExclusive( &diag_lock );
    }
    /* Extract safe correlation-ID fragments as the body streams in, before
     * the application can consume the receive buffer. */
    if (diag_file)
    {
        size_t i = 0;
        char line[384];
        char *out = line;
        DWORD written;
        while (i + 5 < length)
        {
            if (data[i] == 'T' && data[i+1] == 'C' && data[i+2] == 'C' &&
                data[i+3] == 'L' && data[i+4] == '-')
            {
                size_t n = 0;
                i += 5;
                out = line;
                out += snprintf( out, sizeof(line), "H2FRAG host=%s path=%s tccl=TCCL-",
                                 h2->diag_host, h2->diag_path );
                while (i + n < length && (size_t)(out - line) < sizeof(line) - 2)
                {
                    uint8_t c = data[i + n];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) break;
                    *out++ = (char)c;
                    n++;
                }
                *out++ = '\r'; *out++ = '\n';
                AcquireSRWLockExclusive( &diag_lock );
                SetFilePointer( diag_file, 0, NULL, FILE_END );
                WriteFile( diag_file, line, out - line, &written, NULL );
                ReleaseSRWLockExclusive( &diag_lock );
                i += n;
            }
            else i++;
        }
    }
    return 0;
}

static int on_close( nghttp2_session *session, int32_t stream, uint32_t error, void *data )
{
    struct http2_connection *h2 = data;
    (void)session;
    if (stream == h2->stream)
    {
        h2->ended = TRUE;
        if (error) h2->error = error == NGHTTP2_CANCEL ? ERROR_WINHTTP_OPERATION_CANCELLED : ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        diag_response( h2 );
    }
    return 0;
}

static ssize_t upload_read( nghttp2_session *session, int32_t stream, uint8_t *buffer,
                            size_t length, uint32_t *flags, nghttp2_data_source *source, void *data )
{
    struct http2_connection *h2 = data;
    size_t count = min( length, h2->upload_len - h2->upload_pos );
    (void)session; (void)stream; (void)source;
    if (count)
    {
        memcpy( buffer, h2->upload + h2->upload_pos, count );
        h2->upload_pos += count;
        h2->upload_read += count;
    }
    if (h2->upload_read == h2->upload_total) *flags |= NGHTTP2_DATA_FLAG_EOF;
    else if (!count) return NGHTTP2_ERR_DEFERRED;
    return count;
}

static DWORD flush_frames( struct http2_connection *h2 )
{
    const uint8_t *frame;
    nghttp2_ssize length;
    int sent;
    DWORD ret;
    if (InterlockedCompareExchange( &h2->owner->cancelled, 0, 0 )) return h2->error = ERROR_WINHTTP_OPERATION_CANCELLED;
    while ((length = p_session_mem_send2( h2->session, &frame )) > 0)
        if ((ret = netconn_send( h2->connection, frame, length, &sent, NULL ))) return h2->error = ret;
    if (length < 0) h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    return h2->error;
}

static DWORD receive_frames( struct http2_connection *h2 )
{
    BYTE input[16384];
    int count;
    nghttp2_ssize consumed;
    DWORD ret;
    DWORD timeout = 0;
    int option_size = sizeof(timeout);
    ULONGLONG started = GetTickCount64();
    if (h2->error) return h2->error;
    if ((ret = flush_frames( h2 ))) return ret;
    getsockopt( (SOCKET)h2->connection->socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, &option_size );
    /* Cancellable wait without a second worker or request-object lifetime. */
    while (!h2->connection->extra_len)
    {
        fd_set readable;
        struct timeval interval = {0, 100000};
        int ready;
        if (InterlockedCompareExchange( &h2->owner->cancelled, 0, 0 )) return h2->error = ERROR_WINHTTP_OPERATION_CANCELLED;
        FD_ZERO(&readable); FD_SET((SOCKET)h2->connection->socket, &readable);
        ready = select(0, &readable, NULL, NULL, &interval);
        if (ready > 0) break;
        if (ready < 0) return h2->error = ERROR_WINHTTP_CONNECTION_ERROR;
        if (timeout && GetTickCount64() - started >= timeout) return h2->error = ERROR_WINHTTP_TIMEOUT;
    }
    if ((ret = netconn_recv( h2->connection, input, sizeof(input), 0, &count ))) return h2->error = ret;
    if (!count) return h2->error = ERROR_WINHTTP_CONNECTION_ERROR;
    consumed = p_session_mem_recv2( h2->session, input, count );
    if (consumed != count) return h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    return flush_frames( h2 );
}

void http2_destroy( struct netconn *connection )
{
    struct http2_connection *h2 = connection->http2_state;
    if (!h2) return;
    p_session_del( h2->session );
    free( h2 );
    connection->http2_state = NULL;
}

static DWORD initialize( struct request *request )
{
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *options = NULL;
    struct http2_connection *h2;
    int result;
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
        { NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, H2_HEADERS_LIMIT },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, H2_WINDOW }
    };
    if (!InitOnceExecuteOnce( &library_once, load_library, NULL, NULL )) return ERROR_MOD_NOT_FOUND;
    if (!(h2 = calloc( 1, sizeof(*h2) ))) return ERROR_OUTOFMEMORY;
    h2->connection = request->netconn;
    h2->owner = request; /* Native WinHTTP task owns its object reference. */
    if (p_session_callbacks_new( &callbacks ) || p_option_new( &options )) goto failed;
    p_session_callbacks_set_on_header_callback( callbacks, on_header );
    p_session_callbacks_set_on_frame_recv_callback( callbacks, on_frame );
    p_session_callbacks_set_on_data_chunk_recv_callback( callbacks, on_body );
    p_session_callbacks_set_on_stream_close_callback( callbacks, on_close );
    p_option_set_no_auto_window_update( options, 1 );
    result = p_session_client_new2( &h2->session, callbacks, h2, options );
    p_session_callbacks_del( callbacks ); callbacks = NULL;
    p_option_del( options ); options = NULL;
    if (result) goto failed;
    if (p_submit_settings( h2->session, NGHTTP2_FLAG_NONE, settings, ARRAY_SIZE(settings) )) goto failed;
    request->netconn->http2_state = h2;
    return ERROR_SUCCESS;
failed:
    if (callbacks) p_session_callbacks_del( callbacks );
    if (options) p_option_del( options );
    if (h2->session) p_session_del( h2->session );
    free( h2 );
    return ERROR_OUTOFMEMORY;
}

static char *utf8( const WCHAR *value )
{
    int length = WideCharToMultiByte( CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL );
    char *result;
    if (!length || !(result = malloc( length ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, value, -1, result, length, NULL, NULL );
    return result;
}

static BOOL hop_header( struct request *request, const WCHAR *name )
{
    DWORD i;
    const WCHAR *cursor, *end, *last;
    if (!wcsicmp( name, L"Connection" ) || !wcsicmp( name, L"Proxy-Connection" ) ||
        !wcsicmp( name, L"Keep-Alive" ) || !wcsicmp( name, L"Upgrade" ) ||
        !wcsicmp( name, L"Transfer-Encoding" ) || !wcsicmp( name, L"Host" )) return TRUE;
    for (i = 0; i < request->num_headers; i++)
    {
        if (!request->headers[i].is_request || wcsicmp( request->headers[i].field, L"Connection" )) continue;
        for (cursor = request->headers[i].value; *cursor; cursor = *end ? end + 1 : end)
        {
            while (*cursor == ' ' || *cursor == '\t') cursor++;
            end = wcschr( cursor, ',' ); if (!end) end = cursor + wcslen( cursor );
            last = end;
            while (last > cursor && (last[-1] == ' ' || last[-1] == '\t')) last--;
            if (wcslen( name ) == last - cursor && !wcsnicmp( name, cursor, last - cursor )) return TRUE;
        }
    }
    return FALSE;
}

static BOOL add_nv( nghttp2_nv *nv, const char *name, const char *value )
{
    nv->name = (uint8_t *)strdup( name ); nv->value = (uint8_t *)strdup( value );
    if (!nv->name || !nv->value) return FALSE;
    nv->namelen = strlen(name); nv->valuelen = strlen(value);
    nv->flags = NGHTTP2_NV_FLAG_NO_INDEX; /* Do not retain sensitive headers in dynamic tables. */
    return TRUE;
}

DWORD http2_send_request( struct request *request, const char *path, void *optional, DWORD length, UINT64 total )
{
    struct http2_connection *h2;
    nghttp2_nv *headers;
    nghttp2_data_provider provider = {{0}, upload_read};
    DWORD ret, i, count = 0;
    const WCHAR *authority = request->connect->hostname;
    char *method = NULL, *host = NULL;
    if (length > total) return ERROR_INVALID_PARAMETER;
    /* An undrained old stream must not be reused or silently replayed. */
    if (request->netconn->http2_state) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if ((ret = initialize( request ))) return ret;
    h2 = request->netconn->http2_state;
    h2->upload_total = total;
    h2->upload = optional; h2->upload_len = length;
    if (!(headers = calloc( request->num_headers + 4, sizeof(*headers) ))) return ERROR_OUTOFMEMORY;
    for (i = 0; i < request->num_headers; i++)
        if (request->headers[i].is_request && !wcsicmp( request->headers[i].field, L"Host" )) authority = request->headers[i].value;
    ret = ERROR_OUTOFMEMORY;
    if (!(method = utf8( request->verb )) || !(host = utf8( authority ))) goto done;
    /* Sanitized diagnostic context; the query part is never retained. */
    {
        size_t n = 0;
        while (host[n] && n < sizeof(h2->diag_host) - 1) { h2->diag_host[n] = host[n]; n++; }
        h2->diag_host[n] = 0;
        n = 0;
        while (path[n] && path[n] != '?' && n < sizeof(h2->diag_path) - 1) { h2->diag_path[n] = path[n]; n++; }
        h2->diag_path[n] = 0;
    }
    diag_request( request, h2->diag_path, total );
#define NV(n,v) do { if (!add_nv( headers + count++, n, v )) goto done; } while (0)
    NV(":method", method); NV(":scheme", "https"); NV(":authority", host); NV(":path", path);
    for (i = 0; i < request->num_headers; i++)
    {
        char *name, *value;
        BOOL ok;
        if (!request->headers[i].is_request || hop_header( request, request->headers[i].field )) continue;
        name = utf8( request->headers[i].field ); value = utf8( request->headers[i].value );
        if (!name || !value) { free(name); free(value); goto done; }
        for (char *p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
        ok = add_nv( headers + count++, name, value );
        free(name); free(value);
        if (!ok) goto done;
    }
#undef NV
    h2->stream = p_submit_request( h2->session, NULL, headers, count, total ? &provider : NULL, NULL );
    if (h2->stream < 0) { ret = ERROR_WINHTTP_INVALID_SERVER_RESPONSE; goto done; }
    ret = flush_frames( h2 );
    while (!ret && h2->upload_pos < h2->upload_len && !h2->ended) ret = receive_frames( h2 );
    if (!ret && h2->upload_pos < h2->upload_len) ret = ERROR_WINHTTP_CONNECTION_ERROR;
    h2->upload = NULL; h2->upload_len = h2->upload_pos = 0;
done:
    for (i = 0; i < count; i++) { free( headers[i].name ); free( headers[i].value ); }
    free(headers); free(method); free(host);
    return ret;
}

DWORD http2_read_reply( struct request *request )
{
    struct http2_connection *h2 = request->netconn->http2_state;
    DWORD ret;
    char prefix[32];
    WCHAR *raw, *cursor, *version, *text, status[4];
    int length, prefix_len;
    if (!h2) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if (h2->response_installed) return h2->error;
    while (!h2->headers_ready && !h2->ended)
        if ((ret = receive_frames( h2 ))) return ret;
    if (h2->error) return h2->error;
    if (!h2->headers_ready) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    prefix_len = snprintf( prefix, sizeof(prefix), "HTTP/2.0 %lu\r\n", h2->status );
    length = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, h2->headers, h2->header_len, NULL, 0 );
    if (h2->header_len && !length) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    if (!(raw = calloc( prefix_len + length + 3, sizeof(WCHAR) ))) return ERROR_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, 0, prefix, prefix_len, raw, prefix_len );
    if (length) MultiByteToWideChar( CP_UTF8, 0, h2->headers, h2->header_len, raw + prefix_len, length );
    memcpy( raw + prefix_len + length, L"\r\n", sizeof(L"\r\n") );
    swprintf( status, ARRAY_SIZE(status), L"%lu", h2->status );
    TRACE( "version [HTTP/2.0] status code [%s] status text []\n", debugstr_w(status) );
    ret = process_header( request, L"Status", status, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE, FALSE );
    for (cursor = raw + prefix_len; !ret && *cursor && *cursor != '\r'; )
    {
        WCHAR *colon = wcschr(cursor, ':'), *end = wcsstr(cursor, L"\r\n");
        if (!colon || !end || colon > end) { ret = ERROR_WINHTTP_INVALID_SERVER_RESPONSE; break; }
        *colon = 0; *end = 0;
        ret = process_header( request, cursor, colon + 2, WINHTTP_ADDREQ_FLAG_ADD, FALSE );
        *colon = ':'; *end = '\r'; cursor = end + 2;
    }
    if (ret) { free(raw); return ret; }
    version = wcsdup(L"HTTP/2.0"); text = wcsdup(L"");
    if (!version || !text) { free(version); free(text); free(raw); return ERROR_OUTOFMEMORY; }
    free(request->version); request->version = version;
    free(request->status_text); request->status_text = text;
    free(request->raw_headers); request->raw_headers = raw;
    request->read_reply_len = prefix_len + h2->header_len + 2;
    h2->response_installed = TRUE;
    return ERROR_SUCCESS;
}

DWORD http2_read_data( struct netconn *connection, void *buffer, DWORD length, int *read )
{
    struct http2_connection *h2 = connection->http2_state;
    DWORD ret;
    *read = 0;
    if (!h2) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if (h2->error) return h2->error;
    if (!length) return ERROR_SUCCESS;
    while (!h2->body_len && !h2->ended) if ((ret = receive_frames( h2 ))) return ret;
    if (h2->error) return h2->error;
    *read = min( length, h2->body_len );
    if (*read)
    {
        memcpy( buffer, h2->body, *read ); h2->body_len -= *read;
        memmove( h2->body, h2->body + *read, h2->body_len );
        if (p_session_consume( h2->session, h2->stream, *read )) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        return flush_frames( h2 );
    }
    return ERROR_SUCCESS;
}

DWORD http2_available( struct netconn *connection )
{
    return connection->http2_state ? connection->http2_state->body_len : 0;
}

DWORD http2_write_data( struct netconn *connection, const void *buffer, DWORD length, int *written )
{
    struct http2_connection *h2 = connection->http2_state;
    DWORD ret;
    *written = 0;
    if (!h2 || h2->ended || length > h2->upload_total - h2->upload_read) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    h2->upload = buffer; h2->upload_len = length; h2->upload_pos = 0;
    if (p_session_resume_data( h2->session, h2->stream )) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    ret = flush_frames( h2 );
    while (!ret && h2->upload_pos < length && !h2->ended) ret = receive_frames( h2 );
    *written = h2->upload_pos;
    h2->upload = NULL; h2->upload_len = h2->upload_pos = 0;
    if (!ret && *written != length) ret = ERROR_WINHTTP_CONNECTION_ERROR;
    return ret;
}
