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
}

int bgreq_start(bgreq *r, const char *url, const char *headers)
{
    char host[64];
    const char *p, *path;
    size_t hl;
    int port = 80, on = 1, n;
    struct sockaddr_in sa;

    memset(r, 0, sizeof(*r));
    r->fd = -1;
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
    n = snprintf(r->out, sizeof(r->out), "GET %s HTTP/1.1\r\nHost: %s:%d\r\n%sConnection: close\r\n\r\n", path, host,
                 port, headers ? headers : "");
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
        char buf[512];
        for (;;) {                  /* what has come: the status line kept, the rest read and left */
            ssize_t n = recv(r->fd, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                    return 0;
                bgreq_close(r);
                return -1;
            }
            if (n == 0)
                break;              /* the server closed it: the answer's all here */
            if (r->in_len < sizeof(r->in) - 1) {
                size_t k = (size_t)n < sizeof(r->in) - 1 - r->in_len ? (size_t)n : sizeof(r->in) - 1 - r->in_len;
                memcpy(r->in + r->in_len, buf, k);
                r->in_len += k;
                r->in[r->in_len] = 0;
            }
        }
        if (!strncmp(r->in, "HTTP/1.", 7) && r->in_len >= 12)
            r->status = atoi(r->in + 9);
        bgreq_close(r);
        return r->status ? 1 : -1;
    }
    return -1;
}
