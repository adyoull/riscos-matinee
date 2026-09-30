/*
 * net_sock.c - net.h over plain sockets, for ui_test (which is built for
 * arm-linux and run under qemu, where there's no FFmpeg to link). http://
 * only, one request a connection, Content-Length bodies: what fakeplex.py
 * sends. core_test tests the real net.c (FFmpeg's avio); this stands in
 * for it so ui.c can talk to the same fake server.
 * Part of riscos-plex. GPL v2 or later.
 */
#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static char agent[64] = "PlexRO";

struct net_stream {
    int fd;
    int64_t size, left;
    char pre[16384];            /* body bytes read with the headers */
    int npre, ppre;
};

void net_init(const char *user_agent)
{
    if (user_agent)
        snprintf(agent, sizeof(agent), "%s", user_agent);
}

static void say(char *err, size_t errlen, const char *url, const char *what)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    if (err && errlen)
        snprintf(err, errlen, "%.*s: %s", (int)strcspn(h, "/?"), h, what);
}

/* Connects and sends the request; reads the status line and headers.
   Returns the socket, or -1 (status set when the server answered). */
static int request(const char *url, const char *headers, const char *method, const char *post, int timeout_ms,
                   int *status, int64_t *length, char *pre, int *npre, char *err, size_t errlen)
{
    char host[128], path[4096], *req, head[16384];
    int port = 80, fd, got = 0;
    const char *p, *slash;
    struct sockaddr_in a;
    struct timeval tv;
    size_t rl;

    *status = 0;
    *length = -1;
    *npre = 0;
    if (strncmp(url, "http://", 7)) {
        say(err, errlen, url, "https needs the AcornSSL module");
        return -1;
    }
    p = url + 7;
    slash = p + strcspn(p, "/?");
    snprintf(host, sizeof(host), "%.*s", (int)(slash - p), p);
    snprintf(path, sizeof(path), "%s%s", *slash == '/' ? "" : "/", slash);
    if (strchr(host, ':')) {
        port = atoi(strchr(host, ':') + 1);
        *strchr(host, ':') = 0;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        say(err, errlen, url, "no such host");
        return -1;
    }
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    tv.tv_sec = (timeout_ms > 0 ? timeout_ms : 15000) / 1000;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        say(err, errlen, url, "Connection refused");
        close(fd);
        return -1;
    }
    rl = strlen(path) + (headers ? strlen(headers) : 0) + (post ? strlen(post) : 0) + 512;
    req = malloc(rl);
    if (!req) {
        close(fd);
        return -1;
    }
    if (!post && strcmp(method, "GET"))
        post = "";
    if (post) {
        const char *body = *post ? post : "x=1";
        snprintf(req, rl, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: %s\r\nConnection: close\r\n%s"
                 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n%s",
                 method, path, host, port, agent, headers ? headers : "", (int)strlen(body), body);
    } else {
        snprintf(req, rl, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: %s\r\nConnection: close\r\n%s\r\n",
                 path, host, port, agent, headers ? headers : "");
    }
    if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) {
        free(req);
        close(fd);
        return -1;
    }
    free(req);
    for (;;) {
        char *end;
        ssize_t n = read(fd, head + got, sizeof(head) - 1 - got);
        if (n <= 0) {
            say(err, errlen, url, "no answer in time");
            close(fd);
            return -1;
        }
        got += (int)n;
        head[got] = 0;
        if ((end = strstr(head, "\r\n\r\n")) != NULL) {
            char *cl = strstr(head, "Content-Length:");
            int hl = (int)(end + 4 - head);
            *status = atoi(head + 9);
            if (cl && cl < end)
                *length = atoll(cl + 15);
            *npre = got - hl;
            memcpy(pre, head + hl, *npre);
            break;
        }
    }
    if (*status >= 400) {
        const char *what = *status == 401 ? "the server wants you to sign in again (401)" :
                           *status == 404 ? "not found (404)" : *status == 403 ? "not allowed (403)" :
                           *status >= 500 ? "the server had an error" : "the server refused the request";
        say(err, errlen, url, what);
        close(fd);
        return -1;
    }
    return fd;
}

int net_fetch(const char *url, const char *headers, const char *post, net_buf *out,
              int timeout_ms, char *err, size_t errlen)
{
    return net_send(url, headers, post ? "POST" : "GET", post, out, timeout_ms, err, errlen);
}

int net_send(const char *url, const char *headers, const char *method, const char *post,
             net_buf *out, int timeout_ms, char *err, size_t errlen)
{
    char pre[16384];
    int npre, fd;
    int64_t len;
    size_t cap;
    memset(out, 0, sizeof(*out));
    if (err && errlen)
        *err = 0;
    fd = request(url, headers, method, post, timeout_ms, &out->status, &len, pre, &npre, err, errlen);
    if (fd < 0)
        return -1;
    cap = (len > 0 ? (size_t)len : 65536) + 1;
    if (!(out->data = malloc(cap))) {
        close(fd);
        return -1;
    }
    memcpy(out->data, pre, npre);
    out->len = npre;
    while (len < 0 || (int64_t)out->len < len) {
        ssize_t n;
        if (out->len + 1 >= cap) {
            char *nd = realloc(out->data, cap *= 2);
            if (!nd)
                break;
            out->data = nd;
        }
        n = read(fd, out->data + out->len, cap - out->len - 1);
        if (n <= 0)
            break;
        out->len += n;
    }
    close(fd);
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
    int status;
    if (!s)
        return NULL;
    s->fd = request(url, headers, "GET", NULL, timeout_ms, &status, &s->size, s->pre, &s->npre, err, errlen);
    if (s->fd < 0) {
        free(s);
        return NULL;
    }
    s->left = s->size;
    return s;
}

int64_t net_size(net_stream *s) { return s->size; }

int net_read(net_stream *s, void *buf, int size)
{
    /* like avio_read: as much as asked for, unless the end comes first */
    int got = 0;
    while (got < size && s->left != 0) {
        int n;
        if (s->ppre < s->npre) {
            n = s->npre - s->ppre < size - got ? s->npre - s->ppre : size - got;
            memcpy((char *)buf + got, s->pre + s->ppre, n);
            s->ppre += n;
        } else {
            n = (int)read(s->fd, (char *)buf + got, size - got);
            if (n < 0)
                return -1;
            if (n == 0)
                break;
        }
        got += n;
        if (s->left > 0)
            s->left -= n;
    }
    return got;
}

void net_close(net_stream *s)
{
    if (!s)
        return;
    close(s->fd);
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
