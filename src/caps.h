/*
 * caps.h - what Reel can play on RISC OS, and how Matinee asks for it.
 *
 * One table of limits (caps_t) is used twice:
 *   - to decide whether a file can be played as it is (direct play: Reel
 *     reads the file from the server, nothing is converted);
 *   - otherwise, to tell the server what to convert it to: an HLS stream
 *     of H.264 + stereo AAC within the limits, asked for through
 *     X-Plex-Client-Profile-Extra on the universal transcoder.
 *
 * The limits come from Reel's measurements on a Pi 4: H.264 decoding is
 * one core (CABAC can't use NEON), so the bit rate matters as much as the
 * size; 8-bit 4:2:0 only (High 10 and 4:4:4 are far too slow); 1080p up to
 * 30 pictures a second, 720p up to 60. Reel mixes any sound down to stereo
 * itself, so direct play takes 5.1 AC-3 or AAC as it is.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_CAPS_H
#define MATINEE_CAPS_H
#include <stddef.h>
#include "plex.h"

enum { Q_1080, Q_720, Q_480, Q_COUNT };

typedef struct {
    int max_w, max_h;       /* pixels */
    int max_kbps;           /* the whole file's bit rate */
    int h264_level;         /* 41 = 4.1 */
    int max_channels;       /* sound channels in a converted stream */
    int max_fps_1080;       /* above 720 lines */
    int own_subs;           /* the player draws subtitles itself (the built-in
                               player): a track chosen needn't be burnt in */
} caps_t;

typedef struct {
    int direct;             /* 1: the file as it is; 0: converted by the server */
    char url[4096];
    char headers[1024];     /* X-Plex-* (with the token), for Reel */
    char why[160];          /* for the status line: what, and why */
    char key[160];          /* Reel's "carry on" key (direct play only), or "" */
    long offset_s;          /* converted streams: where it starts */
    char session[32];       /* the session id (the stream's, the timeline's) */
} play_t;

void caps_for(int quality, caps_t *out);
const char *caps_quality_name(int quality);

/* 1 if it can be played as it is; why says what it is, or why not */
int caps_direct_ok(const caps_t *k, const plex_item *it, char *why, size_t size);

/* 1 if the built-in player can show that subtitle track itself: one in
   the file (text or pictures), or a text file of its own beside it */
int caps_sub_own(const plex_sub *sb);

/* The X-Plex-Client-Profile-Extra value (not URL-escaped) */
void caps_profile_extra(const caps_t *k, char *out, size_t size);

/* What to hand Reel for it. allow_direct: the Choices option;
   resume: start a converted stream at the item's view offset. 0 = ok. */
int caps_play(const plex_ctx *c, const plex_item *it, const caps_t *k, int allow_direct,
              int resume, play_t *out);
/* The same, a converted stream starting offset_s seconds in; session: the
   playback's session id to use again (a new sound track, the next part of
   one playback), or NULL for a new one */
int caps_play_at(const plex_ctx *c, const plex_item *it, const caps_t *k, int allow_direct,
                 long offset_s, const char *session, play_t *out);
/* A new session id */
void caps_session_id(const plex_ctx *c, char *out, size_t size);

#endif
