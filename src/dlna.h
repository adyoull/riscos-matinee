/*
 * dlna.h - DLNA (UPnP AV) media servers, through the same calls as Plex's.
 *
 * A DLNA server (MiniDLNA/ReadyMedia, a NAS's, Plex's or Jellyfin's DLNA
 * side) is used as a Plex one is: plex.c's calls see the context's kind
 * (SRV_DLNA) and come here, and the front end's paths are turned into the
 * ContentDirectory service's Browse and Search:
 *
 *   find:     SSDP (M-SEARCH to 239.255.255.250:1900, for MediaServer:1);
 *             each answer's LOCATION is the device's description (XML):
 *             its name, UDN, and ContentDirectory's control address. A
 *             server can also be given by its address.
 *   browse:   SOAP Browse (BrowseDirectChildren, a page at a time;
 *             BrowseMetadata for one item); the answer's Result is a
 *             DIDL-Lite list: containers (folders) and items, each video
 *             with one or more res: the file, and on some servers (Plex's,
 *             Serviio, UMS) converted streams (DLNA.ORG_CI=1)
 *   paths:    /library/sections: the top level's folders, as libraries
 *             (Music and Pictures left out); /library/metadata/<id>[/children];
 *             /library/onDeck: Continue watching, kept here (below).
 *             An object's id is kept URL-escaped (rating_key), as ids
 *             have '$' and '/' in them.
 *   play:     the file itself if Reel can (the server seldom says the
 *             codec: its DLNA profile, the type and the picture size are
 *             what there is), else the best converted stream the server
 *             offers within the Quality chosen
 *
 * DLNA servers keep no "where you got to" or "watched", so Matinee keeps
 * them, per server and object: a file beside Choices (dlna_places_files).
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_DLNA_H
#define MATINEE_DLNA_H
#include <stddef.h>
#include "plex.h"
#include "caps.h"

#define DLNA_FOUND 12

typedef struct {
    char name[64];          /* its friendlyName */
    char udn[96];           /* uuid:... */
    char location[256];     /* its description's address */
    char host[64];          /* for showing: the address, without http:// */
} dlna_server;

/* Looks for servers for wait_ms (about 2 seconds is plenty). The number
   found (each checked: it has a ContentDirectory), or -1 (err). */
int dlna_discover(dlna_server *out, int max, int wait_ms, char *err, size_t errlen);
/* Where M-SEARCH goes (the tests' fake answers on 127.0.0.1) */
void dlna_set_ssdp(const char *addr, int port);

/* The server at addr made the one in use (kind, base = its description's
   address, name, server_id = its UDN). addr: a description's address, or
   a host (and port), which is asked over SSDP, then at the usual places
   (MiniDLNA's 8200, Plex's 32469). 0 = ok; -2 = it isn't a media server. */
int dlna_connect(plex_ctx *c, const char *addr);

/* Where the places (resume points, watched) are read from and written to */
void dlna_places_files(const char *read_path, const char *write_path);

/* plex.c's calls, for a DLNA server */
int dlna_fetch(plex_ctx *c, const char *path, int start, int size, plex_list *out);
int dlna_search(plex_ctx *c, const char *query, plex_list *out);
int dlna_poster(plex_ctx *c, const char *thumb, char **jpeg, size_t *len);
int dlna_timeline(plex_ctx *c, const plex_item *it, const char *state, int64_t time_ms, int64_t duration_ms);
int dlna_mark(plex_ctx *c, const plex_item *it, int watched);
int dlna_remove_continue(plex_ctx *c, const plex_item *it);
/* caps_play_at's, for a DLNA server */
int dlna_play(const plex_ctx *c, const plex_item *it, const caps_t *k, int allow_direct, long offset_s,
              const char *session, play_t *out);

#endif
