/*
 * plex.c - the parts of the Plex API PlexRO uses (see plex.h).
 * Part of riscos-plex. GPL v2 or later.
 */
#include "plex.h"
#include "net.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_TIMEOUT   15000     /* ms */
#define PROBE_TIMEOUT 3000      /* ms, trying one of a server's addresses */
#define PAGE_SIZE     2000      /* items fetched for one list */

static void set_err(plex_ctx *c, const char *fmt, const char *arg)
{
    snprintf(c->err, sizeof(c->err), fmt, arg ? arg : "");
}

void plex_ctx_init(plex_ctx *c, const char *client_id, const char *version)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->client_id, sizeof(c->client_id), "%s", client_id);
    snprintf(c->product, sizeof(c->product), "PlexRO");
    snprintf(c->version, sizeof(c->version), "%s", version);
    /* An unknown platform gets the server's generic profile, which the
       X-Plex-Client-Profile-Extra of each playback request then narrows */
    snprintf(c->platform, sizeof(c->platform), "Generic");
    snprintf(c->device, sizeof(c->device), "RISC OS");
    snprintf(c->device_name, sizeof(c->device_name), "RISC OS computer");
    snprintf(c->plextv, sizeof(c->plextv), "%s", PLEX_TV);
}

void plex_headers(const plex_ctx *c, const char *token, char *out, size_t size)
{
    int n = snprintf(out, size,
                     "Accept: application/json\r\n"
                     "X-Plex-Product: %s\r\n"
                     "X-Plex-Version: %s\r\n"
                     "X-Plex-Client-Identifier: %s\r\n"
                     "X-Plex-Platform: %s\r\n"
                     "X-Plex-Platform-Version: 5\r\n"
                     "X-Plex-Device: %s\r\n"
                     "X-Plex-Device-Name: %s\r\n"
                     "X-Plex-Provides: player\r\n",
                     c->product, c->version, c->client_id, c->platform, c->device, c->device_name);
    if (token && *token && n > 0 && (size_t)n < size)
        snprintf(out + n, size - n, "X-Plex-Token: %s\r\n", token);
}

/* GET/POST and parse the JSON answer; NULL on error (c->err says why) */
static cJSON *get_json(plex_ctx *c, const char *url, const char *token, const char *post, int timeout, int *status)
{
    char headers[1024];
    net_buf b;
    cJSON *j;
    plex_headers(c, token, headers, sizeof(headers));
    if (net_fetch(url, headers, post, &b, timeout, c->err, sizeof(c->err)) != 0) {
        if (status)
            *status = b.status;
        return NULL;
    }
    if (status)
        *status = b.status;
    j = cJSON_ParseWithLength(b.data, b.len);
    if (!j)
        set_err(c, "the answer from %s wasn't JSON", url);
    net_buf_free(&b);
    return j;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && *v->valuestring)      /* some fields come as strings */
        return atof(v->valuestring);
    return def;
}

static int jbool(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valueint != 0;
    if (cJSON_IsString(v))
        return !strcmp(v->valuestring, "1") || !strcmp(v->valuestring, "true");
    return 0;
}

static char *dup_s(const char *s)
{
    char *d;
    if (!s)
        return NULL;
    d = malloc(strlen(s) + 1);
    if (d)
        strcpy(d, s);
    return d;
}

/* ---- sign in -------------------------------------------------------------- */

int plex_pin_create(plex_ctx *c, long *id, char *code, size_t codelen)
{
    char url[256];
    cJSON *j;
    snprintf(url, sizeof(url), "%s/api/v2/pins", c->plextv);
    /* strong=false: the short code plex.tv/link takes */
    if (!(j = get_json(c, url, NULL, "strong=false", API_TIMEOUT, NULL)))
        return -1;
    *id = (long)jnum(j, "id", 0);
    snprintf(code, codelen, "%s", jstr(j, "code") ? jstr(j, "code") : "");
    cJSON_Delete(j);
    if (!*id || !*code) {
        set_err(c, "plex.tv gave no sign-in code%s", NULL);
        return -1;
    }
    return 0;
}

int plex_pin_check(plex_ctx *c, long id)
{
    char url[256];
    const char *tok;
    int status = 0;
    cJSON *j;
    snprintf(url, sizeof(url), "%s/api/v2/pins/%ld", c->plextv, id);
    if (!(j = get_json(c, url, NULL, NULL, API_TIMEOUT, &status)))
        return status == 404 ? -1 : 0;      /* 404: the code has expired; else try again */
    tok = jstr(j, "authToken");
    if (tok && *tok) {
        snprintf(c->account_token, sizeof(c->account_token), "%s", tok);
        cJSON_Delete(j);
        return 1;
    }
    cJSON_Delete(j);
    return 0;
}

/* ---- servers -------------------------------------------------------------- */

/* The order to try a server's addresses in: lower first */
static int conn_rank(const plex_conn *k)
{
    if (k->relay)
        return 40;
    if (k->local)
        return k->https ? 10 : 0;   /* on the local network plain http costs the Pi least */
    return 20;
}

int plex_servers(plex_ctx *c, plex_server *out, int max)
{
    char url[256];
    cJSON *j, *r;
    int n = 0;
    snprintf(url, sizeof(url), "%s/api/v2/resources?includeHttps=1&includeRelay=1", c->plextv);
    if (!(j = get_json(c, url, c->account_token, NULL, API_TIMEOUT, NULL)))
        return -1;
    cJSON_ArrayForEach(r, j) {
        const char *prov = jstr(r, "provides");
        cJSON *conns = cJSON_GetObjectItemCaseSensitive(r, "connections"), *k;
        plex_server *s;
        if (n >= max)
            break;
        if (!prov || !strstr(prov, "server"))
            continue;
        s = &out[n];
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name), "%s", jstr(r, "name") ? jstr(r, "name") : "Plex server");
        snprintf(s->id, sizeof(s->id), "%s", jstr(r, "clientIdentifier") ? jstr(r, "clientIdentifier") : "");
        snprintf(s->token, sizeof(s->token), "%s", jstr(r, "accessToken") ? jstr(r, "accessToken") : c->account_token);
        s->owned = jbool(r, "owned");
        cJSON_ArrayForEach(k, conns) {
            const char *uri = jstr(k, "uri"), *addr = jstr(k, "address"), *proto = jstr(k, "protocol");
            int port = (int)jnum(k, "port", 32400);
            if (jbool(k, "IPv6"))
                continue;               /* RISC OS's Internet stack is IPv4 */
            if (uri && s->nconn < PLEX_CONNS) {
                plex_conn *pc = &s->conn[s->nconn++];
                snprintf(pc->uri, sizeof(pc->uri), "%s", uri);
                pc->local = jbool(k, "local");
                pc->relay = jbool(k, "relay");
                pc->https = !strncmp(uri, "https:", 6);
            }
            /* on the local network, also the plain address (no TLS): the
               server takes it unless "Secure connections" is "Required" */
            if (jbool(k, "local") && !jbool(k, "relay") && addr && proto && !strcmp(proto, "https") &&
                s->nconn < PLEX_CONNS) {
                plex_conn *pc = &s->conn[s->nconn++];
                snprintf(pc->uri, sizeof(pc->uri), "http://%s:%d", addr, port);
                pc->local = 1;
            }
        }
        /* stable sort by rank */
        for (int a = 1; a < s->nconn; a++)
            for (int b = a; b > 0 && conn_rank(&s->conn[b]) < conn_rank(&s->conn[b - 1]); b--) {
                plex_conn t = s->conn[b];
                s->conn[b] = s->conn[b - 1];
                s->conn[b - 1] = t;
            }
        n++;
    }
    cJSON_Delete(j);
    if (n == 0)
        set_err(c, "there are no Plex Media Servers on this account%s", NULL);
    return n;
}

/* Does base answer? Fills server_id (and server_name if found) */
static int probe(plex_ctx *c, const char *base, const char *token, int timeout)
{
    char url[320];
    cJSON *j, *mc;
    int status = 0;
    snprintf(url, sizeof(url), "%s/identity", base);
    if (!(j = get_json(c, url, token, NULL, timeout, NULL)))
        return -1;
    mc = cJSON_GetObjectItemCaseSensitive(j, "MediaContainer");
    snprintf(c->server_id, sizeof(c->server_id), "%s", jstr(mc, "machineIdentifier") ? jstr(mc, "machineIdentifier") : "");
    cJSON_Delete(j);
    /* the token must work too: the list of libraries */
    snprintf(url, sizeof(url), "%s/library/sections", base);
    if (!(j = get_json(c, url, token, NULL, timeout, &status))) {
        if (status == 401)
            set_err(c, "the server didn't take the token%s", NULL);
        return status == 401 ? -2 : -1;
    }
    cJSON_Delete(j);
    return 0;
}

static void use(plex_ctx *c, const char *base, const char *token)
{
    size_t n;
    snprintf(c->base, sizeof(c->base), "%s", base);
    n = strlen(c->base);
    while (n && c->base[n - 1] == '/')
        c->base[--n] = 0;
    snprintf(c->token, sizeof(c->token), "%s", token ? token : "");
}

int plex_use_server(plex_ctx *c, const plex_server *s)
{
    char first[256] = "";
    for (int i = 0; i < s->nconn; i++) {
        int e = probe(c, s->conn[i].uri, s->token, PROBE_TIMEOUT);
        if (e == 0) {
            use(c, s->conn[i].uri, s->token);
            snprintf(c->server_name, sizeof(c->server_name), "%s", s->name);
            c->local = s->conn[i].local;
            return 0;
        }
        if (!*first)
            snprintf(first, sizeof(first), "%s", c->err);
        if (e == -2)
            break;
    }
    if (*first)
        snprintf(c->err, sizeof(c->err), "%s", first);
    else
        snprintf(c->err, sizeof(c->err), "%s: no address to try", s->name);
    return -1;
}

int plex_use_address(plex_ctx *c, const char *base, const char *token)
{
    char url[320];
    cJSON *j;
    char b[256];
    snprintf(b, sizeof(b), "%s", base);
    if (!strstr(b, "://"))
        snprintf(b, sizeof(b), "http://%s", base);
    if (!strchr(strstr(b, "://") + 3, ':'))     /* no port: Plex's */
        snprintf(b + strlen(b), sizeof(b) - strlen(b), ":32400");
    if (probe(c, b, token, API_TIMEOUT) != 0)
        return -1;
    use(c, b, token);
    c->local = 1;
    snprintf(c->server_name, sizeof(c->server_name), "Plex server");
    snprintf(url, sizeof(url), "%s/", c->base);
    if ((j = get_json(c, url, c->token, NULL, API_TIMEOUT, NULL))) {
        const cJSON *mc = cJSON_GetObjectItemCaseSensitive(j, "MediaContainer");
        if (jstr(mc, "friendlyName"))
            snprintf(c->server_name, sizeof(c->server_name), "%s", jstr(mc, "friendlyName"));
        cJSON_Delete(j);
    }
    return 0;
}

/* ---- lists ---------------------------------------------------------------- */

static int grow(plex_list *l, int *cap)
{
    if (l->n < *cap)
        return 0;
    {
        int nc = *cap ? *cap * 2 : 64;
        plex_item *nv = realloc(l->v, nc * sizeof(*nv));
        if (!nv)
            return -1;
        l->v = nv;
        *cap = nc;
    }
    return 0;
}

static char *join_key(const char *path, const char *key)
{
    char *r;
    const char *q;
    size_t pl;
    if (!key)
        return NULL;
    if (*key == '/' || strstr(key, "://"))
        return dup_s(key);
    q = strchr(path, '?');
    pl = q ? (size_t)(q - path) : strlen(path);
    if (!(r = malloc(pl + strlen(key) + 2)))
        return NULL;
    memcpy(r, path, pl);
    while (pl && r[pl - 1] == '/')
        pl--;
    r[pl] = '/';
    strcpy(r + pl + 1, key);
    return r;
}

static void media(plex_item *it, const cJSON *m)
{
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(m, "Media");
    const cJSON *m0 = cJSON_IsArray(media) ? cJSON_GetArrayItem(media, 0) : NULL;
    const cJSON *parts, *p0;
    const char *fr;
    if (!m0)
        return;
    it->container = dup_s(jstr(m0, "container"));
    it->vcodec = dup_s(jstr(m0, "videoCodec"));
    it->acodec = dup_s(jstr(m0, "audioCodec"));
    it->vprofile = dup_s(jstr(m0, "videoProfile"));
    it->width = (int)jnum(m0, "width", 0);
    it->height = (int)jnum(m0, "height", 0);
    it->bitrate_kbps = (int)jnum(m0, "bitrate", 0);
    it->channels = (int)jnum(m0, "audioChannels", 0);
    it->bit_depth = it->vprofile && strstr(it->vprofile, "10") ? 10 : 8;
    fr = jstr(m0, "videoFrameRate");
    if (fr) {
        if (!strcmp(fr, "PAL"))
            it->fps = 25;
        else if (!strcmp(fr, "NTSC"))
            it->fps = 29.97;
        else
            it->fps = atof(fr);
    }
    parts = cJSON_GetObjectItemCaseSensitive(m0, "Part");
    p0 = cJSON_IsArray(parts) ? cJSON_GetArrayItem(parts, 0) : NULL;
    if (p0) {
        const cJSON *st;
        int n = 0;
        it->part_key = dup_s(jstr(p0, "key"));
        it->part_file = dup_s(jstr(p0, "file"));
        it->part_size = (int64_t)jnum(p0, "size", 0);
        it->part_id = (long)jnum(p0, "id", 0);
        /* sound tracks (only the metadata of one item has Stream) */
        cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(p0, "Stream"))
            if (jnum(st, "streamType", 0) == 2)
                n++;
        if (n && (it->auds = calloc(n, sizeof(plex_audio))) != NULL) {
            cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(p0, "Stream")) {
                plex_audio *a;
                const char *t;
                if (jnum(st, "streamType", 0) != 2)
                    continue;
                a = &it->auds[it->nauds++];
                a->id = (long)jnum(st, "id", 0);
                a->codec = dup_s(jstr(st, "codec"));
                a->language = dup_s(jstr(st, "language"));
                t = jstr(st, "extendedDisplayTitle") ? jstr(st, "extendedDisplayTitle") : jstr(st, "displayTitle");
                a->title = dup_s(t ? t : a->language ? a->language : "Sound");
                a->selected = jbool(st, "selected");
            }
        }
        n = 0;
        /* subtitle tracks */
        cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(p0, "Stream"))
            if (jnum(st, "streamType", 0) == 3)
                n++;
        if (n && (it->subs = calloc(n, sizeof(plex_sub))) != NULL) {
            cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(p0, "Stream")) {
                plex_sub *sb;
                const char *t;
                if (jnum(st, "streamType", 0) != 3)
                    continue;
                sb = &it->subs[it->nsubs++];
                sb->id = (long)jnum(st, "id", 0);
                sb->codec = dup_s(jstr(st, "codec"));
                sb->language = dup_s(jstr(st, "language"));
                t = jstr(st, "extendedDisplayTitle") ? jstr(st, "extendedDisplayTitle") : jstr(st, "displayTitle");
                sb->title = dup_s(t ? t : sb->language ? sb->language : "Subtitles");
                sb->forced = jbool(st, "forced");
                sb->selected = jbool(st, "selected");
                sb->key = dup_s(jstr(st, "key"));
                sb->external = sb->key != NULL;
            }
        }
    }
}

/* The names in one of a metadata's tag arrays ("Genre": [{"tag": "Comedy"}...]),
   at most max, joined with ", "; NULL if there are none */
static char *tags(const cJSON *m, const char *name, int max)
{
    const cJSON *t;
    char out[400] = "";
    size_t n = 0;
    int k = 0;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(m, name)) {
        const char *s = jstr(t, "tag");
        if (!s || k >= max)
            continue;
        n += snprintf(out + n, n < sizeof(out) ? sizeof(out) - n : 0, "%s%s", k ? ", " : "", s);
        k++;
        if (n >= sizeof(out))
            break;
    }
    return k ? dup_s(out) : NULL;
}

#define CAST_MAX 12

/* The cast (Role), in Plex's order: the leads first */
static void cast(plex_item *it, const cJSON *m)
{
    const cJSON *r, *roles = cJSON_GetObjectItemCaseSensitive(m, "Role");
    int n = cJSON_GetArraySize(roles);
    if (n <= 0)
        return;
    if (n > CAST_MAX)
        n = CAST_MAX;
    if (!(it->cast = calloc(n, sizeof(plex_person))))
        return;
    cJSON_ArrayForEach(r, roles) {
        plex_person *p;
        if (it->ncast >= n)
            break;
        if (!jstr(r, "tag"))
            continue;
        p = &it->cast[it->ncast++];
        p->name = dup_s(jstr(r, "tag"));
        p->role = dup_s(jstr(r, "role"));
        p->thumb = dup_s(jstr(r, "thumb"));
    }
}

static void add_metadata(plex_list *l, int *cap, const cJSON *m, const char *path, int on_deck)
{
    plex_item *it;
    const char *type = jstr(m, "type");
    char sub[128] = "";
    if (grow(l, cap))
        return;
    it = &l->v[l->n++];
    memset(it, 0, sizeof(*it));
    it->type = dup_s(type ? type : "");
    it->rating_key = dup_s(jstr(m, "ratingKey"));
    it->key = join_key(path, jstr(m, "key"));
    it->duration_ms = (int64_t)jnum(m, "duration", 0);
    it->view_offset_ms = (int64_t)jnum(m, "viewOffset", 0);
    it->watched = jnum(m, "viewCount", 0) > 0;
    it->title = dup_s(jstr(m, "title") ? jstr(m, "title") : "");
    it->thumb = dup_s(jstr(m, "thumb"));
    it->summary = dup_s(jstr(m, "summary"));
    it->art = dup_s(jstr(m, "art") ? jstr(m, "art") : jstr(m, "grandparentArt"));
    it->content_rating = dup_s(jstr(m, "contentRating"));
    it->tagline = dup_s(jstr(m, "tagline"));
    it->year = (int)jnum(m, "year", 0);
    it->index = (int)jnum(m, "index", 0);
    it->parent_index = (int)jnum(m, "parentIndex", 0);
    it->grandparent_key = dup_s(jstr(m, "grandparentRatingKey"));
    it->grandparent_title = dup_s(jstr(m, "grandparentTitle"));
    it->show_thumb = dup_s(jstr(m, "grandparentThumb") ? jstr(m, "grandparentThumb") : jstr(m, "parentThumb"));
    it->genres = tags(m, "Genre", 4);
    it->directors = tags(m, "Director", 3);
    it->writers = tags(m, "Writer", 3);
    it->country = tags(m, "Country", 2);
    it->studio = dup_s(jstr(m, "studio"));
    it->released = dup_s(jstr(m, "originallyAvailableAt"));
    it->guid = dup_s(jstr(m, "guid"));
    it->audience_rating = jnum(m, "audienceRating", 0);
    cast(it, m);
    it->rating = jnum(m, "rating", 0) > 0 ? jnum(m, "rating", 0) : jnum(m, "audienceRating", 0);
    if (!type)
        it->kind = PI_OTHER;
    else if (!strcmp(type, "movie") || !strcmp(type, "episode") || !strcmp(type, "clip"))
        it->kind = PI_VIDEO;
    else if (!strcmp(type, "show") || !strcmp(type, "season") || !strcmp(type, "collection"))
        it->kind = PI_FOLDER;
    else if (!strcmp(type, "playlist") && (!jstr(m, "playlistType") || !strcmp(jstr(m, "playlistType"), "video")))
        it->kind = PI_FOLDER;       /* a playlist of videos: its items */
    else
        it->kind = PI_OTHER;

    if (!strcmp(it->type, "movie")) {
        if (jnum(m, "year", 0) > 0)
            snprintf(sub, sizeof(sub), "%d", (int)jnum(m, "year", 0));
    } else if (!strcmp(it->type, "episode")) {
        int s = (int)jnum(m, "parentIndex", 0), e = (int)jnum(m, "index", 0);
        if (on_deck && jstr(m, "grandparentTitle")) {
            /* Continue watching: the show's name and poster, the episode below */
            snprintf(sub, sizeof(sub), "S%d E%d %s", s, e, it->title);
            free(it->title);
            it->title = dup_s(jstr(m, "grandparentTitle"));
            if (jstr(m, "grandparentThumb")) {
                free(it->thumb);
                it->thumb = dup_s(jstr(m, "grandparentThumb"));
            }
        } else if (s)
            snprintf(sub, sizeof(sub), "S%d E%d", s, e);
        else
            snprintf(sub, sizeof(sub), "Episode %d", e);
    } else if (!strcmp(it->type, "show")) {
        int n = (int)jnum(m, "childCount", 0);
        if (n)
            snprintf(sub, sizeof(sub), "%d season%s", n, n == 1 ? "" : "s");
        it->watched = jnum(m, "leafCount", 0) > 0 && jnum(m, "viewedLeafCount", 0) >= jnum(m, "leafCount", 0);
        it->unwatched = (int)(jnum(m, "leafCount", 0) - jnum(m, "viewedLeafCount", 0));
        if (it->unwatched < 0)
            it->unwatched = 0;
    } else if (!strcmp(it->type, "season")) {
        int n = (int)jnum(m, "leafCount", 0);
        if (on_deck && jstr(m, "parentTitle")) {    /* recently added: "Show", "Season 2" */
            snprintf(sub, sizeof(sub), "%s", it->title);
            free(it->title);
            it->title = dup_s(jstr(m, "parentTitle"));
        } else if (n)
            snprintf(sub, sizeof(sub), "%d episode%s", n, n == 1 ? "" : "s");
        if (!it->grandparent_key)                   /* its show (to open the show's page) */
            it->grandparent_key = dup_s(jstr(m, "parentRatingKey"));
        if (!it->grandparent_title)
            it->grandparent_title = dup_s(jstr(m, "parentTitle"));
        it->watched = n > 0 && jnum(m, "viewedLeafCount", 0) >= n;
        it->unwatched = n - (int)jnum(m, "viewedLeafCount", 0);
        if (it->unwatched < 0)
            it->unwatched = 0;
    }
    if (!strcmp(it->type, "collection") || !strcmp(it->type, "playlist")) {
        int n = (int)jnum(m, "childCount", jnum(m, "leafCount", 0));
        if (n)
            snprintf(sub, sizeof(sub), "%d item%s", n, n == 1 ? "" : "s");
        if (!it->thumb)             /* a playlist's picture: its composite */
            it->thumb = dup_s(jstr(m, "composite"));
    }
    if (*sub)
        it->subtitle = dup_s(sub);
    if (it->kind == PI_VIDEO)
        media(it, m);
}

int plex_list_parse(const char *json, const char *path, plex_list *out)
{
    cJSON *j = cJSON_Parse(json), *mc, *e;
    int cap = 0;
    int sections = !strcmp(path, "/library/sections");
    /* on deck and recently added: an episode (or a series) under its show's name */
    int on_deck = !strncmp(path, "/library/onDeck", 15) || strstr(path, "/recentlyAdded") != NULL;
    memset(out, 0, sizeof(*out));
    if (!j)
        return -1;
    mc = cJSON_GetObjectItemCaseSensitive(j, "MediaContainer");
    if (!cJSON_IsObject(mc)) {
        cJSON_Delete(j);
        return -1;
    }
    {
        const char *t = jstr(mc, "title2") ? jstr(mc, "title2") :
                        jstr(mc, "librarySectionTitle") ? jstr(mc, "librarySectionTitle") : jstr(mc, "title1");
        snprintf(out->title, sizeof(out->title), "%s", t ? t : "");
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(mc, "Directory")) {
        plex_item *it;
        const char *type = jstr(e, "type");
        if (grow(out, &cap))
            break;
        it = &out->v[out->n++];
        memset(it, 0, sizeof(*it));
        it->title = dup_s(jstr(e, "title") ? jstr(e, "title") : "");
        it->type = dup_s(type ? type : "");
        it->thumb = dup_s(jstr(e, "thumb"));
        it->rating_key = dup_s(jstr(e, "ratingKey"));
        if (sections && jstr(e, "key")) {
            char k[160];
            snprintf(k, sizeof(k), "/library/sections/%s/all", jstr(e, "key"));
            it->key = dup_s(k);
            /* music and photos: shown, but PlexRO plays video only for now */
            it->kind = type && (!strcmp(type, "movie") || !strcmp(type, "show")) ? PI_FOLDER : PI_OTHER;
            it->subtitle = dup_s(type && !strcmp(type, "movie") ? "Films" :
                                 type && !strcmp(type, "show") ? "TV" :
                                 type && !strcmp(type, "artist") ? "Music" :
                                 type && !strcmp(type, "photo") ? "Photos" : NULL);
        } else {
            it->key = join_key(path, jstr(e, "key"));
            it->kind = PI_FOLDER;
        }
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(mc, "Metadata"))
        add_metadata(out, &cap, e, path, on_deck);
    out->total = (int)jnum(mc, "totalSize", out->n);
    if (out->total < out->n)
        out->total = out->n;
    cJSON_Delete(j);
    return 0;
}

static int list_fetch(plex_ctx *c, const char *path, int size, plex_list *out);

int plex_list_get(plex_ctx *c, const char *path, plex_list *out)
{
    memset(out, 0, sizeof(*out));
    if (!*c->base) {
        set_err(c, "no server chosen%s", NULL);
        return -1;
    }
    if (!strncmp(path, "search:", 7)) {        /* a search, as a list (Back, Refresh) */
        if (plex_search(c, path + 7, out) != 0)
            return -1;
        snprintf(out->title, sizeof(out->title), "Search");
        return 0;
    }
    if (!*path) {
        /* the top: Continue watching, then the libraries */
        plex_list secs;
        if (plex_list_get(c, "/library/sections", &secs) != 0)
            return -1;
        out->v = calloc(secs.n + 1, sizeof(plex_item));
        if (!out->v) {
            plex_list_free(&secs);
            return -1;
        }
        out->v[0].kind = PI_FOLDER;
        out->v[0].title = dup_s("Continue watching");
        out->v[0].key = dup_s("/library/onDeck");
        out->v[0].type = dup_s("ondeck");
        memcpy(out->v + 1, secs.v, secs.n * sizeof(plex_item));
        out->n = out->total = secs.n + 1;
        free(secs.v);               /* the items moved to out */
        snprintf(out->title, sizeof(out->title), "%s", c->server_name);
        return 0;
    }
    return list_fetch(c, path, PAGE_SIZE, out);
}

/* One list from the server: at most size items */
static int list_fetch(plex_ctx *c, const char *path, int size, plex_list *out)
{
    char url[1024];
    char headers[1024];
    net_buf b;
    memset(out, 0, sizeof(*out));
    snprintf(url, sizeof(url), "%s%s%sX-Plex-Container-Start=0&X-Plex-Container-Size=%d",
             c->base, path, strchr(path, '?') ? "&" : "?", size);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_fetch(url, headers, NULL, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0)
        return -1;
    if (plex_list_parse(b.data, path, out) != 0) {
        set_err(c, "the server's list for %s wasn't understood", path);
        net_buf_free(&b);
        return -1;
    }
    net_buf_free(&b);
    return 0;
}

static void item_free(plex_item *it)
{
    free(it->title); free(it->subtitle); free(it->show_thumb); free(it->key); free(it->rating_key); free(it->type);
    free(it->thumb); free(it->container); free(it->vcodec); free(it->acodec); free(it->vprofile);
    free(it->part_key); free(it->part_file);
    free(it->summary); free(it->art); free(it->content_rating); free(it->tagline);
    free(it->grandparent_key); free(it->grandparent_title);
    free(it->genres); free(it->directors); free(it->writers); free(it->studio); free(it->country);
    free(it->released); free(it->guid);
    for (int i = 0; i < it->ncast; i++) {
        free(it->cast[i].name); free(it->cast[i].role); free(it->cast[i].thumb);
    }
    free(it->cast);
    for (int i = 0; i < it->nsubs; i++) {
        free(it->subs[i].title); free(it->subs[i].codec); free(it->subs[i].language); free(it->subs[i].key);
    }
    free(it->subs);
    for (int i = 0; i < it->nauds; i++) {
        free(it->auds[i].title); free(it->auds[i].codec); free(it->auds[i].language); free(it->auds[i].key);
    }
    free(it->auds);
}

int plex_list_append(plex_list *dst, plex_list *src)
{
    plex_item *v = realloc(dst->v, (size_t)(dst->n + src->n + 1) * sizeof(plex_item));
    if (!v) {
        plex_list_free(src);
        return -1;
    }
    memcpy(v + dst->n, src->v, (size_t)src->n * sizeof(plex_item));
    dst->v = v;
    dst->n += src->n;
    dst->total = dst->n;
    free(src->v);                   /* the items moved to dst */
    memset(src, 0, sizeof(*src));
    return 0;
}

int plex_home(plex_ctx *c, plex_list *out, plex_row *rows, int max, int *nrows, plex_list *libs)
{
    plex_list secs, l;
    int nr = 0;
    memset(out, 0, sizeof(*out));
    memset(libs, 0, sizeof(*libs));
    *nrows = 0;
    if (!*c->base) {
        set_err(c, "no server chosen%s", NULL);
        return -1;
    }
    if (plex_list_get(c, "/library/sections", &secs) != 0)
        return -1;
    /* Continue watching */
    if (list_fetch(c, "/library/onDeck", HOME_ROW, &l) == 0) {
        if (l.n && nr < max) {
            rows[nr].kind = PR_CONTINUE;
            rows[nr].start = out->n;
            rows[nr].n = l.n;
            snprintf(rows[nr].title, sizeof(rows[nr].title), "Continue watching");
            snprintf(rows[nr].path, sizeof(rows[nr].path), "/library/onDeck");
            nr++;
        }
        plex_list_append(out, &l);
    }
    /* recently added, in each film and TV library */
    for (int i = 0; i < secs.n && nr < max; i++) {
        const plex_item *s2 = &secs.v[i];
        char path[200];
        const char *k = s2->key ? strstr(s2->key, "/library/sections/") : NULL;
        int id;
        if (s2->kind != PI_FOLDER || !k || sscanf(k, "/library/sections/%d", &id) != 1)
            continue;
        snprintf(path, sizeof(path), "/library/sections/%d/recentlyAdded", id);
        if (list_fetch(c, path, HOME_ROW, &l) != 0)
            continue;
        if (l.n) {
            rows[nr].kind = PR_RECENT;
            rows[nr].start = out->n;
            rows[nr].n = l.n;
            snprintf(rows[nr].title, sizeof(rows[nr].title), "Recently added in %s", s2->title);
            snprintf(rows[nr].path, sizeof(rows[nr].path), "%s", path);
            nr++;
        }
        plex_list_append(out, &l);
    }
    *libs = secs;                   /* the libraries: the caller's, for its tabs */
    snprintf(out->title, sizeof(out->title), "%s", c->server_name);
    *nrows = nr;
    return 0;
}

void plex_list_keep(plex_list *l, const char *type)
{
    int n = 0;
    for (int i = 0; i < l->n; i++) {
        if (l->v[i].type && !strcmp(l->v[i].type, type))
            l->v[n++] = l->v[i];
        else
            item_free(&l->v[i]);
    }
    l->n = n;
}

void plex_list_free(plex_list *l)
{
    for (int i = 0; i < l->n; i++)
        item_free(&l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

/* ---- one video's details, and its subtitles ------------------------------------- */

int plex_details(plex_ctx *c, const plex_item *it, plex_list *out)
{
    char path[160];
    memset(out, 0, sizeof(*out));
    if (!it->rating_key) {
        set_err(c, "no details for that%s", NULL);
        return -1;
    }
    snprintf(path, sizeof(path), "/library/metadata/%s", it->rating_key);
    if (plex_list_get(c, path, out) != 0)
        return -1;
    if (out->n < 1) {
        plex_list_free(out);
        set_err(c, "the server has no details for %s", path);
        return -1;
    }
    return 0;
}

int plex_sub_selected(const plex_item *it)
{
    for (int i = 0; i < it->nsubs; i++)
        if (it->subs[i].selected)
            return i;
    return -1;
}

int plex_set_subtitle(plex_ctx *c, const plex_item *it, long stream_id)
{
    char url[512], headers[1024];
    net_buf b;
    if (!it->part_id) {
        set_err(c, "no file to choose subtitles for%s", NULL);
        return -1;
    }
    snprintf(url, sizeof(url), "%s/library/parts/%ld?subtitleStreamID=%ld&allParts=1", c->base, it->part_id,
             stream_id);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_send(url, headers, "PUT", NULL, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

int plex_audio_selected(const plex_item *it)
{
    for (int i = 0; i < it->nauds; i++)
        if (it->auds[i].selected)
            return i;
    return it->nauds ? 0 : -1;
}

int plex_remove_continue(plex_ctx *c, const plex_item *it)
{
    char url[512], headers[1024];
    net_buf b;
    if (!it->rating_key) {
        set_err(c, "it isn't something the server knows%s", NULL);
        return -1;
    }
    snprintf(url, sizeof(url), "%s/actions/removeFromContinueWatching?ratingKey=%s", c->base, it->rating_key);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_send(url, headers, "PUT", NULL, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

int plex_set_audio(plex_ctx *c, const plex_item *it, long stream_id)
{
    char url[512], headers[1024];
    net_buf b;
    if (!it->part_id) {
        set_err(c, "no file to choose a sound track for%s", NULL);
        return -1;
    }
    snprintf(url, sizeof(url), "%s/library/parts/%ld?audioStreamID=%ld&allParts=1", c->base, it->part_id,
             stream_id);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_send(url, headers, "PUT", NULL, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

/* ---- playing: where it's got to, and what's next -------------------------------- */

int plex_play_queue(plex_ctx *c, const plex_item *it, plex_playing *pl)
{
    char uri[256], esc[512], url[1024];
    cJSON *j, *mc, *m0;
    pl->pq_id = pl->pq_item_id = 0;
    pl->pq_version = 0;
    if (!it->rating_key || !*c->server_id)
        return -1;
    snprintf(uri, sizeof(uri), "server://%s/com.plexapp.plugins.library/library/metadata/%s", c->server_id,
             it->rating_key);
    net_escape(uri, esc, sizeof(esc));
    snprintf(url, sizeof(url), "%s/playQueues?type=video&uri=%s&continuous=0&repeat=0&own=1&includeChapters=0",
             c->base, esc);
    if (!(j = get_json(c, url, c->token, "", API_TIMEOUT, NULL)))
        return -1;
    mc = cJSON_GetObjectItemCaseSensitive(j, "MediaContainer");
    pl->pq_id = (long)jnum(mc, "playQueueID", 0);
    pl->pq_version = (int)jnum(mc, "playQueueVersion", 1);
    pl->pq_item_id = (long)jnum(mc, "playQueueSelectedItemID", 0);
    m0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(mc, "Metadata"), 0);
    if (!pl->pq_item_id && m0)
        pl->pq_item_id = (long)jnum(m0, "playQueueItemID", 0);
    cJSON_Delete(j);
    if (!pl->pq_id) {
        set_err(c, "the server made no play queue%s", NULL);
        return -1;
    }
    return 0;
}

int plex_timeline(plex_ctx *c, const plex_item *it, const char *state, int64_t time_ms, int64_t duration_ms,
                  const plex_playing *pl)
{
    char url[1400], esc[64], guid[300], headers[1200];
    net_buf b;
    size_t n, u;
    if (!it->rating_key)
        return -1;
    net_escape(it->rating_key, esc, sizeof(esc));
    if (time_ms < 0)
        time_ms = 0;
    u = snprintf(url, sizeof(url),
                 "%s/:/timeline?ratingKey=%s&key=%%2Flibrary%%2Fmetadata%%2F%s&state=%s&time=%lld&duration=%lld"
                 "&playbackTime=%lld&hasMDE=1&identifier=com.plexapp.plugins.library&type=video"
                 "&mediaIndex=0&partIndex=0&partCount=1",
                 c->base, esc, esc, state, (long long)time_ms, (long long)duration_ms, (long long)time_ms);
    if (it->guid && u < sizeof(url)) {
        net_escape(it->guid, guid, sizeof(guid));
        u += snprintf(url + u, sizeof(url) - u, "&guid=%s", guid);
    }
    if (pl && pl->pq_id && u < sizeof(url))     /* as the Plex apps: from a play queue */
        u += snprintf(url + u, sizeof(url) - u,
                      "&containerKey=%%2FplayQueues%%2F%ld&playQueueID=%ld&playQueueVersion=%d&playQueueItemID=%ld",
                      pl->pq_id, pl->pq_id, pl->pq_version, pl->pq_item_id);
    plex_headers(c, c->token, headers, sizeof(headers));
    n = strlen(headers);
    if (pl && *pl->session)         /* the same id the stream was asked for with */
        snprintf(headers + n, sizeof(headers) - n, "X-Plex-Session-Identifier: %s\r\n", pl->session);
    /* short: it's sent while playing, from the desktop's own time */
    if (net_fetch(url, headers, NULL, &b, 3000, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

int plex_search(plex_ctx *c, const char *query, plex_list *out)
{
    static const char *const kinds[3] = { "movie", "show", "episode" };
    char esc[400], url[1024];
    cJSON *j, *hubs, *h, *m;
    int cap = 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "Search");
    if (!query || !*query)
        return 0;
    net_escape(query, esc, sizeof(esc));
    snprintf(url, sizeof(url), "%s/hubs/search?query=%s&limit=20&includeCollections=0", c->base, esc);
    if (!(j = get_json(c, url, c->token, NULL, API_TIMEOUT, NULL)))
        return -1;
    hubs = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "MediaContainer"), "Hub");
    for (int k = 0; k < 3; k++)                 /* films, then shows, then episodes */
        cJSON_ArrayForEach(h, hubs) {
            const char *t = jstr(h, "type");
            if (!t || strcmp(t, kinds[k]))
                continue;
            cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(h, "Metadata")) {
                plex_item *it;
                char sub[160];
                int before = out->n;
                add_metadata(out, &cap, m, "/hubs/search", k == 2);
                if (out->n == before)
                    continue;
                it = &out->v[out->n - 1];
                if (k < 2) {                    /* what it is, before its year or seasons */
                    snprintf(sub, sizeof(sub), "%s%s%s", k ? "Show" : "Film", it->subtitle ? " \xc2\xb7 " : "",
                             it->subtitle ? it->subtitle : "");
                    free(it->subtitle);
                    it->subtitle = dup_s(sub);
                }
            }
        }
    out->total = out->n;
    cJSON_Delete(j);
    return 0;
}

static int transcode_call(plex_ctx *c, const char *what, const char *session)
{
    char url[512], esc[96], headers[1024];
    net_buf b;
    if (!session || !*session)
        return -1;
    net_escape(session, esc, sizeof(esc));
    snprintf(url, sizeof(url), "%s/video/:/transcode/universal/%s?session=%s", c->base, what, esc);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_fetch(url, headers, NULL, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

int plex_transcode_stop(plex_ctx *c, const char *session) { return transcode_call(c, "stop", session); }
int plex_transcode_ping(plex_ctx *c, const char *session) { return transcode_call(c, "ping", session); }

int plex_next_episode(plex_ctx *c, const plex_item *it, plex_list *out)
{
    char path[160];
    plex_list all;
    int i;
    memset(out, 0, sizeof(*out));
    if (!it->type || strcmp(it->type, "episode") || !it->grandparent_key || !it->rating_key)
        return -1;
    /* every episode of the show, in order */
    snprintf(path, sizeof(path), "/library/metadata/%s/allLeaves", it->grandparent_key);
    if (plex_list_get(c, path, &all) != 0)
        return -1;
    for (i = 0; i < all.n; i++)
        if (all.v[i].rating_key && !strcmp(all.v[i].rating_key, it->rating_key))
            break;
    if (i + 1 < all.n) {
        /* the one after it, moved into a list of its own */
        out->v = malloc(sizeof(plex_item));
        if (out->v) {
            out->v[0] = all.v[i + 1];
            memset(&all.v[i + 1], 0, sizeof(plex_item));
            out->n = 1;
        }
    }
    plex_list_free(&all);
    return 0;
}

/* ---- the rest ------------------------------------------------------------- */

int plex_poster(plex_ctx *c, const char *thumb, int w, int h, char **jpeg, size_t *len)
{
    char esc[512], url[1024], headers[1024];
    net_buf b;
    net_escape(thumb, esc, sizeof(esc));
    snprintf(url, sizeof(url), "%s/photo/:/transcode?width=%d&height=%d&minSize=1&upscale=1&format=jpeg&url=%s",
             c->base, w, h, esc);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_fetch(url, headers, NULL, &b, PROBE_TIMEOUT * 2, c->err, sizeof(c->err)) != 0)
        return -1;
    *jpeg = b.data;
    *len = b.len;
    return 0;
}

int plex_mark(plex_ctx *c, const plex_item *it, int watched)
{
    char url[512], esc[64], headers[1024];
    net_buf b;
    if (!it->rating_key)
        return -1;
    net_escape(it->rating_key, esc, sizeof(esc));
    snprintf(url, sizeof(url), "%s/:/%s?key=%s&identifier=com.plexapp.plugins.library",
             c->base, watched ? "scrobble" : "unscrobble", esc);
    plex_headers(c, c->token, headers, sizeof(headers));
    if (net_fetch(url, headers, NULL, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0)
        return -1;
    net_buf_free(&b);
    return 0;
}

void plex_part_url(const plex_ctx *c, const plex_item *it, char *url, size_t size)
{
    snprintf(url, size, "%s%s", c->base, it->part_key ? it->part_key : "");
}
