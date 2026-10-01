/*
 * core_test.c - Matinee's core (net, plex, caps, handoff) against
 * fakeplex.py, and the hand-off read back by Reel's own sources.c.
 *   core_test PORT
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "net.h"
#include "plex.h"
#include "caps.h"
#include "handoff.h"
#include "cJSON.h"
#include "sources.h"          /* Reel's (riscos-ffmpeg player/sources.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
static char base[64];

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static cJSON *server_log(void)
{
    char url[128];
    net_buf b;
    cJSON *j;
    snprintf(url, sizeof(url), "%s/_log", base);
    if (net_fetch(url, NULL, NULL, &b, 5000, NULL, 0) != 0)
        return NULL;
    j = cJSON_Parse(b.data);
    net_buf_free(&b);
    return j;
}

static void server_reset(void)
{
    char url[128];
    net_buf b;
    snprintf(url, sizeof(url), "%s/_reset", base);
    if (net_fetch(url, NULL, "", &b, 5000, NULL, 0) == 0)
        net_buf_free(&b);
}

/* The last request to path in the log (a new reference: delete the log) */
static const cJSON *last(const cJSON *log, const char *path)
{
    const cJSON *r, *found = NULL;
    cJSON_ArrayForEach(r, log)
        if (!strcmp(cJSON_GetObjectItem(r, "path")->valuestring, path))
            found = r;
    return found;
}

static int count(const cJSON *log, const char *path)
{
    const cJSON *r;
    int n = 0;
    cJSON_ArrayForEach(r, log)
        if (!strcmp(cJSON_GetObjectItem(r, "path")->valuestring, path))
            n++;
    return n;
}

static const char *hdr(const cJSON *req, const char *name)
{
    const cJSON *h = cJSON_GetObjectItem(cJSON_GetObjectItem(req, "headers"), name);
    return cJSON_IsString(h) ? h->valuestring : "";
}

static const char *qv(const cJSON *req, const char *name)
{
    const cJSON *q = cJSON_GetObjectItem(cJSON_GetObjectItem(req, "query"), name);
    q = cJSON_IsArray(q) ? cJSON_GetArrayItem(q, 0) : NULL;
    return cJSON_IsString(q) ? q->valuestring : "";
}

static const plex_item *find(const plex_list *l, const char *title)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].title, title))
            return &l->v[i];
    return NULL;
}

int main(int argc, char **argv)
{
    plex_ctx c;
    plex_server servers[4];
    plex_list top, films, tv, seasons, eps, deck;
    caps_t k1080, k720;
    long pin;
    char code[16], why[160];
    cJSON *log;
    int n;

    if (argc < 2)
        return 2;
    snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]);
    net_init("Matinee/test");
    plex_ctx_init(&c, "client-123", "0.1.0");
    snprintf(c.plextv, sizeof(c.plextv), "%s", base);
    server_reset();

    /* ---- sign in with a PIN */
    CHECK(plex_pin_create(&c, &pin, code, sizeof(code)) == 0, "pin create: %s", c.err);
    CHECK(pin == 4242 && !strcmp(code, "ABCD"), "pin %ld code %s", pin, code);
    CHECK(plex_pin_check(&c, pin) == 0, "first poll should wait");
    CHECK(plex_pin_check(&c, pin) == 1, "second poll should sign in");
    CHECK(!strcmp(c.account_token, "ACCT-TOKEN"), "account token %s", c.account_token);
    CHECK(plex_pin_check(&c, 999) == -1, "an expired pin is -1");
    log = server_log();
    {
        const cJSON *r = last(log, "/api/v2/pins");
        CHECK(r && !strcmp(cJSON_GetObjectItem(r, "method")->valuestring, "POST"), "pins is a POST");
        CHECK(r && strstr(cJSON_GetObjectItem(r, "body")->valuestring, "strong=false"), "short code asked for");
        CHECK(r && !strcmp(hdr(r, "Content-Type"), "application/x-www-form-urlencoded"), "form type");
        CHECK(r && !strcmp(hdr(r, "X-Plex-Client-Identifier"), "client-123"), "client id sent");
        CHECK(r && !strcmp(hdr(r, "X-Plex-Product"), "Matinee"), "product sent");
        CHECK(r && !strcmp(hdr(r, "Accept"), "application/json"), "asks for JSON");
        CHECK(r && !*hdr(r, "X-Plex-Token"), "no token before signing in");
    }
    cJSON_Delete(log);

    /* ---- Plex Home */
    {
        plex_user u[PLEX_USERS];
        plex_ctx k = c;
        int nu = plex_home_users(&k, u, PLEX_USERS);
        CHECK(nu == 2 && !strcmp(u[0].uuid, "u-admin") && !strcmp(u[0].title, "Andrew") && u[0].admin &&
              u[0].protected_ && !strcmp(u[1].title, "Kids") && !u[1].protected_ && u[1].restricted,
              "Plex Home: Andrew (the owner, a PIN) and Kids (%d: %s)", nu, k.err);
        CHECK(plex_switch_user(&k, "u-admin", "0000") == -2 && !strcmp(k.account_token, "ACCT-TOKEN"),
              "a wrong PIN: -2, the token kept");
        CHECK(plex_switch_user(&k, "u-kids", "") == 0 && !strcmp(k.account_token, "KID-ACCT"), "to Kids: their token");
        CHECK(plex_servers(&k, servers, 4) == 1 && !strcmp(servers[0].token, "KID-SRV"), "the server's token for Kids");
        CHECK(plex_switch_user(&k, "u-admin", "1234") == 0 && !strcmp(k.account_token, "ACCT-TOKEN"),
              "back to Andrew with his PIN");
        log = server_log();
        {
            const cJSON *r = last(log, "/api/v2/home/users/u-admin/switch");
            CHECK(r && !strcmp(cJSON_GetObjectItem(r, "method")->valuestring, "POST") && !strcmp(qv(r, "pin"), "1234") &&
                  !strcmp(hdr(r, "X-Plex-Token"), "KID-ACCT"), "POST .../switch?pin=1234, as the user switching");
        }
        cJSON_Delete(log);
    }

    /* ---- servers */
    n = plex_servers(&c, servers, 4);
    CHECK(n == 1, "one server (the player isn't one), got %d: %s", n, c.err);
    if (n == 1) {
        plex_server *s = &servers[0];
        char want0[64];
        snprintf(want0, sizeof(want0), "http://127.0.0.1:%s", argv[1]);
        CHECK(!strcmp(s->name, "Attic") && !strcmp(s->token, "SRV-TOKEN") && s->owned, "server fields");
        CHECK(s->nconn == 4, "4 addresses (IPv6 left out, local http added), got %d", s->nconn);
        CHECK(!strcmp(s->conn[0].uri, want0) && s->conn[0].local && !s->conn[0].https,
              "local http first: %s", s->conn[0].uri);
        CHECK(s->conn[1].local && s->conn[1].https, "local https second: %s", s->conn[1].uri);
        CHECK(!s->conn[2].local && !s->conn[2].relay, "remote third: %s", s->conn[2].uri);
        CHECK(s->conn[3].relay, "relay last: %s", s->conn[3].uri);
        server_reset();
        CHECK(plex_use_server(&c, s) == 0, "use server: %s", c.err);
        CHECK(!strcmp(c.base, want0) && !strcmp(c.token, "SRV-TOKEN") && !strcmp(c.server_id, "MID") && c.local,
              "server in use: %s %s %s", c.base, c.token, c.server_id);
        log = server_log();
        CHECK(count(log, "/identity") == 1, "only the first address was needed");
        CHECK(!strcmp(hdr(last(log, "/library/sections"), "X-Plex-Token"), "SRV-TOKEN"),
              "the server's own token is used");
        cJSON_Delete(log);
    }
    {
        plex_ctx bad = c;
        CHECK(plex_use_address(&bad, base + 7, "WRONG") != 0, "a wrong token is refused");
        CHECK(strstr(bad.err, "token") != NULL, "and says so: %s", bad.err);
        CHECK(plex_use_address(&bad, base + 7, "SRV-TOKEN") == 0, "address typed by hand: %s", bad.err);
        CHECK(!strcmp(bad.server_name, "Attic") && !strcmp(bad.base, base), "friendly name %s, base %s",
              bad.server_name, bad.base);
    }

    /* ---- browsing */
    CHECK(plex_list_get(&c, "", &top) == 0, "top: %s", c.err);
    CHECK(top.n == 4, "Continue watching + 3 libraries, got %d", top.n);
    if (top.n == 4) {
        CHECK(!strcmp(top.v[0].key, "/library/onDeck") && top.v[0].kind == PI_FOLDER, "continue watching first");
        CHECK(!strcmp(top.v[1].title, "Films") && !strcmp(top.v[1].key, "/library/sections/1/all") &&
              top.v[1].kind == PI_FOLDER, "films: %s", top.v[1].key);
        CHECK(!strcmp(top.v[2].subtitle, "TV") && top.v[2].kind == PI_FOLDER, "tv");
        CHECK(top.v[3].kind == PI_OTHER && !strcmp(top.v[3].subtitle, "Music"), "music shown, not opened");
        CHECK(!strcmp(top.title, "Attic"), "top title %s", top.title);
    }
    CHECK(plex_list_get(&c, "/library/sections/1/all", &films) == 0, "films: %s", c.err);
    CHECK(films.n == 6 && films.total == 6 && !strcmp(films.title, "Films"), "6 films, got %d (%s)", films.n, films.title);
    log = server_log();
    {
        const cJSON *r = last(log, "/library/sections/1/all");
        CHECK(r && !strcmp(hdr(r, "Accept-Encoding"), "gzip"), "lists asked for compressed (the fake gzips them; FFmpeg unzips)");
    }
    cJSON_Delete(log);
    log = server_log();
    {
        const cJSON *r = last(log, "/library/sections/1/all");
        CHECK(r && !strcmp(qv(r, "X-Plex-Container-Size"), "2000"), "paged request");
    }
    cJSON_Delete(log);
    caps_for(Q_1080, &k1080);
    caps_for(Q_720, &k720);
    {
        const plex_item *bb = find(&films, "Big Buck Bunny"), *it;
        CHECK(bb && bb->kind == PI_VIDEO && !strcmp(bb->subtitle, "2008") && bb->width == 1920 &&
              bb->bitrate_kbps == 5000 && bb->fps == 24 && bb->view_offset_ms == 2530000 &&
              bb->part_size == 3500000000LL && !strcmp(bb->part_key, "/library/parts/11/101/file.mp4"),
              "big buck fields");
        CHECK(bb && caps_direct_ok(&k1080, bb, why, sizeof(why)), "BBB plays directly at 1080p: %s", why);
        CHECK(bb && !caps_direct_ok(&k720, bb, why, sizeof(why)) && strstr(why, "bigger"), "not at 720p: %s", why);
        it = find(&films, "Hevc Film");
        CHECK(it && !caps_direct_ok(&k1080, it, why, sizeof(why)) && strstr(why, "hevc"), "hevc: %s", why);
        it = find(&films, "Ten Bit");
        CHECK(it && it->bit_depth == 10 && !caps_direct_ok(&k1080, it, why, sizeof(why)) && strstr(why, "10"),
              "high 10: %s", why);
        it = find(&films, "Remux");
        CHECK(it && !caps_direct_ok(&k1080, it, why, sizeof(why)) && strstr(why, "Mbit"), "remux: %s", why);
        it = find(&films, "Sixty");
        CHECK(it && !caps_direct_ok(&k1080, it, why, sizeof(why)) && strstr(why, "60"), "1080p60: %s", why);
        it = find(&films, "Dvd Rip");
        CHECK(it && caps_direct_ok(&k1080, it, why, sizeof(why)) && it->fps == 25 && it->watched,
              "DVD MPEG-2 + AC-3 plays directly: %s", why);
    }

    /* ---- what Reel is given */
    {
        const plex_item *bb = find(&films, "Big Buck Bunny"), *hevc = find(&films, "Hevc Film");
        play_t p;
        source_t src[2];
        int hls;
        char *json;
        char want[256];
        CHECK(caps_play(&c, bb, &k1080, 1, 1, &p) == 0 && p.direct, "BBB direct");
        snprintf(want, sizeof(want), "%s/library/parts/11/101/file.mp4", base);
        CHECK(!strcmp(p.url, want) && !strstr(p.url, "Token"), "direct url %s (token in the headers only)", p.url);
        CHECK(!strcmp(p.key, "plex:MID/101"), "carry-on key %s", p.key);
        json = handoff_json(&p, "Big Buck Bunny \xe2\x80\x93 Director\xe2\x80\x99s cut", "Matinee/test");
        CHECK(json != NULL, "json");
        n = sources_parse(json, strlen(json), src, 2, &hls);
        CHECK(n == 1, "Reel reads one source from it, got %d", n);
        if (n == 1) {
            CHECK(!strcmp(src[0].url, want), "Reel's url %s", src[0].url);
            CHECK(src[0].headers && strstr(src[0].headers, "X-Plex-Token: SRV-TOKEN\r\n") &&
                  strstr(src[0].headers, "X-Plex-Client-Identifier: client-123\r\n") &&
                  !strstr(src[0].headers, "Accept:"), "Reel's headers: %s", src[0].headers);
            CHECK(src[0].user_agent && !strcmp(src[0].user_agent, "Matinee/test"), "Reel's user agent");
            CHECK(src[0].title && !strcmp(src[0].title, "Big Buck Bunny - Director's cut"),
                  "title in Latin-1: %s", src[0].title);
            CHECK(src[0].key && !strcmp(src[0].key, "plex:MID/101"), "Reel's key");
            source_free(&src[0]);
        }
        free(json);

        /* a converted stream */
        CHECK(caps_play(&c, hevc, &k1080, 1, 1, &p) == 0 && !p.direct, "hevc converted");
        CHECK(strstr(p.why, "hevc") != NULL, "why: %s", p.why);
        CHECK(!*p.key, "no carry-on key for a converted stream");
        {
            net_buf b;
            char err[256];
            CHECK(net_fetch(p.url, p.headers, NULL, &b, 5000, err, sizeof(err)) == 0 &&
                  strstr(b.data, "#EXTM3U"), "the stream's playlist: %s", err);
            net_buf_free(&b);
        }
        log = server_log();
        {
            const cJSON *r = last(log, "/video/:/transcode/universal/start.m3u8");
            const char *extra = r ? qv(r, "X-Plex-Client-Profile-Extra") : "";
            CHECK(r && !strcmp(qv(r, "path"), "/library/metadata/102"), "path %s", r ? qv(r, "path") : "");
            CHECK(r && !strcmp(qv(r, "protocol"), "hls") && !strcmp(qv(r, "videoResolution"), "1920x1080") &&
                  !strcmp(qv(r, "maxVideoBitrate"), "10000") && !strcmp(qv(r, "location"), "lan") &&
                  !strcmp(qv(r, "subtitles"), "burn") && !strcmp(qv(r, "offset"), "0"),
                  "transcode parameters");
            CHECK(r && !strcmp(qv(r, "X-Plex-Token"), "SRV-TOKEN") && !strcmp(hdr(r, "X-Plex-Token"), "SRV-TOKEN"),
                  "token in the address and the headers");
            CHECK(strstr(extra, "add-transcode-target(type=videoProfile&context=streaming&protocol=hls"
                                "&container=mpegts&videoCodec=h264&audioCodec=aac"),
                  "profile target: %s", extra);
            CHECK(strstr(extra, "name=video.height&value=1080") && strstr(extra, "name=video.bitrate&value=10000") &&
                  strstr(extra, "name=video.level&value=41") && strstr(extra, "name=video.bitDepth&value=8") &&
                  strstr(extra, "name=audio.channels&value=2"), "profile limits: %s", extra);
            CHECK(!strcmp(qv(r, "session"), qv(r, "X-Plex-Session-Identifier")) && strlen(qv(r, "session")) == 12,
                  "session id");
        }
        cJSON_Delete(log);
        /* resume: a converted stream starts at the view offset */
        CHECK(caps_play(&c, bb, &k720, 1, 1, &p) == 0 && !p.direct && p.offset_s == 2530 &&
              strstr(p.url, "&offset=2530&") && strstr(p.url, "videoResolution=1280x720"), "720p resume");
        CHECK(caps_play(&c, bb, &k1080, 0, 0, &p) == 0 && !p.direct && strstr(p.why, "off") &&
              strstr(p.url, "&offset=0&"), "direct play off, from the start: %s", p.why);
        CHECK(caps_play(&c, &top.v[1], &k1080, 1, 0, &p) != 0, "a library isn't playable");
    }

    /* ---- TV */
    CHECK(plex_list_get(&c, "/library/sections/2/all", &tv) == 0 && tv.n == 1, "tv: %s", c.err);
    if (tv.n == 1) {
        CHECK(tv.v[0].kind == PI_FOLDER && !strcmp(tv.v[0].subtitle, "3 seasons") && !tv.v[0].watched &&
              tv.v[0].unwatched == 10, "show: 10 of 12 episodes unwatched (%d)", tv.v[0].unwatched);
        CHECK(plex_list_get(&c, tv.v[0].key, &seasons) == 0 && seasons.n == 4 &&
              !strcmp(seasons.v[0].title, "All episodes"), "the show's children: All episodes first, as Plex: %s", c.err);
        plex_list_keep(&seasons, "season");
        CHECK(seasons.n == 3 &&
              !strcmp(seasons.title, "Space Show"), "seasons: %s", c.err);
        if (seasons.n == 3) {
            CHECK(seasons.v[0].watched && !seasons.v[0].unwatched, "Specials: all seen");
            CHECK(!strcmp(seasons.v[1].subtitle, "6 episodes") && !seasons.v[1].watched && seasons.v[1].unwatched == 4 &&
                  seasons.v[2].unwatched == 6, "season");
            CHECK(plex_list_get(&c, seasons.v[1].key, &eps) == 0 && eps.n == 6, "episodes");
            CHECK(eps.n == 6 && !strcmp(eps.v[2].subtitle, "S1 E3") && eps.v[2].kind == PI_VIDEO &&
                  !strcmp(eps.v[2].title, "Episode \xe2\x80\x98" "3\xe2\x80\x99"), "episode 3");
            CHECK(eps.n == 6 && eps.v[0].watched && !eps.v[2].watched && eps.v[2].show_thumb &&
                  !strcmp(eps.v[2].show_thumb, "/library/metadata/20/thumb/1"), "watched, and the show's poster");
            plex_list_free(&eps);
            plex_list_free(&seasons);
        }
    }
    CHECK(plex_list_get(&c, "/library/onDeck", &deck) == 0 && deck.n == 1, "on deck");
    if (deck.n == 1)
        CHECK(!strcmp(deck.v[0].title, "Space Show") && !strcmp(deck.v[0].subtitle, "S1 E3 Episode Three") &&
              !strcmp(deck.v[0].thumb, "/library/metadata/20/thumb/1") && deck.v[0].view_offset_ms == 600000,
              "continue watching shows the show's name and poster");

    /* ---- posters, marking, saving */
    {
        char *jpeg = NULL;
        size_t len = 0;
        CHECK(plex_poster(&c, films.v[0].thumb, 120, 180, &jpeg, &len) == 0 && len > 4 &&
              (unsigned char)jpeg[0] == 0xFF && (unsigned char)jpeg[1] == 0xD8, "poster");
        free(jpeg);
        CHECK(plex_mark(&c, &films.v[0], 1) == 0 && plex_mark(&c, &films.v[0], 0) == 0, "mark");
        CHECK(plex_rate(&c, &films.v[0], 7) == 0, "rate");
        {
            plex_list d;
            CHECK(plex_details(&c, &films.v[0], &d) == 0 && d.n == 1 && d.v[0].user_rating == 7, "your rating comes back");
            if (d.n)
                plex_list_free(&d);
            CHECK(plex_rate(&c, &films.v[0], -1) == 0, "the rating taken away");
        }
        log = server_log();
        {
            const cJSON *r = last(log, "/photo/:/transcode");
            CHECK(r && !strcmp(qv(r, "width"), "120") && !strcmp(qv(r, "height"), "180") &&
                  !strcmp(qv(r, "url"), "/library/metadata/101/thumb/1700000000"), "poster request");
            r = last(log, "/:/scrobble");
            CHECK(r && !strcmp(qv(r, "key"), "101") && !strcmp(qv(r, "identifier"), "com.plexapp.plugins.library"),
                  "scrobble");
            CHECK(last(log, "/:/unscrobble") != NULL, "unscrobble");
        }
        cJSON_Delete(log);
    }
    {
        char url[512], headers[1024], err[256];
        net_stream *s;
        unsigned char buf[65536];
        long long total = 0;
        unsigned sum = 0;
        int got;
        plex_part_url(&c, &films.v[0], url, sizeof(url));
        plex_headers(&c, c.token, headers, sizeof(headers));
        s = net_open(url, headers, 5000, err, sizeof(err));
        CHECK(s != NULL, "part opens: %s", err);
        if (s) {
            CHECK(net_size(s) == 5 * 1024 * 1024, "part size %lld", (long long)net_size(s));
            while ((got = net_read(s, buf, sizeof(buf))) > 0) {
                for (int i = 0; i < got; i++)
                    sum = sum * 31 + buf[i];
                total += got;
            }
            CHECK(got == 0 && total == 5 * 1024 * 1024, "whole part read: %lld", total);
            printf("part checksum %08x\n", sum);
            net_close(s);
        }
        c.token[0] ^= 1;
        plex_headers(&c, c.token, headers, sizeof(headers));
        s = net_open(url, headers, 5000, err, sizeof(err));
        CHECK(!s && strstr(err, "sign in"), "a bad token says to sign in again: %s", err);
        c.token[0] ^= 1;
    }
    /* ---- one video's details, and its subtitles */
    {
        plex_list d;
        play_t p;
        const plex_item *bb = find(&films, "Big Buck Bunny");
        CHECK(bb && !strcmp(bb->art, "/library/metadata/101/art/1700000000") && bb->year == 2008 &&
              bb->rating == 7.5 && !strcmp(bb->content_rating, "PG") && bb->summary &&
              bb->nsubs == 0, "details from the list (no subtitle tracks there)");
        CHECK(bb && plex_details(&c, bb, &d) == 0 && d.n == 1, "details: %s", c.err);
        if (d.n == 1) {
            const plex_item *it = &d.v[0];
            CHECK(it->part_id == 11101 && it->nsubs == 3, "part %ld, %d subtitle tracks", it->part_id, it->nsubs);
            CHECK(it->nsubs == 3 && !strcmp(it->subs[0].title, "English (SRT)") && !it->subs[0].external &&
                  it->subs[1].external && !strcmp(it->subs[1].key, "/library/streams/1002") &&
                  it->subs[2].forced && !strcmp(it->subs[2].codec, "pgs"), "the tracks");
            CHECK(plex_sub_selected(it) == -1, "none chosen");
            CHECK(caps_play(&c, it, &k1080, 1, 0, &p) == 0 && p.direct, "no subtitles: direct play");
            {
                plex_item ep;
                plex_list e;
                memset(&ep, 0, sizeof(ep));
                ep.rating_key = "211";
                CHECK(plex_details(&c, &ep, &e) == 0 && e.n == 1, "an episode's details: %s", c.err);
                if (e.n == 1) {
                    const plex_item *x = &e.v[0];
                    CHECK(x->nmarkers == 2 && x->markers[0].type == PM_INTRO && x->markers[0].start_ms == 2000 &&
                          x->markers[0].end_ms == 8000 && !x->markers[0].final && x->markers[1].type == PM_CREDITS &&
                          x->markers[1].final && x->nchapters == 0,
                          "its intro and credits (includeMarkers=1; the commercial left out)");
                    CHECK(plex_marker_at(x, 1999) == -1 && plex_marker_at(x, 2000) == 0 && plex_marker_at(x, 9500) == -1 &&
                          plex_marker_at(x, 19999) == 1 && plex_marker_at(x, 20000) == -1, "the marker at a time");
                    plex_list_free(&e);
                }
            }
            CHECK(plex_set_subtitle(&c, it, 1002) == 0, "choose subtitles: %s", c.err);
            plex_list_free(&d);
        }
        log = server_log();
        {
            const cJSON *r = last(log, "/library/parts/11101");
            CHECK(r && !strcmp(cJSON_GetObjectItem(r, "method")->valuestring, "PUT") &&
                  !strcmp(qv(r, "subtitleStreamID"), "1002") && !strcmp(qv(r, "allParts"), "1") &&
                  !strcmp(hdr(r, "X-Plex-Token"), "SRV-TOKEN"), "PUT /library/parts/11101?subtitleStreamID=1002");
        }
        cJSON_Delete(log);
        CHECK(plex_details(&c, bb, &d) == 0 && d.n == 1 && plex_sub_selected(&d.v[0]) == 1, "the choice kept");
        if (d.n == 1) {
            CHECK(caps_play(&c, &d.v[0], &k1080, 1, 0, &p) == 0 && !p.direct && strstr(p.url, "subtitles=burn") &&
                  strstr(p.why, "subtitles burnt in: English (SRT External)"), "chosen: converted, burnt in: %s", p.why);
            {
                caps_t own = k1080;
                own.own_subs = 1;
                CHECK(caps_play(&c, &d.v[0], &own, 1, 0, &p) == 0 && p.direct &&
                      strstr(p.why, "Direct Play: ") && strstr(p.why, "; subtitles: English (SRT External)"),
                      "the built-in player draws them: played directly: %s", p.why);
                free(d.v[0].subs[1].codec);
                d.v[0].subs[1].codec = strdup("vobsub");     /* a file of pictures beside it: can't */
                CHECK(caps_play(&c, &d.v[0], &own, 1, 0, &p) == 0 && !p.direct, "external VobSub: burnt in");
                d.v[0].subs[1].external = 0;                 /* VobSub in the file: can */
                CHECK(caps_play(&c, &d.v[0], &own, 1, 0, &p) == 0 && p.direct, "VobSub in the file: played directly");
                d.v[0].subs[1].external = 1;
            }
            CHECK(plex_set_subtitle(&c, &d.v[0], 0) == 0, "none again");
            plex_list_free(&d);
        }
        CHECK(plex_details(&c, bb, &d) == 0 && d.n == 1 && plex_sub_selected(&d.v[0]) == -1, "none chosen again");
        plex_list_free(&d);
    }

    /* ---- the rest of the metadata, the play queue and timeline, search */
    {
        plex_list d, r;
        plex_playing pl;
        const plex_item *bb = find(&films, "Big Buck Bunny");
        memset(&pl, 0, sizeof(pl));
        CHECK(bb && plex_details(&c, bb, &d) == 0 && d.n == 1, "details again");
        if (d.n == 1) {
            const plex_item *it = &d.v[0];
            CHECK(it->genres && !strcmp(it->genres, "Animation, Comedy, Short") &&
                  it->directors && !strcmp(it->directors, "Sacha Goedegebure") &&
                  it->writers && !strcmp(it->writers, "Sacha Goedegebure, Ton Roosendaal") &&
                  it->studio && !strcmp(it->studio, "Blender Foundation") &&
                  it->released && !strcmp(it->released, "2008-04-10") && it->audience_rating == 8.1 &&
                  it->country && !strcmp(it->country, "Netherlands"), "genres, people, studio, dates");
            CHECK(it->ncast == 4 && !strcmp(it->cast[1].name, "Frank") && !strcmp(it->cast[1].role, "Flying squirrel") &&
                  !strcmp(it->cast[1].thumb, "https://metadata-static.plex.tv/people/frank.jpg") && !it->cast[2].thumb,
                  "the cast, in order, with photos where there are");
            CHECK(plex_play_queue(&c, it, &pl) == 0 && pl.pq_id == 3141 && pl.pq_item_id == 59265 && pl.pq_version == 1,
                  "a play queue: %s", c.err);
            snprintf(pl.session, sizeof(pl.session), "sess42");
            CHECK(plex_timeline(&c, it, "playing", 61000, 5400000, &pl) == 0, "timeline");
            log = server_log();
            {
                const cJSON *q = last(log, "/playQueues"), *t = last(log, "/:/timeline");
                CHECK(q && !strcmp(cJSON_GetObjectItem(q, "method")->valuestring, "POST") &&
                      !strcmp(qv(q, "uri"), "server://MID/com.plexapp.plugins.library/library/metadata/101") &&
                      !strcmp(qv(q, "type"), "video"), "POST /playQueues for the video");
                CHECK(t && !strcmp(qv(t, "state"), "playing") && !strcmp(qv(t, "time"), "61000") &&
                      !strcmp(qv(t, "ratingKey"), "101") && !strcmp(qv(t, "key"), "/library/metadata/101") &&
                      !strcmp(qv(t, "containerKey"), "/playQueues/3141") && !strcmp(qv(t, "playQueueItemID"), "59265") &&
                      !strcmp(qv(t, "guid"), "plex://movie/5d776825880197001ec967c6") &&
                      !strcmp(hdr(t, "X-Plex-Session-Identifier"), "sess42") &&
                      !strcmp(hdr(t, "X-Plex-Provides"), "player") && !strcmp(hdr(t, "X-Plex-Client-Identifier"), c.client_id),
                      "the timeline: the play queue, the session, the player's headers");
            }
            cJSON_Delete(log);
            plex_list_free(&d);
        }
        CHECK(plex_search(&c, "bunny", &r) == 0 && r.n == 1 && !strcmp(r.v[0].title, "Big Buck Bunny") &&
              !strcmp(r.v[0].subtitle, "Film \xc2\xb7 2008"), "search: a film (%d)", r.n);
        plex_list_free(&r);
        CHECK(plex_search(&c, "space", &r) == 0 && r.n == 7 && r.v[0].kind == PI_FOLDER &&
              !strcmp(r.v[0].subtitle, "Show \xc2\xb7 2 seasons") && r.v[1].kind == PI_VIDEO &&
              !strcmp(r.v[1].title, "Space Show") && strstr(r.v[1].subtitle, "S1 E1"),
              "search: the show first, then its episodes (under the show's name)");
        plex_list_free(&r);
        CHECK(plex_search(&c, "", &r) == 0 && r.n == 0, "an empty search: nothing, and nothing asked");
        plex_list_free(&r);
        CHECK(plex_list_get(&c, "search:bunny", &r) == 0 && r.n == 1, "a search as a list");
        plex_list_free(&r);
    }

    plex_list_free(&top);
    plex_list_free(&films);
    plex_list_free(&tv);
    plex_list_free(&deck);
    printf("core_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
