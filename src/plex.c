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
                     "X-Plex-Device: %s\r\n"
                     "X-Plex-Device-Name: %s\r\n",
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
        it->part_key = dup_s(jstr(p0, "key"));
        it->part_file = dup_s(jstr(p0, "file"));
        it->part_size = (int64_t)jnum(p0, "size", 0);
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
    if (!type)
        it->kind = PI_OTHER;
    else if (!strcmp(type, "movie") || !strcmp(type, "episode") || !strcmp(type, "clip"))
        it->kind = PI_VIDEO;
    else if (!strcmp(type, "show") || !strcmp(type, "season") || !strcmp(type, "collection"))
        it->kind = PI_FOLDER;
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
    } else if (!strcmp(it->type, "season")) {
        int n = (int)jnum(m, "leafCount", 0);
        if (n)
            snprintf(sub, sizeof(sub), "%d episode%s", n, n == 1 ? "" : "s");
        it->watched = n > 0 && jnum(m, "viewedLeafCount", 0) >= n;
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
    int on_deck = !strncmp(path, "/library/onDeck", 15);
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

int plex_list_get(plex_ctx *c, const char *path, plex_list *out)
{
    char url[1024];
    char headers[1024];
    net_buf b;
    memset(out, 0, sizeof(*out));
    if (!*c->base) {
        set_err(c, "no server chosen%s", NULL);
        return -1;
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
    snprintf(url, sizeof(url), "%s%s%sX-Plex-Container-Start=0&X-Plex-Container-Size=%d",
             c->base, path, strchr(path, '?') ? "&" : "?", PAGE_SIZE);
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
    free(it->title); free(it->subtitle); free(it->key); free(it->rating_key); free(it->type);
    free(it->thumb); free(it->container); free(it->vcodec); free(it->acodec); free(it->vprofile);
    free(it->part_key); free(it->part_file);
}

void plex_list_free(plex_list *l)
{
    for (int i = 0; i < l->n; i++)
        item_free(&l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
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
