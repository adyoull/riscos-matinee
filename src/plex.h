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

/* A subtitle track (Plex's streamType 3), or a sound track (streamType 2) */
typedef struct {
    long id;                /* Plex's stream id */
    char *title;            /* "English (SRT)", as Plex names it */
    char *codec;            /* srt, ass, pgs, vobsub, mov_text... */
    char *language;         /* "English", or NULL */
    int forced, selected;   /* selected: the user's choice, kept by the server */
    int external;           /* a file of its own beside the video (it has a key) */
    char *key;              /* /library/streams/<id> (external only) */
} plex_sub;
typedef plex_sub plex_audio;

/* Someone in the cast (Plex's Role): the actor, the part, and a photo (a
   URL the server fetches for us through /photo/:/transcode), or NULL */
typedef struct {
    char *name, *role, *thumb;
} plex_person;

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
    int unwatched;          /* a show's or season's episodes not yet seen */
    /* the first Media / Part (what "play" plays) */
    char *container, *vcodec, *acodec, *vprofile;
    int width, height, bitrate_kbps, channels, bit_depth;
    double fps;
    char *part_key;         /* /library/parts/<id>/<n>/file.ext */
    char *part_file;        /* the file's name on the server */
    int64_t part_size;
    long part_id;           /* Part's id (choosing subtitles goes by it) */
    /* for the details panel */
    char *summary, *art, *content_rating, *tagline;
    int year;
    double rating;          /* the critics' or audience's, out of 10; 0 = none */
    /* an episode's place: its number, its season's, and the show's */
    int index, parent_index;
    char *grandparent_key;  /* the show's ratingKey (for the next episode) */
    char *grandparent_title;
    char *show_thumb;       /* an episode's show's poster (grandparentThumb), or NULL */
    /* the rest of the metadata (a video's details, plex_details()): names
       joined with ", ", or NULL */
    char *genres, *directors, *writers, *studio, *country;
    char *released;         /* originallyAvailableAt: "2008-04-10" (an episode's air date) */
    char *guid;             /* plex://movie/..., for the timeline */
    double audience_rating; /* out of 10; 0 = none */
    plex_person *cast;
    int ncast;
    /* subtitle tracks: only in an item from plex_details() (lists leave
       them out) */
    plex_sub *subs;
    int nsubs;
    plex_audio *auds;       /* sound tracks (plex_details() only, too) */
    int nauds;
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

/* One video's full details (summary, art, and its subtitle tracks), as a
   list of one item. 0 = ok. */
int plex_details(plex_ctx *c, const plex_item *it, plex_list *out);

/* The subtitle track chosen, or -1 for none */
int plex_sub_selected(const plex_item *it);

/* Chooses a subtitle track (Plex's stream id; 0 = none) for the video's
   file, on the server: other Plex apps see the choice too, and a converted
   stream burns it in. it must have its part_id (plex_details). 0 = ok. */
int plex_set_subtitle(plex_ctx *c, const plex_item *it, long stream_id);

/* The sound track chosen (selected, else the first), or -1 for none */
int plex_audio_selected(const plex_item *it);
/* Chooses a sound track for the video's file, on the server (a converted
   stream then has that one). 0 = ok. */
int plex_set_audio(plex_ctx *c, const plex_item *it, long stream_id);

/* What's being played, as the server knows it: its play queue (the Plex
   apps play from one; the dashboard's Now Playing goes by it) and the
   session id the stream was asked for with */
typedef struct {
    long pq_id, pq_item_id;
    int pq_version;
    char session[40];
} plex_playing;

/* Makes a play queue of one video (POST /playQueues); fills pl's pq_*.
   0 = ok (playing goes on without one if not) */
int plex_play_queue(plex_ctx *c, const plex_item *it, plex_playing *pl);

/* Tells the server where playing has got to: state "playing", "paused" or
   "stopped", time and duration in ms, with the play queue and session of
   pl (may be NULL). Continue watching, Resume and the dashboard's Now
   Playing come from this. 0 = ok. */
int plex_timeline(plex_ctx *c, const plex_item *it, const char *state, int64_t time_ms, int64_t duration_ms,
                  const plex_playing *pl);

/* Searches every library: films, then shows, then episodes (as tiles:
   "Film", "Show" or the episode's show and number under the title).
   An empty query gives an empty list without asking. 0 = ok. */
int plex_search(plex_ctx *c, const char *query, plex_list *out);
/* Asks the server to stop converting for a session. 0 = ok. */
int plex_transcode_stop(plex_ctx *c, const char *session);
/* Tells the server a session's conversion is still wanted (paused). 0 = ok. */
int plex_transcode_ping(plex_ctx *c, const char *session);
/* The episode after it (the next in the show, across seasons), as a list
   of one; n = 0 if it was the last. 0 = ok. */
int plex_next_episode(plex_ctx *c, const plex_item *it, plex_list *out);

/* Tells the server an item was watched (1) or not (0). 0 = ok. */
int plex_mark(plex_ctx *c, const plex_item *it, int watched);

/* The address and headers for the file itself (for saving it) */
void plex_part_url(const plex_ctx *c, const plex_item *it, char *url, size_t size);

#endif
