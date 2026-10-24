/*
 * bgreq.c - one HTTP GET without waiting (see bgreq.h).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "bgreq.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int bgreq_active(const bgreq *r)
{
    return r->fd >= 0 && r->state != BG_IDLE;
}

void bgreq_close(bgreq *r)
{
    if (r->fd >= 0)
        close(r->fd);
    r->fd = -1;
    r->state = BG_IDLE;
    free(r->buf);
    r->buf = NULL;
    r->len = r->cap = 0;
}

/* Adds n bytes to what's kept: 0, or -1 (too big, or no memory) */
static int keep_add(bgreq *r, const char *b, size_t n)
{
    if (r->len + n + 1 > r->cap) {    /* (and a 0 after it, for strtoul) */
        size_t cap = r->cap ? r->cap * 2 : 65536;
        char *nb;
        while (cap < r->len + n + 1)
            cap *= 2;
        if (cap > BGREQ_MAX + 65536 || !(nb = realloc(r->buf, cap)))
            return -1;
        r->buf = nb;
        r->cap = cap;
    }
    memcpy(r->buf + r->len, b, n);
    r->len += n;
    r->buf[r->len] = 0;
    return 0;
}

/* n bytes at s: where `what` (k bytes) first is, or NULL */
static char *find(char *s, size_t n, const char *what, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
        if (s[i] == what[0] && !memcmp(s + i, what, k))
            return s + i;
    return NULL;
}

/* A header's value in the answer's head (head .. end), case ignored; NULL if it isn't there */
static const char *header(const char *head, const char *end, const char *name)
{
    size_t n = strlen(name);
    for (const char *l = head; l && l < end; l = memchr(l, '\n', (size_t)(end - l)), l = l ? l + 1 : NULL)
        if ((size_t)(end - l) > n && !strncasecmp(l, name, n) && l[n] == ':')
            return l + n + 1;
    return NULL;
}

/* The kept answer made into its body (moved to the start of buf, len its
   size): the head taken off, and a chunked body put back together. 0, or
   -1 if it's not a whole HTTP answer. */
static int keep_body(bgreq *r)
{
    char *end = r->len ? find(r->buf, r->len, "\r\n\r\n", 4) : NULL;
    const char *te, *cl;
    size_t head, out = 0;
    if (!end)
        return -1;
    head = (size_t)(end - r->buf) + 4;
    te = header(r->buf, end, "Transfer-Encoding");
    cl = header(r->buf, end, "Content-Length");
    if (te && strstr(te, "chunked") && strstr(te, "chunked") < end) {
        size_t at = head;
        for (;;) {                  /* size (hex) CRLF data CRLF ... 0 CRLF */
            char *e;
            unsigned long n = strtoul(r->buf + at, &e, 16);
            char *crlf = find(r->buf + at, r->len - at, "\r\n", 2);
            if (!crlf || e == r->buf + at)
                return -1;
            at = (size_t)(crlf - r->buf) + 2;
            if (n == 0)
                break;
            if (n > r->len - at)
                return -1;          /* cut short */
            memmove(r->buf + out, r->buf + at, n);
            out += n;
            at += n + 2;
            if (at > r->len)
                return -1;
        }
        r->len = out;
        return 0;
    }
    if (cl && strtoul(cl, NULL, 10) > r->len - head)
        return -1;                  /* cut short */
    memmove(r->buf, r->buf + head, r->len - head);
    r->len -= head;
    return 0;
}

char *bgreq_take(bgreq *r, size_t *len)
{
    char *b = r->buf;
    *len = b ? r->len : 0;
    r->buf = NULL;
    r->len = r->cap = 0;
    return b;
}

int bgreq_start(bgreq *r, const char *url, const char *headers, int keep)
{
    return bgreq_send(r, "GET", url, headers, NULL, keep);
}

int bgreq_send(bgreq *r, const char *method, const char *url, const char *headers, const char *body, int keep)
{
    size_t blen = body ? strlen(body) : 0;
    char host[64];
    const char *p, *path;
    size_t hl;
    int port = 80, on = 1, n;
    struct sockaddr_in sa;

    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->keep = keep;
    if (strncmp(url, "http://", 7))
        return -1;
    p = url + 7;
    hl = strcspn(p, ":/");
    if (!hl || hl >= sizeof(host))
        return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    p += hl;
    if (*p == ':') {
        port = atoi(p + 1);
        p += 1 + strspn(p + 1, "0123456789");
    }
    path = *p == '/' ? p : "/";
    if (port <= 0 || port > 65535)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (!inet_aton(host, &sa.sin_addr))
        return -1;                  /* a name: looking it up would wait */
    if (blen)
        n = snprintf(r->out, sizeof(r->out),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%sContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                     method, path, host, port, headers ? headers : "",
                     *body == '<' ? "text/xml; charset=\"utf-8\"" : "application/json", (unsigned)blen, body);
    else
        n = snprintf(r->out, sizeof(r->out), "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%sConnection: close\r\n\r\n", method,
                     path, host, port, headers ? headers : "");
    if (n < 0 || (size_t)n >= sizeof(r->out))
        return -1;
    r->out_len = (size_t)n;
    if ((r->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    if (ioctl(r->fd, FIONBIO, &on) < 0) {
        bgreq_close(r);
        return -1;
    }
    if (connect(r->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
        r->state = BG_SENDING;
    else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY)
        r->state = BG_CONNECTING;
    else {
        bgreq_close(r);
        return -1;
    }
    return 0;
}

/* The socket is ready for writing (or reading) now: select without waiting */
static int ready(int fd, int write)
{
    fd_set s;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&s);
    FD_SET(fd, &s);
    return select(fd + 1, write ? NULL : &s, write ? &s : NULL, NULL, &tv) > 0;
}

int bgreq_step(bgreq *r)
{
    if (r->fd < 0)
        return -1;
    if (r->state == BG_CONNECTING) {
        int e = 0;
        socklen_t el = sizeof(e);
        if (!ready(r->fd, 1))
            return 0;
        if (getsockopt(r->fd, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
            bgreq_close(r);
            return -1;
        }
        r->state = BG_SENDING;
    }
    if (r->state == BG_SENDING) {
        ssize_t n = send(r->fd, r->out + r->sent, r->out_len - r->sent, 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOTCONN)
                return 0;
            bgreq_close(r);
            return -1;
        }
        r->sent += (size_t)n;
        if (r->sent < r->out_len)
            return 0;
        r->state = BG_READING;
    }
    if (r->state == BG_READING) {
        char buf[16384];
        for (;;) {                  /* what has come: the status line kept (and all of it, if keep) */
            ssize_t n = recv(r->fd, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                    return 0;
                bgreq_close(r);
                return -1;
            }
            if (n == 0)
                break;              /* the server closed it: the answer's all here */
            if (r->keep && keep_add(r, buf, (size_t)n)) {
                bgreq_close(r);
                return -1;
            }
            if (r->in_len < sizeof(r->in) - 1) {
                size_t k = (size_t)n < sizeof(r->in) - 1 - r->in_len ? (size_t)n : sizeof(r->in) - 1 - r->in_len;
                memcpy(r->in + r->in_len, buf, k);
                r->in_len += k;
                r->in[r->in_len] = 0;
            }
        }
        if (!strncmp(r->in, "HTTP/1.", 7) && r->in_len >= 12)
            r->status = atoi(r->in + 9);
        if (r->fd >= 0)
            close(r->fd);
        r->fd = -1;
        r->state = BG_IDLE;
        if (!r->status || (r->keep && keep_body(r))) {
            bgreq_close(r);
            return -1;
        }
        return 1;
    }
    return -1;
}
