/*
 * net.h - HTTP(S) requests for Matinee, through FFmpeg's avio.
 *
 * On RISC OS https goes through the AcornSSL module (riscos-ffmpeg's
 * patch 0018, tls_acornssl.c), the same code Reel plays web addresses
 * with, so Matinee has no TLS code of its own. On Linux (the host tests)
 * it is the system FFmpeg.
 *
 * Every call blocks until it's done or times out: the front end shows the
 * hourglass meanwhile, and reads big things (a film being saved) a piece
 * at a time from null events.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_NET_H
#define MATINEE_NET_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;         /* NUL terminated (the NUL isn't counted in len) */
    size_t len;
    int status;         /* 200 when it worked, the HTTP error code if known, else 0 */
} net_buf;

void net_init(const char *user_agent);

/* GET (post == NULL) or POST (a form body, may be "") url. headers:
   "Name: value\r\n" lines, or NULL. timeout_ms: how long with no data
   before giving up. 0 on success; else an error, described in err. */
int net_fetch(const char *url, const char *headers, const char *post, net_buf *out,
              int timeout_ms, char *err, size_t errlen);
/* The same with another method ("PUT", "DELETE"...): body is a form, or
   NULL for none (a short one is sent anyway, so there's a Content-Length) */
int net_send(const char *url, const char *headers, const char *method, const char *body,
             net_buf *out, int timeout_ms, char *err, size_t errlen);
void net_buf_free(net_buf *b);

/* A download read a piece at a time */
typedef struct net_stream net_stream;
net_stream *net_open(const char *url, const char *headers, int timeout_ms, char *err, size_t errlen);
int64_t net_size(net_stream *s);        /* -1 if the server didn't say */
int net_read(net_stream *s, void *buf, int size);   /* > 0 bytes, 0 at the end, < 0 an error */
void net_close(net_stream *s);

/* Percent-encodes s for a URL's query (RFC 3986 unreserved kept) */
void net_escape(const char *s, char *out, size_t size);

#endif
