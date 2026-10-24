/*
 * bgreq.h - one HTTP request to a server given by its IP address, sent and
 * answered a piece at a time, never waiting: each bgreq_step() does what
 * the socket allows at once and returns.
 *
 * For the built-in player's reports every 10 s, and the posters and
 * photos fetched while browsing (ui.c). Sent from a thread, a report still
 * cost a few pictures each time on a Pi, and fetched as before, each
 * picture held up the whole desktop until the server answered; this needs
 * no thread, no DNS (a name would have to be looked up, which waits) and
 * no TLS (an https server is sent to the old way). The connection is
 * closed after the answer ("Connection: close"). A report keeps only the
 * answer's status; a picture keeps its body too (keep), with a chunked
 * one put back together.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_BGREQ_H
#define MATINEE_BGREQ_H

#include <stddef.h>

typedef struct {
    int fd;                 /* -1: none */
    int state;              /* BG_* */
    char out[3000];         /* the request */
    size_t out_len, sent;
    char in[16];            /* the answer's first bytes: "HTTP/1.1 200" */
    size_t in_len;
    int status;             /* the answer's status (0 until read) */
    int keep;               /* keep the answer (bgreq_take) */
    char *buf;              /* kept: all of it as it came, then the body */
    size_t len, cap;
} bgreq;

#define BGREQ_MAX (32u << 20)   /* the most of an answer kept */

enum { BG_IDLE, BG_CONNECTING, BG_SENDING, BG_READING };

/* Starts the request (url: http://a.b.c.d[:port]/path...; headers: lines
   ending \r\n). 0 = started; -1 = not such an address (an https one, or a
   name), or the socket couldn't be made: send it another way. */
int bgreq_start(bgreq *r, const char *url, const char *headers, int keep);
/* The same with a method ("GET", "POST"...) and a body (NULL or "": none; a
   body starting '<' is sent as XML, else JSON) */
int bgreq_send(bgreq *r, const char *method, const char *url, const char *headers, const char *body, int keep);
/* 0 = still going; 1 = answered (r->status); -1 = failed (and closed) */
int bgreq_step(bgreq *r);
/* After an answer (keep): its body, which the caller frees; NULL if none */
char *bgreq_take(bgreq *r, size_t *len);
/* Closes it, answered or not, and frees what was kept */
void bgreq_close(bgreq *r);
/* Going? */
int bgreq_active(const bgreq *r);

#endif
