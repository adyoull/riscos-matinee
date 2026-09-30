/*
 * handoff.h - what PlexRO gives Reel to play.
 *
 * Reel (0.1.17 and later) plays yt-dlp's JSON: the address, the HTTP
 * headers to send with it, the title, and a key for "carry on from where
 * you stopped" (webpage_url). PlexRO writes the same form, so Reel and
 * ReelEGL need no change to play from a Plex server.
 * Part of riscos-plex. GPL v2 or later.
 */
#ifndef PLEXRO_HANDOFF_H
#define PLEXRO_HANDOFF_H
#include "caps.h"

/* The JSON text for it (malloc'd), NULL if out of memory.
   title: UTF-8; user_agent may be NULL. */
char *handoff_json(const play_t *p, const char *title, const char *user_agent);

/* Writes it to path. 0 = ok. */
int handoff_write(const char *path, const play_t *p, const char *title, const char *user_agent);

#endif
