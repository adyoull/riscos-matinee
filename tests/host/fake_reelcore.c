/*
 * fake_reelcore.c - a stand-in for riscos-ffmpeg's reelcore, for ui_test.
 *
 * It plays nothing: a "video" of FAKE_W x FAKE_H that opens after a few
 * updates (REELCORE_ASYNC), moves on with the fake Wimp's clock (as the
 * sound clock would), gives a new picture every 4 cs and ends after
 * FAKE_LEN seconds. Its pictures are a gradient with a band whose place
 * follows the position, so a picture drawn can be told from the one
 * before. What the player asked for is kept in fake_rc for the checks.
 * reelcore itself is tested in riscos-ffmpeg (reel_test and the rest).
 * Part of riscos-plex. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reelcore.h"
#include "fake_reelcore.h"

struct ReelCore {
    int opening;                /* updates left before it's open */
    int fail, ready, paused, ended;
    double pos, dur;
    int last_cs, last_frame;
    int track, tracks;
    double vol;
    long long bytes;
};

fake_rc_t fake_rc;

static double fake_len(const char *url)
{
    return strstr(url, ".m3u8") ? 0 : fake_rc.len > 0 ? fake_rc.len : FAKE_LEN;   /* a converted stream doesn't say */
}

ReelCore *reelcore_open_source(const ReelCoreSource *src, int flags)
{
    ReelCore *v = calloc(1, sizeof(*v));
    fake_rc.opens++;
    snprintf(fake_rc.url, sizeof(fake_rc.url), "%s", src->url);
    snprintf(fake_rc.headers, sizeof(fake_rc.headers), "%s", src->headers ? src->headers : "");
    fake_rc.async = (flags & REELCORE_ASYNC) != 0;
    fake_rc.open_now++;
    v->opening = 3;
    v->fail = strstr(src->url, "fail") != NULL;
    v->dur = fake_len(src->url);
    v->tracks = strstr(src->url, ".m3u8") ? 1 : 2;
    v->vol = 1;
    v->last_frame = -1;
    return v;
}

ReelCore *reelcore_open(const char *url, int flags)
{
    ReelCoreSource s = { url, NULL, NULL, NULL, NULL };
    return reelcore_open_source(&s, flags);
}

void reelcore_close(ReelCore *v)
{
    if (!v)
        return;
    fake_rc.closes++;
    fake_rc.open_now--;
    free(v);
}

int reelcore_is_network(const char *url) { return strstr(url, "://") != NULL; }
int reelcore_ready(const ReelCore *v) { return v->fail && !v->opening ? -1 : v->ready; }
const char *reelcore_last_error(void) { return "Server returned 404 Not Found"; }

int reelcore_update(ReelCore *v)
{
    int now = fake_rc_cs(), frame;
    fake_rc.updates++;
    if (!v->ready) {
        if (v->opening > 0) {
            v->opening--;
            return REELCORE_OPENING;
        }
        if (v->fail)
            return REELCORE_FAILED;
        v->ready = 1;
        v->last_cs = now;
        return REELCORE_READY;
    }
    if (v->paused || v->ended)
        return REELCORE_SAME_FRAME;
    v->pos += (now - v->last_cs) / 100.0;
    v->bytes += (long long)(now - v->last_cs) * 6250;     /* 5 Mbit/s */
    v->last_cs = now;
    if (v->dur > 0 && v->pos >= v->dur) {
        v->pos = v->dur;
        v->ended = 1;
        return REELCORE_END;
    }
    if (fake_rc.stream_end > 0 && v->pos >= fake_rc.stream_end) {
        v->ended = 1;
        return REELCORE_END;
    }
    frame = (int)(v->pos * 25);
    if (frame != v->last_frame) {
        v->last_frame = frame;
        fake_rc.frames++;
        return REELCORE_NEW_FRAME;
    }
    return REELCORE_SAME_FRAME;
}

double reelcore_idle_time(ReelCore *v) { (void)v; return 0.04; }
double reelcore_position(const ReelCore *v) { return v->pos + fake_rc.pos_offset; }
double reelcore_duration(const ReelCore *v) { return v->dur; }
int reelcore_width(const ReelCore *v) { (void)v; return FAKE_W; }
int reelcore_height(const ReelCore *v) { (void)v; return FAKE_H; }
double reelcore_frame_rate(const ReelCore *v) { (void)v; return 25; }
int reelcore_has_audio(const ReelCore *v) { (void)v; return 1; }
unsigned reelcore_dropped_frames(const ReelCore *v) { (void)v; return 0; }

int reelcore_frame_size(const ReelCore *v, int *w, int *h)
{
    if (!v->ready)
        return -11;
    *w = FAKE_W;
    *h = FAKE_H;
    return 0;
}

void reelcore_pause(ReelCore *v, int paused)
{
    v->paused = paused;
    v->last_cs = fake_rc_cs();
}
int reelcore_paused(const ReelCore *v) { return v->paused; }

int reelcore_seek(ReelCore *v, double s)
{
    fake_rc.seeks++;
    fake_rc.seek_to = s;
    v->pos = s < 0 ? 0 : s;
    v->ended = 0;
    v->last_frame = -1;
    return 0;
}

void reelcore_set_volume(ReelCore *v, double vol) { v->vol = vol; fake_rc.volume = vol; }
double reelcore_volume(const ReelCore *v) { return v->vol; }

int reelcore_audio_tracks(const ReelCore *v) { return v->tracks; }
int reelcore_audio_track(const ReelCore *v) { return v->track; }
int reelcore_audio_track_name(const ReelCore *v, int t, char *buf, int size)
{
    (void)v;
    return snprintf(buf, size, t == 0 ? "aac, 2 ch, eng" : "ac3, 6 ch, fre, Commentary");
}
int reelcore_set_audio_track(ReelCore *v, int t)
{
    if (t < 0 || t >= v->tracks)
        return -1;
    v->track = t;
    fake_rc.track = t;
    return 0;
}

int reelcore_net(const ReelCore *v, ReelCoreNet *st)
{
    memset(st, 0, sizeof(*st));
    st->opening = !v->ready;
    st->buffering = fake_rc.buffering;
    st->ahead = fake_rc.buffering ? 0 : 8;
    st->bytes_ahead = fake_rc.buffering ? 0 : 5 << 20;
    st->bytes_read = v->bytes;
    return 1;
}

int reelcore_info(const ReelCore *v, char *buf, int size)
{
    (void)v;
    return snprintf(buf, size, "h264 %dx%d, 25 fps; aac 48000 Hz, 2 channels; fake", FAKE_W, FAKE_H);
}

int reelcore_debug(const ReelCore *v, char *buf, int size)
{
    return snprintf(buf, size, "pos %.2f", v->pos);
}

int reelcore_media_info(const ReelCore *v, char *buf, int size)
{
    (void)v;
    return snprintf(buf, size, "#File\nContainer\tfake\n#Video\nCodec\th264\n#Audio\nCodec\taac\n");
}

void reelcore_stats(const ReelCore *v, ReelCoreStats *st)
{
    memset(st, 0, sizeof(*st));
    st->position = v->pos;
    st->clock = v->pos;
    st->fps = 25;
    st->decoded = st->shown = (unsigned)fake_rc.frames;
    st->decode_time = fake_rc.frames * 0.01;
    st->bytes_read = v->bytes;
    st->sound = 1;
    st->speed = 1;
}

void reelcore_set_log(void (*fn)(int, const char *), int verbose) { (void)fn; (void)verbose; }

/* A gradient, with a white band down the picture that moves with the position */
int reelcore_draw_pixels(ReelCore *v, void *pixels, int pitch, int w, int h, int bgr, int flags)
{
    int band = (int)(v->pos * 40) % (w > 0 ? w : 1);
    (void)bgr;
    fake_rc.draws++;
    fake_rc.draw_w = w;
    fake_rc.draw_h = h;
    fake_rc.draw_flags = flags;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch);
        for (int x = 0; x < w; x++)
            row[x] = x >= band && x < band + 8 ? 0xFFFFFF : (uint32_t)((x * 255 / w) | (y * 255 / h) << 8 | 0x40 << 16);
    }
    if (fake_rc.panel_rows && h > 20)               /* the panel's place: a grey block */
        for (int y = 4; y < 20; y++)
            for (int x = 4; x < 60 && x < w; x++)
                ((uint32_t *)((uint8_t *)pixels + (size_t)y * pitch))[x] = 0x808080;
    return 0;
}

int reelcore_draw_yuv420(ReelCore *v, uint8_t *const planes[3], const int pitch[3], int w, int h, int *colour)
{
    (void)v;
    if (colour)
        *colour = REELCORE_YUV_709;
    if (!planes)
        return 0;
    fake_rc.yuv_draws++;
    fake_rc.yuv_w = w;
    fake_rc.yuv_h = h;
    for (int y = 0; y < h; y++)
        memset(planes[0] + (size_t)y * pitch[0], 128, w);
    for (int y = 0; y < h / 2; y++) {
        memset(planes[1] + (size_t)y * pitch[1], 100, w / 2);
        memset(planes[2] + (size_t)y * pitch[2], 150, w / 2);
    }
    return 0;
}

int reelcore_set_panel(ReelCore *v, const ReelCorePanel *p)
{
    (void)v;
    fake_rc.panel_rows = p ? p->rows : 0;
    fake_rc.panels++;
    return 0;
}

void reelcore_panel_size(const ReelCore *v, int *w, int *h) { (void)v; *w = fake_rc.panel_rows ? 400 : 0; *h = fake_rc.panel_rows * 20; }
