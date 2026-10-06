/*
 * plex_int.h - what plex.c and jellyfin.c share: the JSON helpers, and
 * making and freeing items. Not for the front end.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_PLEX_INT_H
#define MATINEE_PLEX_INT_H
#include "plex.h"
#include "cJSON.h"

#define API_TIMEOUT   15000     /* ms */
#define PROBE_TIMEOUT 3000      /* ms, trying one of a server's addresses */
#define PAGE_SIZE     2000      /* items fetched for one list (one page: plex_list_more for the rest) */

void px_err(plex_ctx *c, const char *fmt, const char *arg);
/* GET (post NULL) or POST, the answer parsed; NULL on error (c->err) */
cJSON *px_get_json(plex_ctx *c, const char *url, const char *token, const char *post, int timeout, int *status);
const char *px_jstr(const cJSON *o, const char *k);
double px_jnum(const cJSON *o, const char *k, double def);
int px_jbool(const cJSON *o, const char *k);
char *px_dup(const char *s);
/* room for one more item in l (cap: its room so far); 0 = ok */
int px_grow(plex_list *l, int *cap);
void px_item_free(plex_item *it);
/* The headers for the API's JSON (gzip allowed) */
void px_api_headers(plex_ctx *c, const char *token, char *out, size_t size);

#endif
