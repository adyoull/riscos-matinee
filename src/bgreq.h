/*
 * bgreq.h - one HTTP GET to a server given by its IP address, sent and
 * answered a piece at a time, never waiting: each bgreq_step() does what
 * the socket allows at once and returns.
 *
 * For the built-in player's reports every 10 s (ui.c). Sent from a thread,
 * a report still cost a few pictures each time on a Pi; this needs no
 * thread, no DNS (a name would have to be looked up, which waits) and no
 * TLS (an https server gets the thread). The answer's status is all that's
 * read; the connection is closed after it ("Connection: close").
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
} bgreq;

enum { BG_IDLE, BG_CONNECTING, BG_SENDING, BG_READING };

/* Starts the request (url: http://a.b.c.d[:port]/path...; headers: lines
   ending \r\n). 0 = started; -1 = not such an address (an https one, or a
   name), or the socket couldn't be made: send it another way. */
int bgreq_start(bgreq *r, const char *url, const char *headers);
/* 0 = still going; 1 = answered (r->status); -1 = failed (and closed) */
int bgreq_step(bgreq *r);
/* Closes it, answered or not */
void bgreq_close(bgreq *r);
/* Going? */
int bgreq_active(const bgreq *r);

#endif
