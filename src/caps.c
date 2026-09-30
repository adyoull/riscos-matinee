/*
 * caps.c - what Reel can play, and the requests for it (see caps.h).
 * Part of riscos-plex. GPL v2 or later.
 */
#include "caps.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const struct {
    const char *name;
    int w, h, kbps;
} qualities[Q_COUNT] = {
    { "1080p (10 Mbit/s)", 1920, 1080, 10000 },
    { "720p (4 Mbit/s)",   1280,  720,  4000 },
    { "480p (2 Mbit/s)",    854,  480,  2000 },
};

void caps_for(int quality, caps_t *out)
{
    if (quality < 0 || quality >= Q_COUNT)
        quality = Q_1080;
    memset(out, 0, sizeof(*out));
    out->max_w = qualities[quality].w;
    out->max_h = qualities[quality].h;
    out->max_kbps = qualities[quality].kbps;
    out->h264_level = 41;
    out->max_channels = 2;
    out->max_fps_1080 = 31;
}

const char *caps_quality_name(int quality)
{
    return quality >= 0 && quality < Q_COUNT ? qualities[quality].name : "?";
}

static int in_list(const char *s, const char *const *list)
{
    if (!s)
        return 0;
    for (; *list; list++)
        if (!strcasecmp(s, *list))
            return 1;
    return 0;
}

static const char *const containers[] = { "mp4", "m4v", "mov", "mkv", "matroska", "avi", "mpegts", "ts", "mpeg", NULL };
/* FFmpeg's decoders in Reel; Reel mixes to stereo itself */
static const char *const audio[] = { "aac", "mp3", "mp2", "ac3", "eac3", "flac", "opus", "vorbis", "pcm", "dca", "dts", NULL };
/* cheap enough to play as they are at DVD sizes */
static const char *const sd_video[] = { "mpeg2video", "mpeg1video", "mpeg4", "msmpeg4v3", "h263", NULL };

int caps_direct_ok(const caps_t *k, const plex_item *it, char *why, size_t size)
{
    const char *vc = it->vcodec ? it->vcodec : "?";
    int h264 = it->vcodec && !strcasecmp(it->vcodec, "h264");
#define NO(...) do { snprintf(why, size, __VA_ARGS__); return 0; } while (0)
    if (!it->part_key)
        NO("no file to play as it is");
    if (!in_list(it->container, containers))
        NO("%s files are converted", it->container ? it->container : "these");
    if (!h264 && !(in_list(it->vcodec, sd_video) && it->height > 0 && it->height <= 576))
        NO("%s video is converted", vc);
    if (h264 && it->vprofile && (strstr(it->vprofile, "10") || strstr(it->vprofile, "4:")))
        NO("H.264 %s is converted (too slow)", it->vprofile);
    if (it->width > k->max_w || it->height > k->max_h)
        NO("%dx%d is bigger than %dx%d", it->width, it->height, k->max_w, k->max_h);
    if (it->bitrate_kbps > k->max_kbps)
        NO("%.1f Mbit/s is more than %.0f", it->bitrate_kbps / 1000.0, k->max_kbps / 1000.0);
    if (it->height > 720 && it->fps > k->max_fps_1080)
        NO("%dp at %.0f a second is too fast", it->height, it->fps);
    if (it->acodec && !in_list(it->acodec, audio))
        NO("%s sound is converted", it->acodec);
#undef NO
    snprintf(why, size, "%s %dx%d, %.1f Mbit/s", h264 ? "H.264" : vc, it->width, it->height,
             it->bitrate_kbps / 1000.0);
    return 1;
}

void caps_profile_extra(const caps_t *k, char *out, size_t size)
{
    snprintf(out, size,
             /* what to convert to: HLS, H.264 in MPEG-TS with AAC */
             "add-transcode-target(type=videoProfile&context=streaming&protocol=hls&container=mpegts"
             "&videoCodec=h264&audioCodec=aac&replace=true)"
             /* and within these limits */
             "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.width&value=%d&replace=true)"
             "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.height&value=%d&replace=true)"
             "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.bitrate&value=%d&replace=true)"
             "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level&value=%d&replace=true)"
             "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.bitDepth&value=8&replace=true)"
             "+add-limitation(scope=videoCodec&scopeName=h264&type=match&name=video.profile"
             "&list=baseline|constrained baseline|main|high&replace=true)"
             "+add-limitation(scope=videoAudioCodec&scopeName=*&type=upperBound&name=audio.channels&value=%d&replace=true)",
             k->max_w, k->max_h, k->max_kbps, k->h264_level, k->max_channels);
}

/* A new transcode session id each time (the server keeps one per id) */
static void session_id(const plex_ctx *c, char *out, size_t size)
{
    static unsigned n;
    unsigned h = 2166136261u;
    for (const char *p = c->client_id; *p; p++)
        h = (h ^ (unsigned char)*p) * 16777619u;
    h ^= (unsigned)time(NULL) * 2654435761u;
    snprintf(out, size, "%08x%04x", h, (++n) & 0xFFFF);
}

int caps_play(const plex_ctx *c, const plex_item *it, const caps_t *k, int allow_direct,
              int resume, play_t *out)
{
    char why[160];
    memset(out, 0, sizeof(*out));
    if (it->kind != PI_VIDEO || !it->rating_key) {
        snprintf(out->why, sizeof(out->why), "that isn't a video");
        return -1;
    }
    /* Reel sends these with every request, the segments of a converted
       stream too (FFmpeg's HLS reader passes its headers on) */
    plex_headers(c, c->token, out->headers, sizeof(out->headers));

    if (allow_direct && caps_direct_ok(k, it, why, sizeof(why))) {
        out->direct = 1;
        snprintf(out->url, sizeof(out->url), "%s%s", c->base, it->part_key);
        snprintf(out->why, sizeof(out->why), "Direct play: %s", why);
        /* Reel's own "carry on from where you stopped" goes by this */
        snprintf(out->key, sizeof(out->key), "plex:%s/%s", c->server_id, it->rating_key);
        return 0;
    }
    if (!allow_direct)
        snprintf(why, sizeof(why), "direct play is off");
    {
        char extra[1400], extra_esc[2800], path[128], path_esc[256], sid[32];
        char tok_esc[512], id_esc[128], prod_esc[64];
        snprintf(path, sizeof(path), "/library/metadata/%s", it->rating_key);
        net_escape(path, path_esc, sizeof(path_esc));
        caps_profile_extra(k, extra, sizeof(extra));
        net_escape(extra, extra_esc, sizeof(extra_esc));
        net_escape(c->token, tok_esc, sizeof(tok_esc));
        net_escape(c->client_id, id_esc, sizeof(id_esc));
        net_escape(c->product, prod_esc, sizeof(prod_esc));
        session_id(c, sid, sizeof(sid));
        out->offset_s = resume && it->view_offset_ms > 0 ? (long)(it->view_offset_ms / 1000) : 0;
        snprintf(out->url, sizeof(out->url),
                 "%s/video/:/transcode/universal/start.m3u8?hasMDE=1&path=%s&mediaIndex=0&partIndex=0"
                 "&protocol=hls&fastSeek=1&directPlay=0&directStream=1&directStreamAudio=1"
                 "&videoQuality=100&videoResolution=%dx%d&maxVideoBitrate=%d"
                 "&subtitles=burn&subtitleSize=100&audioBoost=100&location=%s&offset=%ld"
                 "&session=%s&X-Plex-Session-Identifier=%s"
                 "&X-Plex-Product=%s&X-Plex-Platform=%s&X-Plex-Client-Identifier=%s"
                 "&X-Plex-Client-Profile-Extra=%s&X-Plex-Token=%s",
                 c->base, path_esc, k->max_w, k->max_h, k->max_kbps,
                 c->local ? "lan" : "wan", out->offset_s, sid, sid,
                 prod_esc, c->platform, id_esc, extra_esc, tok_esc);
        snprintf(out->why, sizeof(out->why), "Converted by the server (%s)", why);
    }
    return 0;
}
