/*
 * jellyfin.c - Jellyfin servers, through the same calls as Plex's (see
 * jellyfin.h).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "jellyfin.h"
#include "plex_int.h"
#include "net.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TICKS_MS 10000LL        /* Jellyfin's ticks: 10,000,000 a second */

/* What a list's items are asked for with (Overview too for the home page's rows) */
#define LIST_FIELDS "ChildCount,RecursiveItemCount,DateCreated"
#define ROW_FIELDS  LIST_FIELDS ",Overview,Genres,OfficialRating"
#define IMAGES      "&EnableImageTypes=Primary,Backdrop&ImageTypeLimit=1&EnableUserData=true"

/* ---- requests -------------------------------------------------------------- */

void jf_headers(const plex_ctx *c, const char *token, char *out, size_t size)
{
    char prod[96], dev[200], id[150], ver[48], tok[300];
    int n;
    /* the values are URL-decoded by the server: escaped, so a quote or a
       comma in a computer's name can't break the header */
    net_escape(c->product, prod, sizeof(prod));
    net_escape(c->device_name, dev, sizeof(dev));
    net_escape(c->client_id, id, sizeof(id));
    net_escape(c->version, ver, sizeof(ver));
    n = snprintf(out, size,
                 "Accept: application/json\r\n"
                 "Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"",
                 prod, dev, id, ver);
    if (token && *token && n > 0 && (size_t)n < size) {
        net_escape(token, tok, sizeof(tok));
        n += snprintf(out + n, size - n, ", Token=\"%s\"", tok);
    }
    if (n > 0 && (size_t)n + 3 < size)
        snprintf(out + n, size - n, "\r\n");
}

/* GET (body NULL) or POST a path on the server; the answer parsed */
static cJSON *jcall(plex_ctx *c, const char *path, const char *body, int *status)
{
    char url[2048];
    snprintf(url, sizeof(url), "%s%s", c->base, path);
    return px_get_json(c, url, c->token, body, API_TIMEOUT, status);
}

/* A request whose answer isn't wanted (Jellyfin answers most with 204) */
static int jsend(plex_ctx *c, const char *method, const char *path, const char *body, int timeout)
{
    char url[2048], headers[1024];
    net_buf b;
    snprintf(url, sizeof(url), "%s%s", c->base, path);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_send(url, headers, method, body, &b, timeout, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

static int signed_in(plex_ctx *c)
{
    if (!*c->base) {
        px_err(c, "no server chosen%s", NULL);
        return 0;
    }
    if (!*c->user_id || !*c->token) {
        px_err(c, "not signed in to %s", c->server_name);
        return 0;
    }
    return 1;
}

/* ---- signing in -------------------------------------------------------------- */

int jf_connect(plex_ctx *c, const char *addr)
{
    plex_ctx t = *c;
    char b[256];
    const char *host;
    size_t n;
    int status = 0;
    cJSON *j;
    char a[256];
    snprintf(a, sizeof(a), "%s", addr);
    a[strcspn(a, "\r\n")] = 0;
    if (strstr(a, "://")) {
        snprintf(b, sizeof(b), "%s", a);
    } else {
        /* just the host: http, and Jellyfin's own port unless one is given */
        size_t h = strcspn(a, "/");
        host = a;
        if (memchr(host, ':', h))
            snprintf(b, sizeof(b), "http://%s", a);
        else
            snprintf(b, sizeof(b), "http://%.*s:%d%s", (int)h, a, JF_PORT, a + h);
    }
    n = strlen(b);
    while (n && b[n - 1] == '/')
        b[--n] = 0;
    t.kind = SRV_JELLYFIN;
    snprintf(t.base, sizeof(t.base), "%s", b);
    t.token[0] = t.user_id[0] = t.user_name[0] = 0;
    if (!(j = jcall(&t, "/System/Info/Public", NULL, &status))) {
        snprintf(c->err, sizeof(c->err), "%s", status == 404 ? "that isn't a Jellyfin server" : t.err);
        return status == 404 ? -2 : -1;
    }
    if (!px_jstr(j, "Id") || !*px_jstr(j, "Id")) {
        cJSON_Delete(j);
        snprintf(c->err, sizeof(c->err), "%s doesn't answer as a Jellyfin server does", b);
        return -2;
    }
    snprintf(t.server_id, sizeof(t.server_id), "%s", px_jstr(j, "Id"));
    snprintf(t.server_name, sizeof(t.server_name), "%s",
             px_jstr(j, "ServerName") && *px_jstr(j, "ServerName") ? px_jstr(j, "ServerName") : "Jellyfin server");
    cJSON_Delete(j);
    t.local = 1;
    t.err[0] = 0;
    *c = t;
    return 0;
}

/* After either way of signing in: the token and who it is */
static int signed_in_as(plex_ctx *c, cJSON *j)
{
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "User");
    const char *tok = px_jstr(j, "AccessToken"), *id = px_jstr(u, "Id");
    if (!tok || !*tok || !id || !*id) {
        px_err(c, "the server didn't sign you in%s", NULL);
        return -1;
    }
    snprintf(c->token, sizeof(c->token), "%s", tok);
    snprintf(c->user_id, sizeof(c->user_id), "%s", id);
    snprintf(c->user_name, sizeof(c->user_name), "%s", px_jstr(u, "Name") ? px_jstr(u, "Name") : "");
    return 0;
}

int jf_qc_start(plex_ctx *c, char *secret, size_t slen, char *code, size_t clen)
{
    int status = 0;
    cJSON *j;
    /* POST since 10.9; a GET before */
    j = jcall(c, "/QuickConnect/Initiate", "{}", &status);
    if (!j && (status == 404 || status == 499))
        j = jcall(c, "/QuickConnect/Initiate", NULL, &status);
    if (!j) {
        if (status == 401 || status == 403) {
            px_err(c, "Quick Connect is turned off on this server%s", NULL);
            return -2;
        }
        return -1;
    }
    snprintf(secret, slen, "%s", px_jstr(j, "Secret") ? px_jstr(j, "Secret") : "");
    snprintf(code, clen, "%s", px_jstr(j, "Code") ? px_jstr(j, "Code") : "");
    cJSON_Delete(j);
    if (!*secret || !*code) {
        px_err(c, "the server gave no Quick Connect code%s", NULL);
        return -1;
    }
    return 0;
}

int jf_qc_check(plex_ctx *c, const char *secret)
{
    char path[300], esc[200], body[300];
    int status = 0, e;
    cJSON *j, *b;
    net_escape(secret, esc, sizeof(esc));
    snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", esc);
    if (!(j = jcall(c, path, NULL, &status)))
        return status == 404 || status == 401 ? -1 : 0;     /* gone; else try again */
    e = px_jbool(j, "Authenticated");
    cJSON_Delete(j);
    if (!e)
        return 0;
    b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "Secret", secret);
    if (!cJSON_PrintPreallocated(b, body, sizeof(body), 0))
        body[0] = 0;
    cJSON_Delete(b);
    if (!(j = jcall(c, "/Users/AuthenticateWithQuickConnect", body, &status)))
        return -1;
    e = signed_in_as(c, j);
    cJSON_Delete(j);
    return e == 0 ? 1 : -1;
}

int jf_login(plex_ctx *c, const char *user, const char *password)
{
    cJSON *b = cJSON_CreateObject(), *j;
    char *body;
    int status = 0, e;
    cJSON_AddStringToObject(b, "Username", user);
    cJSON_AddStringToObject(b, "Pw", password ? password : "");
    body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body)
        return -1;
    c->token[0] = 0;
    j = jcall(c, "/Users/AuthenticateByName", body, &status);
    free(body);
    if (!j) {
        if (status == 401 || status == 400) {
            px_err(c, "the server didn't take that name and password%s", NULL);
            return -2;
        }
        return -1;
    }
    e = signed_in_as(c, j);
    cJSON_Delete(j);
    return e;
}

int jf_logout(plex_ctx *c)
{
    if (!*c->token)
        return 0;
    return jsend(c, "POST", "/Sessions/Logout", "{}", PROBE_TIMEOUT);
}

/* ---- what the front end chose: subtitles and sound, while Matinee runs ------------ */

#define PREFS 64
static struct {
    char id[48];
    int sub, aud;               /* Jellyfin's stream Index; sub -1 none; -2 not chosen */
} prefs[PREFS];
static int prefs_next;

static int pref_find(const char *id, int make)
{
    for (int i = 0; i < PREFS; i++)
        if (*prefs[i].id && !strcmp(prefs[i].id, id))
            return i;
    if (!make)
        return -1;
    {
        int i = prefs_next;
        prefs_next = (prefs_next + 1) % PREFS;
        snprintf(prefs[i].id, sizeof(prefs[i].id), "%s", id);
        prefs[i].sub = prefs[i].aud = -2;
        return i;
    }
}

int jf_set_subtitle(plex_ctx *c, const plex_item *it, long stream_id)
{
    int p;
    (void)c;
    if (!it->rating_key)
        return -1;
    p = pref_find(it->rating_key, 1);
    prefs[p].sub = stream_id > 0 ? (int)stream_id - 1 : -1;     /* ids are Index + 1: 0 is "none" */
    return 0;
}

int jf_set_audio(plex_ctx *c, const plex_item *it, long stream_id)
{
    int p;
    (void)c;
    if (!it->rating_key || stream_id <= 0)
        return -1;
    p = pref_find(it->rating_key, 1);
    prefs[p].aud = (int)stream_id - 1;
    return 0;
}

/* ---- the libraries -------------------------------------------------------------- */

#define SECS 32
static struct {
    char id[48];
    char ctype[24];             /* movies, tvshows, homevideos, music... ("" mixed) */
    char name[96];
} secs[SECS];
static int nsecs;
static char secs_of[96];        /* the server and user they're for */

static int views_get(plex_ctx *c, plex_list *out);

/* Section n (1...) of the libraries, fetching them if they aren't known */
static int section(plex_ctx *c, int n)
{
    char who[96];
    snprintf(who, sizeof(who), "%s/%s", c->server_id, c->user_id);
    if (strcmp(who, secs_of) || n < 1 || n > nsecs) {
        plex_list l;
        if (views_get(c, &l) != 0)
            return -1;
        plex_list_free(&l);
    }
    if (n < 1 || n > nsecs) {
        px_err(c, "the server has no library %s", NULL);
        return -1;
    }
    return n - 1;
}

/* ---- items ------------------------------------------------------------------------ */

static char *join_arr(const cJSON *a, const char *field, int max)
{
    const cJSON *e;
    char out[400] = "";
    size_t n = 0;
    int k = 0;
    cJSON_ArrayForEach(e, a) {
        const char *s = field ? px_jstr(e, field) : cJSON_IsString(e) ? e->valuestring : NULL;
        if (!s || !*s || k >= max)
            continue;
        n += snprintf(out + n, n < sizeof(out) ? sizeof(out) - n : 0, "%s%s", k ? ", " : "", s);
        k++;
        if (n >= sizeof(out))
            break;
    }
    return k ? px_dup(out) : NULL;
}

static char *image(const char *id, const char *what, const char *tag)
{
    char t[256];
    if (!id || !*id || !tag || !*tag)
        return NULL;
    snprintf(t, sizeof(t), "/Items/%s/Images/%s?tag=%s", id, what, tag);
    return px_dup(t);
}

static const char *first_str(const cJSON *a)
{
    const cJSON *e = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, 0) : NULL;
    return cJSON_IsString(e) ? e->valuestring : NULL;
}

static char *lower(const char *s)
{
    char *d = px_dup(s);
    for (char *p = d; p && *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return d;
}

/* Jellyfin's codec names as the ones caps.c knows (FFmpeg's, mostly) */
static char *codec(const char *s)
{
    char *d = lower(s);
    if (d && !strncmp(d, "pcm_", 4))
        d[3] = 0;
    return d;
}

/* The file (the first MediaSource): its container, streams, and the
   subtitle and sound tracks as plex.c makes them (an id is Jellyfin's
   Index + 1, so 0 stays "none") */
static void source(plex_item *it, const cJSON *m)
{
    const cJSON *srcs = cJSON_GetObjectItemCaseSensitive(m, "MediaSources");
    const cJSON *s0 = cJSON_IsArray(srcs) ? cJSON_GetArrayItem(srcs, 0) : NULL, *st, *streams;
    const char *msid, *ct, *path;
    int def_a, def_s, p, nsub = 0, naud = 0, a_found = 0;
    char t[400];
    if (!s0)
        return;
    msid = px_jstr(s0, "Id");
    it->source_id = px_dup(msid);
    ct = px_jstr(s0, "Container");
    if (ct) {                       /* "mov,mp4,m4a,..." */
        snprintf(t, sizeof(t), "%s", ct);
        t[strcspn(t, ",")] = 0;
        it->container = lower(t);
    }
    it->bitrate_kbps = (int)(px_jnum(s0, "Bitrate", 0) / 1000);
    it->part_size = (int64_t)px_jnum(s0, "Size", 0);
    path = px_jstr(s0, "Path");
    it->part_file = px_dup(path);
    if (msid && it->rating_key) {
        char e[120];
        net_escape(msid, e, sizeof(e));
        snprintf(t, sizeof(t), "/Videos/%s/stream?static=true&mediaSourceId=%s", it->rating_key, e);
        it->part_key = px_dup(t);
    }
    def_a = (int)px_jnum(s0, "DefaultAudioStreamIndex", -1);
    def_s = cJSON_GetObjectItemCaseSensitive(s0, "DefaultSubtitleStreamIndex") ?
            (int)px_jnum(s0, "DefaultSubtitleStreamIndex", -1) : -1;
    p = it->rating_key ? pref_find(it->rating_key, 0) : -1;
    if (p >= 0 && prefs[p].sub != -2)
        def_s = prefs[p].sub;
    if (p >= 0 && prefs[p].aud != -2)
        def_a = prefs[p].aud;
    streams = cJSON_GetObjectItemCaseSensitive(s0, "MediaStreams");
    cJSON_ArrayForEach(st, streams) {
        const char *ty = px_jstr(st, "Type");
        if (ty && !strcmp(ty, "Subtitle"))
            nsub++;
        else if (ty && !strcmp(ty, "Audio"))
            naud++;
    }
    if (nsub)
        it->subs = calloc(nsub, sizeof(plex_sub));
    if (naud)
        it->auds = calloc(naud, sizeof(plex_audio));
    cJSON_ArrayForEach(st, streams) {
        const char *ty = px_jstr(st, "Type"), *dt = px_jstr(st, "DisplayTitle");
        int idx = (int)px_jnum(st, "Index", 0);
        if (!ty)
            continue;
        if (!strcmp(ty, "Video") && !it->vcodec) {
            int bd = (int)px_jnum(st, "BitDepth", 8);
            const char *pr = px_jstr(st, "Profile");
            it->vcodec = codec(px_jstr(st, "Codec"));
            it->width = (int)px_jnum(st, "Width", 0);
            it->height = (int)px_jnum(st, "Height", 0);
            it->fps = px_jnum(st, "RealFrameRate", px_jnum(st, "AverageFrameRate", 0));
            it->bit_depth = bd > 8 ? bd : 8;
            if (pr) {
                snprintf(t, sizeof(t), "%s%s", pr, bd > 8 && !strstr(pr, "10") ? " 10" : "");
                it->vprofile = px_dup(t);
            } else if (bd > 8) {
                it->vprofile = px_dup("10 bit");
            }
            if (!it->bitrate_kbps)
                it->bitrate_kbps = (int)(px_jnum(st, "BitRate", 0) / 1000);
        } else if (!strcmp(ty, "Audio") && it->auds) {
            plex_audio *a = &it->auds[it->nauds++];
            a->id = idx + 1;
            a->codec = codec(px_jstr(st, "Codec"));
            a->language = px_dup(px_jstr(st, "Language"));
            a->title = px_dup(dt ? dt : a->language ? a->language : "Sound");
            a->selected = idx == def_a;
            if (a->selected || !a_found) {   /* the one played: the chosen, else the first */
                if (a->selected || !it->acodec) {
                    free(it->acodec);
                    it->acodec = px_dup(a->codec);
                    it->channels = (int)px_jnum(st, "Channels", 0);
                }
                a_found |= a->selected;
            }
        } else if (!strcmp(ty, "Subtitle") && it->subs) {
            plex_sub *sb = &it->subs[it->nsubs++];
            sb->id = idx + 1;
            sb->codec = codec(px_jstr(st, "Codec"));
            sb->language = px_dup(px_jstr(st, "Language"));
            sb->title = px_dup(dt ? dt : sb->language ? sb->language : "Subtitles");
            sb->forced = px_jbool(st, "IsForced");
            sb->external = px_jbool(st, "IsExternal");
            sb->selected = idx == def_s;
            if (sb->external && msid && it->rating_key) {
                /* the server sends it as SubRip (or ASS as it is) */
                int ass = sb->codec && (!strcmp(sb->codec, "ass") || !strcmp(sb->codec, "ssa"));
                snprintf(t, sizeof(t), "/Videos/%s/%s/Subtitles/%d/0/Stream.%s", it->rating_key, msid, idx,
                         ass ? "ass" : "srt");
                sb->key = px_dup(t);
                if (!ass) {             /* what the player is given */
                    free(sb->codec);
                    sb->codec = px_dup("srt");
                }
            }
        }
    }
}

#define CAST_MAX     12
#define CHAPTERS_MAX 99

static void people(plex_item *it, const cJSON *m)
{
    const cJSON *p, *a = cJSON_GetObjectItemCaseSensitive(m, "People");
    char dir[300] = "", wr[300] = "";
    int nd = 0, nw = 0, n = 0;
    cJSON_ArrayForEach(p, a) {
        const char *ty = px_jstr(p, "Type");
        n += ty && (!strcmp(ty, "Actor") || !strcmp(ty, "GuestStar"));
    }
    if (n > CAST_MAX)
        n = CAST_MAX;
    if (n)
        it->cast = calloc(n, sizeof(plex_person));
    cJSON_ArrayForEach(p, a) {
        const char *ty = px_jstr(p, "Type"), *name = px_jstr(p, "Name");
        if (!ty || !name)
            continue;
        if ((!strcmp(ty, "Actor") || !strcmp(ty, "GuestStar")) && it->cast && it->ncast < n) {
            plex_person *q = &it->cast[it->ncast++];
            q->name = px_dup(name);
            q->role = px_jstr(p, "Role") && *px_jstr(p, "Role") ? px_dup(px_jstr(p, "Role")) : NULL;
            q->thumb = image(px_jstr(p, "Id"), "Primary", px_jstr(p, "PrimaryImageTag"));
        } else if (!strcmp(ty, "Director") && nd < 3) {
            snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "%s%s", nd++ ? ", " : "", name);
        } else if (!strcmp(ty, "Writer") && nw < 3) {
            snprintf(wr + strlen(wr), sizeof(wr) - strlen(wr), "%s%s", nw++ ? ", " : "", name);
        }
    }
    if (nd)
        it->directors = px_dup(dir);
    if (nw)
        it->writers = px_dup(wr);
}

static void chapters(plex_item *it, const cJSON *m)
{
    const cJSON *e, *a = cJSON_GetObjectItemCaseSensitive(m, "Chapters");
    int n = cJSON_GetArraySize(a);
    if (n <= 0 || !(it->chapters = calloc(n < CHAPTERS_MAX ? n : CHAPTERS_MAX, sizeof(plex_chapter))))
        return;
    cJSON_ArrayForEach(e, a) {
        plex_chapter *ch;
        char t[40];
        if (it->nchapters >= CHAPTERS_MAX)
            break;
        ch = &it->chapters[it->nchapters++];
        ch->start_ms = (int64_t)(px_jnum(e, "StartPositionTicks", 0) / TICKS_MS);
        snprintf(t, sizeof(t), "Chapter %d", it->nchapters);
        ch->title = px_dup(px_jstr(e, "Name") && *px_jstr(e, "Name") ? px_jstr(e, "Name") : t);
    }
    for (int i = 0; i < it->nchapters; i++)
        it->chapters[i].end_ms = i + 1 < it->nchapters ? it->chapters[i + 1].start_ms : it->duration_ms;
}

/* One of Jellyfin's items (a BaseItemDto) as plex.c's. on_deck: an episode
   under its show's name and poster (Continue watching, recently added) */
static void add_item(plex_list *l, int *cap, const cJSON *m, int on_deck)
{
    plex_item *it;
    const char *ty = px_jstr(m, "Type"), *id = px_jstr(m, "Id"), *loc = px_jstr(m, "LocationType");
    const char *sid = px_jstr(m, "SeriesId"), *mt = px_jstr(m, "MediaType");
    const cJSON *ud = cJSON_GetObjectItemCaseSensitive(m, "UserData");
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(m, "ImageTags");
    char sub[160] = "", key[200];
    if (!id || !ty)
        return;
    if (!strcmp(ty, "Episode") && loc && !strcmp(loc, "Virtual"))
        return;                     /* a missing episode: known of, not there */
    if (px_grow(l, cap))
        return;
    it = &l->v[l->n++];
    memset(it, 0, sizeof(*it));
    it->rating_key = px_dup(id);
    it->title = px_dup(px_jstr(m, "Name") ? px_jstr(m, "Name") : "");
    it->duration_ms = (int64_t)(px_jnum(m, "RunTimeTicks", 0) / TICKS_MS);
    it->view_offset_ms = (int64_t)(px_jnum(ud, "PlaybackPositionTicks", 0) / TICKS_MS);
    it->watched = px_jbool(ud, "Played");
    it->summary = px_dup(px_jstr(m, "Overview"));
    it->content_rating = px_dup(px_jstr(m, "OfficialRating"));
    it->tagline = px_dup(first_str(cJSON_GetObjectItemCaseSensitive(m, "Taglines")));
    it->year = (int)px_jnum(m, "ProductionYear", 0);
    it->index = (int)px_jnum(m, "IndexNumber", 0);
    it->parent_index = (int)px_jnum(m, "ParentIndexNumber", 0);
    it->genres = join_arr(cJSON_GetObjectItemCaseSensitive(m, "Genres"), NULL, 4);
    it->studio = join_arr(cJSON_GetObjectItemCaseSensitive(m, "Studios"), "Name", 2);
    it->country = join_arr(cJSON_GetObjectItemCaseSensitive(m, "ProductionLocations"), NULL, 2);
    if (px_jstr(m, "PremiereDate")) {
        char d[16];
        snprintf(d, sizeof(d), "%.10s", px_jstr(m, "PremiereDate"));
        it->released = px_dup(d);
    }
    it->rating = px_jnum(m, "CommunityRating", 0);
    it->audience_rating = px_jnum(m, "CriticRating", 0) / 10;
    it->thumb = image(id, "Primary", px_jstr(tags, "Primary"));
    it->art = image(id, "Backdrop/0", first_str(cJSON_GetObjectItemCaseSensitive(m, "BackdropImageTags")));
    if (!it->art)
        it->art = image(px_jstr(m, "ParentBackdropItemId"), "Backdrop/0",
                        first_str(cJSON_GetObjectItemCaseSensitive(m, "ParentBackdropImageTags")));
    if (sid) {
        it->grandparent_key = px_dup(sid);
        it->grandparent_title = px_dup(px_jstr(m, "SeriesName"));
        it->show_thumb = image(sid, "Primary", px_jstr(m, "SeriesPrimaryImageTag"));
    }
    people(it, m);

    if (!strcmp(ty, "Movie")) {
        it->type = px_dup("movie");
        it->kind = PI_VIDEO;
    } else if (!strcmp(ty, "Episode")) {
        it->type = px_dup("episode");
        it->kind = PI_VIDEO;
    } else if (!strcmp(ty, "Video") || !strcmp(ty, "Trailer") || !strcmp(ty, "MusicVideo")) {
        it->type = px_dup("clip");
        it->kind = PI_VIDEO;
    } else if (!strcmp(ty, "Series")) {
        it->type = px_dup("show");
        it->kind = PI_FOLDER;
    } else if (!strcmp(ty, "Season")) {
        it->type = px_dup("season");
        it->kind = PI_FOLDER;
    } else if (!strcmp(ty, "BoxSet") || !strcmp(ty, "Folder") || !strcmp(ty, "CollectionFolder")) {
        it->type = px_dup("collection");
        it->kind = PI_FOLDER;
    } else if (!strcmp(ty, "Playlist")) {
        it->type = px_dup("playlist");
        it->kind = mt && strcmp(mt, "Video") ? PI_OTHER : PI_FOLDER;    /* music playlists: shown only */
    } else {
        it->type = lower(ty);
        it->kind = PI_OTHER;
    }
    /* what opening it lists: a show's series; a series' episodes (of its
       show); a collection's or a playlist's items, in their order */
    if (!strcmp(ty, "Series"))
        snprintf(key, sizeof(key), "/library/metadata/%s/children", id);
    else if (!strcmp(ty, "Season"))
        snprintf(key, sizeof(key), "/library/metadata/%s/children?series=%s", id, sid ? sid : "");
    else if (!strcmp(ty, "Playlist"))
        snprintf(key, sizeof(key), "/library/metadata/%s/children?parent=list", id);
    else if (!strcmp(ty, "BoxSet"))
        snprintf(key, sizeof(key), "/library/metadata/%s/children?parent=box", id);
    else if (it->kind == PI_FOLDER)
        snprintf(key, sizeof(key), "/library/metadata/%s/children?parent=folder", id);
    else
        snprintf(key, sizeof(key), "/library/metadata/%s", id);
    it->key = px_dup(key);

    if (!strcmp(it->type, "movie") || !strcmp(it->type, "clip")) {
        if (it->year > 0)
            snprintf(sub, sizeof(sub), "%d", it->year);
    } else if (!strcmp(it->type, "episode")) {
        int s = it->parent_index, e = it->index;
        if (on_deck && it->grandparent_title) {
            snprintf(sub, sizeof(sub), "S%d E%d %s", s, e, it->title);
            free(it->title);
            it->title = px_dup(it->grandparent_title);
            if (it->show_thumb) {
                free(it->thumb);
                it->thumb = px_dup(it->show_thumb);
            }
        } else if (s)
            snprintf(sub, sizeof(sub), "S%d E%d", s, e);
        else
            snprintf(sub, sizeof(sub), "Episode %d", e);
        if (!it->thumb && it->show_thumb)
            it->thumb = px_dup(it->show_thumb);
    } else if (!strcmp(it->type, "show")) {
        int n = (int)px_jnum(m, "ChildCount", 0);
        if (n)
            snprintf(sub, sizeof(sub), "%d season%s", n, n == 1 ? "" : "s");
        it->unwatched = (int)px_jnum(ud, "UnplayedItemCount", 0);
    } else if (!strcmp(it->type, "season")) {
        int n = (int)px_jnum(m, "ChildCount", px_jnum(m, "RecursiveItemCount", 0));
        if (on_deck && it->grandparent_title) {
            snprintf(sub, sizeof(sub), "%s", it->title);
            free(it->title);
            it->title = px_dup(it->grandparent_title);
        } else if (n)
            snprintf(sub, sizeof(sub), "%d episode%s", n, n == 1 ? "" : "s");
        it->unwatched = (int)px_jnum(ud, "UnplayedItemCount", 0);
        if (!it->thumb && it->show_thumb)
            it->thumb = px_dup(it->show_thumb);
    } else if (!strcmp(it->type, "collection") || !strcmp(it->type, "playlist")) {
        int n = (int)px_jnum(m, "ChildCount", px_jnum(m, "RecursiveItemCount", 0));
        if (n)
            snprintf(sub, sizeof(sub), "%d item%s", n, n == 1 ? "" : "s");
    }
    if (*sub)
        it->subtitle = px_dup(sub);
    if (it->kind == PI_VIDEO) {
        source(it, m);
        chapters(it, m);
    }
}

/* A list's items: {"Items": [...]}, [...] (Latest, SpecialFeatures) or
   one item (an item's details) */
static void add_items(plex_list *out, int *cap, const cJSON *j, int on_deck)
{
    const cJSON *e, *items = cJSON_IsArray(j) ? j : cJSON_GetObjectItemCaseSensitive(j, "Items");
    if (cJSON_IsArray(items)) {
        cJSON_ArrayForEach(e, items)
            add_item(out, cap, e, on_deck);
        out->total = (int)px_jnum(j, "TotalRecordCount", out->n);
    } else if (cJSON_IsObject(j) && px_jstr(j, "Id")) {
        add_item(out, cap, j, on_deck);
        out->total = out->n;
    }
    if (out->total < out->n)
        out->total = out->n;
}

static int get_items(plex_ctx *c, const char *path, int on_deck, plex_list *out)
{
    cJSON *j;
    int cap = 0;
    memset(out, 0, sizeof(*out));
    if (!(j = jcall(c, path, NULL, NULL)))
        return -1;
    add_items(out, &cap, j, on_deck);
    cJSON_Delete(j);
    return 0;
}

/* The libraries, as Plex's /library/sections: section n's key is
   /library/sections/<n>/all */
static int views_get(plex_ctx *c, plex_list *out)
{
    char path[160];
    cJSON *j, *e;
    int cap = 0;
    memset(out, 0, sizeof(*out));
    snprintf(path, sizeof(path), "/Users/%s/Views", c->user_id);
    if (!(j = jcall(c, path, NULL, NULL)))
        return -1;
    nsecs = 0;
    snprintf(secs_of, sizeof(secs_of), "%s/%s", c->server_id, c->user_id);
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "Items")) {
        const char *id = px_jstr(e, "Id"), *ct = px_jstr(e, "CollectionType");
        plex_item *it;
        char k[64];
        if (!id || nsecs >= SECS)
            continue;
        if (!ct)
            ct = "";
        if (!strcmp(ct, "boxsets") || !strcmp(ct, "playlists"))
            continue;               /* each library's bar has its collections and the playlists */
        snprintf(secs[nsecs].id, sizeof(secs[0].id), "%s", id);
        snprintf(secs[nsecs].ctype, sizeof(secs[0].ctype), "%s", ct);
        snprintf(secs[nsecs].name, sizeof(secs[0].name), "%s", px_jstr(e, "Name") ? px_jstr(e, "Name") : "");
        nsecs++;
        if (px_grow(out, &cap))
            break;
        it = &out->v[out->n++];
        memset(it, 0, sizeof(*it));
        it->title = px_dup(secs[nsecs - 1].name);
        it->rating_key = px_dup(id);
        it->thumb = image(id, "Primary", px_jstr(cJSON_GetObjectItemCaseSensitive(e, "ImageTags"), "Primary"));
        snprintf(k, sizeof(k), "/library/sections/%d/all", nsecs);
        it->key = px_dup(k);
        if (!strcmp(ct, "tvshows")) {
            it->type = px_dup("show");
            it->subtitle = px_dup("TV");
            it->kind = PI_FOLDER;
        } else if (!strcmp(ct, "movies") || !strcmp(ct, "homevideos") || !strcmp(ct, "musicvideos") || !*ct) {
            /* films, and the libraries of videos in folders */
            it->type = px_dup("movie");
            it->subtitle = px_dup(!strcmp(ct, "movies") ? "Films" : "Videos");
            it->kind = PI_FOLDER;
        } else {
            it->type = px_dup(!strcmp(ct, "music") ? "artist" : !strcmp(ct, "photos") ? "photo" : ct);
            it->subtitle = px_dup(!strcmp(ct, "music") ? "Music" : !strcmp(ct, "photos") ? "Photos" : NULL);
            it->kind = PI_OTHER;    /* shown, not handled */
        }
    }
    out->total = out->n;
    cJSON_Delete(j);
    return 0;
}

/* Plex's sorts (the library bar's) as Jellyfin's */
static const char *sort_by(const char *q)
{
    if (q && strstr(q, "sort=addedAt:desc"))
        return "&SortBy=DateCreated,SortName&SortOrder=Descending";
    if (q && strstr(q, "sort=year:desc"))
        return "&SortBy=ProductionYear,PremiereDate,SortName&SortOrder=Descending";
    if (q && strstr(q, "sort=rating:desc"))
        return "&SortBy=CommunityRating,SortName&SortOrder=Descending";
    return "&SortBy=SortName&SortOrder=Ascending";
}

/* Continue watching: what's part watched, then the next episode of each
   show being watched (Plex's On Deck is both) */
static int on_deck(plex_ctx *c, int size, plex_list *out)
{
    char path[400];
    plex_list nx;
    int cap;
    snprintf(path, sizeof(path), "/Users/%s/Items/Resume?MediaTypes=Video&Limit=%d&Fields=" ROW_FIELDS IMAGES,
             c->user_id, size);
    if (get_items(c, path, 1, out) != 0)
        return -1;
    snprintf(path, sizeof(path), "/Shows/NextUp?UserId=%s&Limit=%d&Fields=" ROW_FIELDS IMAGES, c->user_id, size);
    if (get_items(c, path, 1, &nx) != 0)
        return 0;                   /* the part watched, at least */
    cap = out->n;
    for (int i = 0; i < nx.n; i++) {
        int dup = out->n >= size;
        for (int k = 0; !dup && k < out->n; k++)
            dup = (nx.v[i].grandparent_key && out->v[k].grandparent_key &&
                   !strcmp(nx.v[i].grandparent_key, out->v[k].grandparent_key)) ||
                  (nx.v[i].rating_key && out->v[k].rating_key && !strcmp(nx.v[i].rating_key, out->v[k].rating_key));
        if (dup || px_grow(out, &cap)) {
            px_item_free(&nx.v[i]);
            continue;
        }
        out->v[out->n++] = nx.v[i];
    }
    free(nx.v);
    out->total = out->n;
    return 0;
}

/* Intro and credits: Jellyfin's media segments (10.10 on; none before) */
static void segments(plex_ctx *c, plex_item *it)
{
    char path[200];
    cJSON *j, *e;
    plex_ctx t = *c;                /* its errors don't matter */
    int n;
    snprintf(path, sizeof(path), "/MediaSegments/%s?includeSegmentTypes=Intro&includeSegmentTypes=Outro",
             it->rating_key);
    if (!(j = jcall(&t, path, NULL, NULL)))
        return;
    n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "Items"));
    if (n > 8)
        n = 8;
    if (n > 0 && (it->markers = calloc(n, sizeof(plex_marker))) != NULL)
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "Items")) {
            const char *ty = px_jstr(e, "Type");
            plex_marker *k;
            int64_t s = (int64_t)(px_jnum(e, "StartTicks", 0) / TICKS_MS),
                    x = (int64_t)(px_jnum(e, "EndTicks", 0) / TICKS_MS);
            if (it->nmarkers >= n || !ty || (strcmp(ty, "Intro") && strcmp(ty, "Outro")) || x <= s)
                continue;
            k = &it->markers[it->nmarkers++];
            k->type = !strcmp(ty, "Intro") ? PM_INTRO : PM_CREDITS;
            k->start_ms = s;
            k->end_ms = x;
            k->final = k->type == PM_CREDITS && it->duration_ms && x >= it->duration_ms - 2000;
        }
    cJSON_Delete(j);
}

int jf_fetch(plex_ctx *c, const char *path, int size, plex_list *out)
{
    char p[256], u[1400], what[64], id[64], rest[64];
    const char *q = strchr(path, '?'), *fields = size <= HOME_ROW ? ROW_FIELDS : LIST_FIELDS;
    int n, e;
    memset(out, 0, sizeof(*out));
    if (!signed_in(c))
        return -1;
    snprintf(p, sizeof(p), "%.*s", q ? (int)(q - path) : (int)strlen(path), path);
    if (!strcmp(p, "/library/sections"))
        return views_get(c, out);
    if (!strcmp(p, "/library/onDeck")) {
        e = on_deck(c, size, out);
        snprintf(out->title, sizeof(out->title), "Continue watching");
        return e;
    }
    if (!strcmp(p, "/playlists")) {
        snprintf(u, sizeof(u), "/Users/%s/Items?IncludeItemTypes=Playlist&Recursive=true&SortBy=SortName"
                 "&Fields=%s%s&StartIndex=0&Limit=%d", c->user_id, fields, IMAGES, size);
        e = get_items(c, u, 0, out);
        plex_list_keep(out, "playlist");
        for (int i = 0; i < out->n; i++)    /* the videos' only */
            if (out->v[i].kind != PI_FOLDER) {
                px_item_free(&out->v[i]);
                memmove(&out->v[i], &out->v[i + 1], (out->n - i - 1) * sizeof(plex_item));
                out->n--, i--;
            }
        out->total = out->n;
        snprintf(out->title, sizeof(out->title), "Playlists");
        return e;
    }
    what[0] = 0;
    if (sscanf(p, "/library/sections/%d/%63s", &n, what) == 2) {
        int s = section(c, n);
        const char *ct;
        if (s < 0)
            return -1;
        ct = secs[s].ctype;
        if (!strcmp(what, "recentlyAdded")) {
            snprintf(u, sizeof(u), "/Users/%s/Items/Latest?ParentId=%s&Limit=%d&Fields=%s%s", c->user_id,
                     secs[s].id, size, ROW_FIELDS, IMAGES);
            e = get_items(c, u, 1, out);
        } else if (!strcmp(what, "collections")) {
            snprintf(u, sizeof(u), "/Users/%s/Items?IncludeItemTypes=BoxSet&Recursive=true&SortBy=SortName"
                     "&Fields=%s%s&StartIndex=0&Limit=%d", c->user_id, fields, IMAGES, size);
            e = get_items(c, u, 0, out);
        } else if (!strcmp(what, "all")) {
            const char *types = !strcmp(ct, "movies") ? "&IncludeItemTypes=Movie&Recursive=true" :
                                !strcmp(ct, "tvshows") ? "&IncludeItemTypes=Series&Recursive=true" : "";
            snprintf(u, sizeof(u), "/Users/%s/Items?ParentId=%s%s%s%s&Fields=%s%s&StartIndex=0&Limit=%d",
                     c->user_id, secs[s].id, types, *types ? sort_by(q) : "&SortBy=IsFolder,SortName",
                     q && strstr(q, "unwatched=1") ? "&Filters=IsUnplayed" : "", fields, IMAGES, size);
            e = get_items(c, u, 0, out);
        } else {
            px_err(c, "the server has no %s", path);
            return -1;
        }
        snprintf(out->title, sizeof(out->title), "%s", secs[s].name);
        return e;
    }
    rest[0] = 0;
    if (sscanf(p, "/library/metadata/%63[^/]%63s", id, rest) >= 1) {
        if (!*rest) {               /* its details */
            snprintf(u, sizeof(u), "/Users/%s/Items/%s", c->user_id, id);
            if ((e = get_items(c, u, 0, out)) == 0 && out->n && out->v[0].kind == PI_VIDEO)
                segments(c, &out->v[0]);
            if (e == 0 && out->n)
                snprintf(out->title, sizeof(out->title), "%s", out->v[0].title);
            return e;
        }
        if (!strcmp(rest, "/children")) {
            const char *ser = q ? strstr(q, "series=") : NULL, *par = q ? strstr(q, "parent=") : NULL;
            if (ser) {              /* a series' episodes */
                char sid[64];
                snprintf(sid, sizeof(sid), "%.*s", (int)strcspn(ser + 7, "&"), ser + 7);
                snprintf(u, sizeof(u), "/Shows/%s/Episodes?UserId=%s&SeasonId=%s&IsMissing=false&Fields=%s%s",
                         *sid ? sid : id, c->user_id, id, fields, IMAGES);
            } else if (par) {       /* a collection's, a playlist's or a folder's items */
                snprintf(u, sizeof(u), "/Users/%s/Items?ParentId=%s%s&Fields=%s%s&StartIndex=0&Limit=%d",
                         c->user_id, id,
                         !strncmp(par + 7, "box", 3) ? "&SortBy=ProductionYear,SortName" :
                         !strncmp(par + 7, "folder", 6) ? "&SortBy=IsFolder,SortName" : "",
                         fields, IMAGES, size);
            } else {                /* a show's series */
                snprintf(u, sizeof(u), "/Shows/%s/Seasons?UserId=%s&Fields=%s%s", id, c->user_id, fields, IMAGES);
            }
            return get_items(c, u, 0, out);
        }
        if (!strcmp(rest, "/allLeaves")) {
            snprintf(u, sizeof(u), "/Shows/%s/Episodes?UserId=%s&IsMissing=false&Fields=%s%s", id, c->user_id,
                     LIST_FIELDS, IMAGES);
            return get_items(c, u, 0, out);
        }
        if (!strcmp(rest, "/similar")) {
            snprintf(u, sizeof(u), "/Items/%s/Similar?UserId=%s&Limit=20&Fields=%s%s", id, c->user_id, LIST_FIELDS,
                     IMAGES);
            return get_items(c, u, 0, out);
        }
        if (!strcmp(rest, "/extras")) {
            snprintf(u, sizeof(u), "/Users/%s/Items/%s/SpecialFeatures", c->user_id, id);
            return get_items(c, u, 0, out);
        }
    }
    px_err(c, "a Jellyfin server has no %s", path);
    return -1;
}

int jf_search(plex_ctx *c, const char *query, plex_list *out)
{
    static const char *const kinds[3] = { "movie", "show", "episode" };
    char esc[400], u[1000];
    plex_list all;
    int cap = 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "Search");
    if (!query || !*query)
        return 0;
    if (!signed_in(c))
        return -1;
    net_escape(query, esc, sizeof(esc));
    snprintf(u, sizeof(u), "/Users/%s/Items?searchTerm=%s&IncludeItemTypes=Movie,Series,Episode&Recursive=true"
             "&Limit=60&Fields=%s%s", c->user_id, esc, LIST_FIELDS, IMAGES);
    if (get_items(c, u, 0, &all) != 0)
        return -1;
    /* films, then shows, then episodes (under their show's name) */
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < all.n; i++) {
            plex_item *it = &all.v[i];
            char sub[200];
            if (!it->type || strcmp(it->type, kinds[k]) || px_grow(out, &cap))
                continue;
            if (k < 2) {
                snprintf(sub, sizeof(sub), "%s%s%s", k ? "Show" : "Film", it->subtitle ? " \xc2\xb7 " : "",
                         it->subtitle ? it->subtitle : "");
            } else if (it->grandparent_title) {
                snprintf(sub, sizeof(sub), "S%d E%d %s", it->parent_index, it->index, it->title);
                free(it->title);
                it->title = px_dup(it->grandparent_title);
                if (it->show_thumb) {
                    free(it->thumb);
                    it->thumb = px_dup(it->show_thumb);
                }
            } else {
                snprintf(sub, sizeof(sub), "%s", it->subtitle ? it->subtitle : "");
            }
            free(it->subtitle);
            it->subtitle = px_dup(sub);
            out->v[out->n++] = *it;
            memset(it, 0, sizeof(*it));
        }
    plex_list_free(&all);
    out->total = out->n;
    snprintf(out->title, sizeof(out->title), "Search");
    return 0;
}

/* ---- watched, and the rest ---------------------------------------------------------- */

int jf_mark(plex_ctx *c, const plex_item *it, int watched)
{
    char path[200];
    if (!it->rating_key || !signed_in(c))
        return -1;
    snprintf(path, sizeof(path), "/Users/%s/PlayedItems/%s", c->user_id, it->rating_key);
    return jsend(c, watched ? "POST" : "DELETE", path, watched ? "{}" : NULL, API_TIMEOUT);
}

int jf_remove_continue(plex_ctx *c, const plex_item *it)
{
    char path[200];
    if (!it->rating_key || !signed_in(c))
        return -1;
    /* Jellyfin's Continue watching is what has a place to resume from */
    snprintf(path, sizeof(path), "/Users/%s/Items/%s/UserData", c->user_id, it->rating_key);
    return jsend(c, "POST", path, "{\"PlaybackPositionTicks\":0}", API_TIMEOUT);
}

int jf_rate(plex_ctx *c, const plex_item *it, int rating)
{
    (void)it;
    (void)rating;
    px_err(c, "Jellyfin servers don't keep your star ratings%s", NULL);
    return -1;
}

int jf_play_queue(plex_ctx *c, const plex_item *it, plex_playing *pl)
{
    (void)c;
    (void)it;
    pl->pq_id = pl->pq_item_id = 0;     /* Jellyfin has no play queues */
    pl->pq_version = 0;
    return 0;
}

int jf_poster(plex_ctx *c, const char *thumb, int w, int h, char **jpeg, size_t *len)
{
    char url[1024], headers[1024];
    net_buf b;
    if (!thumb || !*thumb)
        return -1;
    /* at least w x h (cut to fit by the caller, as Plex's minSize=1) */
    snprintf(url, sizeof(url), "%s%s%sfillWidth=%d&fillHeight=%d&quality=90&format=Jpg", c->base, thumb,
             strchr(thumb, '?') ? "&" : "?", w, h);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_fetch(url, headers, NULL, &b, PROBE_TIMEOUT * 2, c->err, sizeof(c->err)) != 0)
        return -1;
    *jpeg = b.data;
    *len = b.len;
    return 0;
}

/* ---- playing --------------------------------------------------------------------- */

static char playing_session[48];    /* the session Jellyfin has been told has started */

int jf_timeline(plex_ctx *c, const plex_item *it, const char *state, int64_t time_ms, int64_t duration_ms,
                const plex_playing *pl)
{
    cJSON *b;
    char body[1024];
    const char *sess = pl && *pl->session ? pl->session : "-";
    int stopped = !strcmp(state, "stopped"), sub = plex_sub_selected(it), aud = plex_audio_selected(it), e;
    const char *path;
    int start = 0;
    (void)duration_ms;
    if (!it->rating_key || !signed_in(c))
        return -1;
    if (time_ms < 0)
        time_ms = 0;
    b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "ItemId", it->rating_key);
    if (it->source_id)
        cJSON_AddStringToObject(b, "MediaSourceId", it->source_id);
    if (pl && *pl->session)
        cJSON_AddStringToObject(b, "PlaySessionId", pl->session);
    cJSON_AddNumberToObject(b, "PositionTicks", (double)(time_ms * TICKS_MS));
    cJSON_AddBoolToObject(b, "IsPaused", !strcmp(state, "paused"));
    cJSON_AddBoolToObject(b, "CanSeek", 1);
    cJSON_AddStringToObject(b, "PlayMethod", pl && pl->direct ? "DirectPlay" : "Transcode");
    if (aud >= 0)
        cJSON_AddNumberToObject(b, "AudioStreamIndex", (double)(it->auds[aud].id - 1));
    cJSON_AddNumberToObject(b, "SubtitleStreamIndex", sub >= 0 ? (double)(it->subs[sub].id - 1) : -1);
    if (!cJSON_PrintPreallocated(b, body, sizeof(body), 0)) {
        cJSON_Delete(b);
        return -1;
    }
    cJSON_Delete(b);
    if (stopped) {
        path = "/Sessions/Playing/Stopped";
        playing_session[0] = 0;
    } else if (strcmp(playing_session, sess)) {
        path = "/Sessions/Playing";         /* the first: it has started */
        start = 1;
        snprintf(playing_session, sizeof(playing_session), "%s", sess);
    } else {
        path = "/Sessions/Playing/Progress";
    }
    /* short: it's sent while playing, from the desktop's own time */
    e = jsend(c, "POST", path, body, 3000);
    if (e != 0 && start)
        playing_session[0] = 0;             /* the start wasn't heard: again next time */
    return e;
}

int jf_transcode_call(plex_ctx *c, const char *what, const char *session)
{
    char path[400], es[96], ed[150];
    if (!session || !*session)
        return -1;
    net_escape(session, es, sizeof(es));
    if (!strcmp(what, "ping")) {
        snprintf(path, sizeof(path), "/Sessions/Playing/Ping?playSessionId=%s", es);
        return jsend(c, "POST", path, "{}", API_TIMEOUT);
    }
    net_escape(c->client_id, ed, sizeof(ed));
    snprintf(path, sizeof(path), "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s", ed, es);
    return jsend(c, "DELETE", path, NULL, API_TIMEOUT);
}

/* What Reel can be sent converted: HLS of H.264 and AAC (or MP3) within
   the limits, the subtitles chosen burnt in */
char *jf_device_profile(const caps_t *k)
{
    static const char *const subs[] = { "srt", "subrip", "ass", "ssa", "vtt", "webvtt", "pgs", "pgssub", "dvdsub",
                                        "dvbsub", "sub", "mov_text", "ttml", "smi", NULL };
    cJSON *p = cJSON_CreateObject(), *a, *o, *cond, *x;
    char v[32];
    char *s;
    cJSON_AddStringToObject(p, "Name", "Matinee");
    cJSON_AddNumberToObject(p, "MaxStreamingBitrate", (double)k->max_kbps * 1000);
    cJSON_AddNumberToObject(p, "MaxStaticBitrate", (double)k->max_kbps * 1000);
    cJSON_AddItemToObject(p, "DirectPlayProfiles", cJSON_CreateArray());
    a = cJSON_AddArrayToObject(p, "TranscodingProfiles");
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "Container", "ts");
    cJSON_AddStringToObject(o, "Type", "Video");
    cJSON_AddStringToObject(o, "VideoCodec", "h264");
    cJSON_AddStringToObject(o, "AudioCodec", "aac,mp3");
    cJSON_AddStringToObject(o, "Protocol", "hls");
    cJSON_AddStringToObject(o, "Context", "Streaming");
    snprintf(v, sizeof(v), "%d", k->max_channels);
    cJSON_AddStringToObject(o, "MaxAudioChannels", v);
    cJSON_AddBoolToObject(o, "BreakOnNonKeyFrames", 1);
    cJSON_AddItemToArray(a, o);
    a = cJSON_AddArrayToObject(p, "CodecProfiles");
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "Type", "Video");
    cJSON_AddStringToObject(o, "Codec", "h264");
    cond = cJSON_AddArrayToObject(o, "Conditions");
#define COND(how, prop, val) do { x = cJSON_CreateObject(); cJSON_AddStringToObject(x, "Condition", how); \
        cJSON_AddStringToObject(x, "Property", prop); cJSON_AddStringToObject(x, "Value", val); \
        cJSON_AddBoolToObject(x, "IsRequired", 1); cJSON_AddItemToArray(cond, x); } while (0)
    snprintf(v, sizeof(v), "%d", k->max_w);
    COND("LessThanEqual", "Width", v);
    snprintf(v, sizeof(v), "%d", k->max_h);
    COND("LessThanEqual", "Height", v);
    snprintf(v, sizeof(v), "%d", k->h264_level);
    COND("LessThanEqual", "VideoLevel", v);
    COND("LessThanEqual", "VideoBitDepth", "8");
    COND("EqualsAny", "VideoProfile", "high|main|baseline|constrained baseline");
#undef COND
    cJSON_AddItemToArray(a, o);
    a = cJSON_AddArrayToObject(p, "SubtitleProfiles");
    for (int i = 0; subs[i]; i++) {
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "Format", subs[i]);
        cJSON_AddStringToObject(o, "Method", "Encode");
        cJSON_AddItemToArray(a, o);
    }
    s = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    return s;
}

int jf_play(const plex_ctx *c0, const plex_item *it, const caps_t *k, int allow_direct, long offset_s,
            const char *session, play_t *out)
{
    plex_ctx cc = *c0, *c = &cc;
    char why[160], esc_tok[300], esc_id[150];
    int sel = plex_sub_selected(it), aud = plex_audio_selected(it);
    (void)offset_s;                 /* the server's playlist is the whole video: the player seeks in it */
    memset(out, 0, sizeof(*out));
    if (it->kind != PI_VIDEO || !it->rating_key) {
        snprintf(out->why, sizeof(out->why), "that isn't a video");
        return -1;
    }
    jf_headers(c, c->token, out->headers, sizeof(out->headers));
    net_escape(c->token, esc_tok, sizeof(esc_tok));
    net_escape(c->client_id, esc_id, sizeof(esc_id));
    /* as for Plex: subtitles the player can't draw itself are burnt in */
    if (allow_direct && sel >= 0 && !(k->own_subs && caps_sub_own(&it->subs[sel]))) {
        allow_direct = 0;
        snprintf(why, sizeof(why), "subtitles burnt in: %s", it->subs[sel].title);
    } else if (!allow_direct)
        snprintf(why, sizeof(why), "direct play is off");
    if (allow_direct && caps_direct_ok(k, it, why, sizeof(why))) {
        out->direct = 1;
        if (session && *session)
            snprintf(out->session, sizeof(out->session), "%s", session);
        else
            caps_session_id(c, out->session, sizeof(out->session));
        snprintf(out->url, sizeof(out->url), "%s%s&PlaySessionId=%s&DeviceId=%s&api_key=%s", c->base, it->part_key,
                 out->session, esc_id, esc_tok);
        if (sel >= 0)
            snprintf(out->why, sizeof(out->why), "Direct Play: %s; subtitles: %s", why, it->subs[sel].title);
        else
            snprintf(out->why, sizeof(out->why), "Direct Play: %s", why);
        snprintf(out->key, sizeof(out->key), "jellyfin:%s/%s", c->server_id, it->rating_key);
        return 0;
    }
    if (k->dry) {                   /* what the details page says */
        snprintf(out->why, sizeof(out->why), "Transcoded (%s)", why);
        return 0;
    }
    {
        cJSON *b = cJSON_CreateObject(), *j, *ms, *s, *pick = NULL;
        char *prof = jf_device_profile(k), *body, path[200];
        const char *tu, *psid;
        cJSON_AddStringToObject(b, "UserId", c->user_id);
        cJSON_AddNumberToObject(b, "MaxStreamingBitrate", (double)k->max_kbps * 1000);
        cJSON_AddNumberToObject(b, "StartTimeTicks", 0);
        if (aud >= 0)
            cJSON_AddNumberToObject(b, "AudioStreamIndex", (double)(it->auds[aud].id - 1));
        cJSON_AddNumberToObject(b, "SubtitleStreamIndex", sel >= 0 ? (double)(it->subs[sel].id - 1) : -1);
        if (it->source_id)
            cJSON_AddStringToObject(b, "MediaSourceId", it->source_id);
        if (prof)
            cJSON_AddItemToObject(b, "DeviceProfile", cJSON_Parse(prof));
        free(prof);
        cJSON_AddBoolToObject(b, "EnableDirectPlay", 0);
        cJSON_AddBoolToObject(b, "EnableDirectStream", 0);
        cJSON_AddBoolToObject(b, "EnableTranscoding", 1);
        cJSON_AddBoolToObject(b, "AllowVideoStreamCopy", 1);
        cJSON_AddBoolToObject(b, "AllowAudioStreamCopy", 1);
        cJSON_AddBoolToObject(b, "AutoOpenLiveStream", 1);
        body = cJSON_PrintUnformatted(b);
        cJSON_Delete(b);
        snprintf(path, sizeof(path), "/Items/%s/PlaybackInfo?UserId=%s", it->rating_key, c->user_id);
        j = body ? jcall(c, path, body, NULL) : NULL;
        free(body);
        if (!j) {
            snprintf(out->why, sizeof(out->why), "Can't convert it: %s", c->err);
            return -1;
        }
        ms = cJSON_GetObjectItemCaseSensitive(j, "MediaSources");
        cJSON_ArrayForEach(s, ms)
            if (!pick || (it->source_id && px_jstr(s, "Id") && !strcmp(px_jstr(s, "Id"), it->source_id)))
                pick = s;
        tu = px_jstr(pick, "TranscodingUrl");
        psid = px_jstr(j, "PlaySessionId");
        if (!tu || !*tu) {
            const char *ec = px_jstr(j, "ErrorCode");
            snprintf(out->why, sizeof(out->why), "The server won't convert it%s%s", ec ? ": " : "", ec ? ec : "");
            cJSON_Delete(j);
            return -1;
        }
        snprintf(out->url, sizeof(out->url), "%s%s", c->base, tu);
        if (!strstr(tu, "ApiKey=") && !strstr(tu, "api_key=") &&
            strlen(out->url) + strlen(esc_tok) + 16 < sizeof(out->url))
            snprintf(out->url + strlen(out->url), sizeof(out->url) - strlen(out->url), "%sapi_key=%s",
                     strchr(tu, '?') ? "&" : "?", esc_tok);
        if (psid && *psid)
            snprintf(out->session, sizeof(out->session), "%s", psid);
        else if (session && *session)
            snprintf(out->session, sizeof(out->session), "%s", session);
        else
            caps_session_id(c, out->session, sizeof(out->session));
        cJSON_Delete(j);
        snprintf(out->why, sizeof(out->why), "Transcoded (%s)", why);
    }
    return 0;
}
