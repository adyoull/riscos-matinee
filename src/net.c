/*
 * net.c - HTTP(S) requests through FFmpeg's avio (see net.h).
 * Part of riscos-plex. GPL v2 or later.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>

#define FETCH_MAX (16 << 20)   /* 16 MB: far more than any Plex listing */

static char agent[64] = "PlexRO";

struct net_stream {
    AVIOContext *io;
};

void net_init(const char *user_agent)
{
    if (user_agent)
        snprintf(agent, sizeof(agent), "%s", user_agent);
    /* FFmpeg's messages go to stderr, which a Wimp task has nowhere to
       show; errors are reported through err instead. PlexRO$Debug (any
       value) keeps them, for running from a TaskWindow. */
    if (!getenv("PlexRO$Debug"))
        av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();
}

/* The HTTP status an avio error stands for, 0 if none */
static int http_status(int e)
{
    switch (e) {
    case AVERROR_HTTP_BAD_REQUEST:  return 400;
    case AVERROR_HTTP_UNAUTHORIZED: return 401;
    case AVERROR_HTTP_FORBIDDEN:    return 403;
    case AVERROR_HTTP_NOT_FOUND:    return 404;
    case AVERROR_HTTP_OTHER_4XX:    return 499;
    case AVERROR_HTTP_SERVER_ERROR: return 500;
    default:                        return 0;
    }
}

static void describe(int e, const char *url, char *err, size_t errlen)
{
    char what[128];
    const char *host = strstr(url, "://");
    char h[80];
    size_t n;
    if (!err || !errlen)
        return;
    host = host ? host + 3 : url;
    n = strcspn(host, "/?");
    if (n >= sizeof(h))
        n = sizeof(h) - 1;
    memcpy(h, host, n);
    h[n] = 0;
    switch (http_status(e)) {
    case 401: snprintf(what, sizeof(what), "the server wants you to sign in again (401)"); break;
    case 403: snprintf(what, sizeof(what), "not allowed (403)"); break;
    case 404: snprintf(what, sizeof(what), "not found (404)"); break;
    case 400: case 499: snprintf(what, sizeof(what), "the server refused the request"); break;
    case 500: snprintf(what, sizeof(what), "the server had an error"); break;
    default:
        if (e == AVERROR(ETIMEDOUT) || e == AVERROR(EAGAIN))
            snprintf(what, sizeof(what), "no answer in time");
        else if (e == AVERROR_PROTOCOL_NOT_FOUND)
            snprintf(what, sizeof(what), "https needs the AcornSSL module");
        else
            av_strerror(e, what, sizeof(what));
    }
    snprintf(err, errlen, "%s: %s", h, what);
}

/* The avio options every request gets */
static AVDictionary *options(const char *headers, int timeout_ms)
{
    AVDictionary *o = NULL;
    char t[32];
    av_dict_set(&o, "protocol_whitelist", "http,https,tcp,tls", 0);
    av_dict_set(&o, "user_agent", agent, 0);
    snprintf(t, sizeof(t), "%lld", (long long)(timeout_ms > 0 ? timeout_ms : 15000) * 1000);
    av_dict_set(&o, "rw_timeout", t, 0);
    av_dict_set(&o, "timeout", t, 0);
    if (headers && *headers)
        av_dict_set(&o, "headers", headers, 0);
    return o;
}

int net_fetch(const char *url, const char *headers, const char *post, net_buf *out,
              int timeout_ms, char *err, size_t errlen)
{
    AVIOContext *io = NULL;
    AVDictionary *o = options(headers, timeout_ms);
    size_t cap = 0;
    int e;

    memset(out, 0, sizeof(*out));
    if (err && errlen)
        *err = 0;
    if (post) {
        /* avio's binary option is set as hex; a POST always carries a body
           (an empty one would leave no Content-Length) */
        const char *body = *post ? post : "x=1";
        size_t n = strlen(body);
        char *hex = malloc(n * 2 + 1);
        char *h;
        if (!hex) {
            av_dict_free(&o);
            snprintf(err, errlen, "out of memory");
            return AVERROR(ENOMEM);
        }
        for (size_t i = 0; i < n; i++)
            sprintf(hex + 2 * i, "%02x", (unsigned char)body[i]);
        av_dict_set(&o, "method", "POST", 0);
        av_dict_set(&o, "post_data", hex, 0);
        free(hex);
        /* the form's type, beside the caller's headers */
        h = malloc((headers ? strlen(headers) : 0) + 64);
        if (h) {
            sprintf(h, "%sContent-Type: application/x-www-form-urlencoded\r\n", headers ? headers : "");
            av_dict_set(&o, "headers", h, 0);
            free(h);
        }
    }
    e = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    if (e < 0) {
        out->status = http_status(e);
        describe(e, url, err, errlen);
        return e;
    }
    for (;;) {
        int got;
        if (out->len + 16384 + 1 > cap) {
            char *nd;
            if (cap >= FETCH_MAX) {
                e = AVERROR(E2BIG);
                break;
            }
            nd = realloc(out->data, cap = cap ? cap * 2 : 65536);
            if (!nd) {
                e = AVERROR(ENOMEM);
                break;
            }
            out->data = nd;
        }
        got = avio_read(io, (unsigned char *)out->data + out->len, (int)(cap - out->len - 1));
        if (got == AVERROR_EOF || got == 0)
            break;
        if (got < 0) {
            e = got;
            break;
        }
        out->len += got;
    }
    avio_closep(&io);
    if (e < 0) {
        describe(e, url, err, errlen);
        net_buf_free(out);
        return e;
    }
    if (!out->data && !(out->data = malloc(1)))
        return AVERROR(ENOMEM);
    out->data[out->len] = 0;
    out->status = 200;
    return 0;
}

void net_buf_free(net_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
}

net_stream *net_open(const char *url, const char *headers, int timeout_ms, char *err, size_t errlen)
{
    net_stream *s = calloc(1, sizeof(*s));
    AVDictionary *o = options(headers, timeout_ms);
    int e;
    if (!s) {
        av_dict_free(&o);
        return NULL;
    }
    e = avio_open2(&s->io, url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    if (e < 0) {
        describe(e, url, err, errlen);
        free(s);
        return NULL;
    }
    return s;
}

int64_t net_size(net_stream *s)
{
    int64_t n = avio_size(s->io);
    return n < 0 ? -1 : n;
}

int net_read(net_stream *s, void *buf, int size)
{
    int got = avio_read(s->io, buf, size);
    if (got == AVERROR_EOF)
        return 0;
    return got;
}

void net_close(net_stream *s)
{
    if (!s)
        return;
    avio_closep(&s->io);
    free(s);
}

void net_escape(const char *s, char *out, size_t size)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!size)
        return;
    for (; *s && o + 4 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hexd[c >> 4];
            out[o++] = hexd[c & 15];
        }
    }
    out[o] = 0;
}
