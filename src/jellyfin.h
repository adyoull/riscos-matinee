/*
 * jellyfin.h - Jellyfin servers, through the same calls as Plex's.
 *
 * A Jellyfin server is used as a Plex one is: plex.c's calls see the
 * context's kind (SRV_JELLYFIN) and come here, and the front end's paths
 * (/library/sections/<n>/all, /library/metadata/<id>/children,
 * /library/onDeck...) are turned into Jellyfin's API:
 *
 *   sign in:  GET /System/Info/Public (is it Jellyfin, its name); Quick
 *             Connect (POST /QuickConnect/Initiate, a code to type into
 *             Jellyfin on a phone or computer, GET /QuickConnect/Connect
 *             until it's allowed, then POST /Users/AuthenticateWithQuickConnect)
 *             or a name and password (POST /Users/AuthenticateByName)
 *   browse:   /Users/<user>/Views (the libraries), /Users/<user>/Items
 *             (a library, a folder, a search), /Shows/<id>/Seasons and
 *             /Episodes, /Users/<user>/Items/Resume and /Shows/NextUp
 *             (Continue watching), /Users/<user>/Items/Latest
 *   play:     the file itself (/Videos/<id>/stream?static=true), or
 *             POST /Items/<id>/PlaybackInfo with what Reel can play (a
 *             DeviceProfile) for an HLS stream the server converts
 *   progress: POST /Sessions/Playing, .../Progress, .../Stopped;
 *             /Users/<user>/PlayedItems/<id> (watched)
 *
 * Every request carries "Authorization: MediaBrowser Client=..., Device=...,
 * DeviceId=..., Version=..., Token=...". Jellyfin keeps no choice of
 * subtitles or sound track for a file, so those are remembered here while
 * Matinee runs.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_JELLYFIN_H
#define MATINEE_JELLYFIN_H
#include <stddef.h>
#include "plex.h"
#include "caps.h"

#define JF_PORT 8096

/* Jellyfin's Authorization header (and Accept: JSON), with token if not NULL */
void jf_headers(const plex_ctx *c, const char *token, char *out, size_t size);

/* The server at addr (http://host:8096, or just the host): checks it is a
   Jellyfin server and makes it the one in use (kind, base, name, id), not
   yet signed in. 0 = ok; -2 = it answers, but isn't Jellyfin. */
int jf_connect(plex_ctx *c, const char *addr);
/* Quick Connect: a code to type into Jellyfin (its user menu, Quick
   Connect), and the secret to check it with. 0 = ok; -2 = Quick Connect is
   off on the server. */
int jf_qc_start(plex_ctx *c, char *secret, size_t slen, char *code, size_t clen);
/* 1 = allowed (signed in: token and user set), 0 = not yet, -1 = the code
   has gone (or an error) */
int jf_qc_check(plex_ctx *c, const char *secret);
/* A name and password. 0 = signed in; -2 = the server refused them. */
int jf_login(plex_ctx *c, const char *user, const char *password);
/* Signs out on the server (its token is no use after). 0 = ok. */
int jf_logout(plex_ctx *c);

/* plex.c's calls, for a Jellyfin server */
int jf_fetch(plex_ctx *c, const char *path, int size, plex_list *out);
int jf_search(plex_ctx *c, const char *query, plex_list *out);
int jf_set_subtitle(plex_ctx *c, const plex_item *it, long stream_id);
int jf_set_audio(plex_ctx *c, const plex_item *it, long stream_id);
int jf_remove_continue(plex_ctx *c, const plex_item *it);
int jf_rate(plex_ctx *c, const plex_item *it, int rating);
int jf_play_queue(plex_ctx *c, const plex_item *it, plex_playing *pl);
int jf_timeline(plex_ctx *c, const plex_item *it, const char *state, int64_t time_ms, int64_t duration_ms,
                const plex_playing *pl);
int jf_transcode_call(plex_ctx *c, const char *what, const char *session);
int jf_poster(plex_ctx *c, const char *thumb, int w, int h, char **jpeg, size_t *len);
int jf_mark(plex_ctx *c, const plex_item *it, int watched);
/* caps_play_at's, for a Jellyfin server */
int jf_play(const plex_ctx *c, const plex_item *it, const caps_t *k, int allow_direct, long offset_s,
            const char *session, play_t *out);

/* The DeviceProfile asked for (the tests look at it); the caller frees it */
char *jf_device_profile(const caps_t *k);

#endif
