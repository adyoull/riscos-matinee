/*
 * plex.h - the parts of the Plex API PlexRO uses.
 *
 *   sign in:  POST plex.tv/api/v2/pins (a 4-letter code the user types at
 *             plex.tv/link), then GET plex.tv/api/v2/pins/<id> until it
 *             has an authToken
 *   servers:  GET plex.tv/api/v2/resources (each server's addresses and
 *             its own access token); or a server address typed by hand
 *   browse:   GET <server>/library/sections, <section>/all,
 *             <item>/children, /library/onDeck
 *   posters:  GET <server>/photo/:/transcode (a JPEG at the size wanted)
 *
 * All requests carry the X-Plex-* headers that say who the client is, and
 * ask for JSON. What to play, and how, is in caps.h.
 * Part of riscos-plex. GPL v2 or later.
 */
#ifndef PLEXRO_PLEX_H
#define PLEXRO_PLEX_H
#include <stddef.h>
#include <stdint.h>

#define PLEX_TV "https://plex.tv"

typedef struct {
    char client_id[48];     /* made once, kept in Choices */
    char product[32];       /* "PlexRO" */
    char version[16];
    char platform[32];      /* what the server picks its profile by */
    char device[32];
    char device_name[64];
    char account_token[256];/* plex.tv sign-in ("" = not signed in) */
    /* the server in use */
    char base[256];         /* e.g. http://192.168.1.10:32400 (no / at the end) */
    char token[256];        /* the server's access token */
    char server_name[64];
    char server_id[64];     /* its clientIdentifier (machine id) */
    int local;              /* the connection is on the local network */
    char plextv[128];       /* https://plex.tv, or the tests' fake */
    char err[256];          /* what went wrong last */
} plex_ctx;

#define PLEX_CONNS 8
typedef struct {
    char uri[256];
    int local, relay, https;
} plex_conn;

typedef struct {
    char name[64];
    char id[64];
    char token[256];
    int owned;
    int nconn;
    plex_conn conn[PLEX_CONNS];
} plex_server;

typedef enum {
    PI_FOLDER,              /* opens a list (a library, a show, a season) */
    PI_VIDEO,               /* plays (a film, an episode, a clip) */
    PI_OTHER                /* music, photos: shown, not handled yet */
} plex_kind;

typedef struct {
    plex_kind kind;
    char *title;            /* UTF-8 as Plex sends it; ui converts for the desktop */
    char *subtitle;         /* year, "S1 E2", episode count... or NULL */
    char *key;              /* what browsing into it fetches (PI_FOLDER) */
    char *rating_key;
    char *type;             /* Plex's type: movie, show, season, episode... */
    char *thumb;            /* poster path on the server, or NULL */
    int64_t duration_ms, view_offset_ms;
    int watched;            /* viewCount > 0 (or all episodes seen) */
    /* the first Media / Part (what "play" plays) */
    char *container, *vcodec, *acodec, *vprofile;
    int width, height, bitrate_kbps, channels, bit_depth;
    double fps;
    char *part_key;         /* /library/parts/<id>/<n>/file.ext */
    char *part_file;        /* the file's name on the server */
    int64_t part_size;
} plex_item;

typedef struct {
    plex_item *v;
    int n;
    int total;              /* the server's totalSize (may be more than n) */
    char title[128];        /* the list's title (the library's name...) */
} plex_list;

void plex_ctx_init(plex_ctx *c, const char *client_id, const char *version);

/* The X-Plex-* headers (and Accept: JSON), with token if not NULL */
void plex_headers(const plex_ctx *c, const char *token, char *out, size_t size);

/* Sign in: a PIN to type at plex.tv/link. 0 = ok. */
int plex_pin_create(plex_ctx *c, long *id, char *code, size_t codelen);
/* 1 = signed in (account_token set), 0 = not yet, -1 = error / expired */
int plex_pin_check(plex_ctx *c, long id);

/* The account's servers (those that provide "server"), up to max */
int plex_servers(plex_ctx *c, plex_server *out, int max);

/* Tries the server's addresses (local first, http before https on the
   local network, the relay last) and makes the first that answers the one
   in use. 0 = ok. */
int plex_use_server(plex_ctx *c, const plex_server *s);

/* A server given by hand: an address (http://host:32400) and a token
   (may be ""). Checks it answers. 0 = ok. */
int plex_use_address(plex_ctx *c, const char *base, const char *token);

/* A list: path is "" for the top (Continue watching and the libraries),
   or a key from an item. 0 = ok. */
int plex_list_get(plex_ctx *c, const char *path, plex_list *out);
void plex_list_free(plex_list *l);

/* Parses a MediaContainer (for the tests and plex_list_get). path: what it
   was fetched from (relative keys are relative to it). 0 = ok. */
int plex_list_parse(const char *json, const char *path, plex_list *out);

/* A poster: the server's JPEG, w x h pixels at most. 0 = ok (caller frees). */
int plex_poster(plex_ctx *c, const char *thumb, int w, int h, char **jpeg, size_t *len);

/* Tells the server an item was watched (1) or not (0). 0 = ok. */
int plex_mark(plex_ctx *c, const plex_item *it, int watched);

/* The address and headers for the file itself (for saving it) */
void plex_part_url(const plex_ctx *c, const plex_item *it, char *url, size_t size);

#endif
