/*
 * dlna.c - DLNA (UPnP AV) media servers (see dlna.h).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "dlna.h"
#include "plex_int.h"
#include "net.h"
#include "xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define SSDP_ADDR   "239.255.255.250"
#define SSDP_PORT   1900
#define LIBS_MAX    200             /* the top level's folders asked for */
#define SEARCH_MAX  200
#define RES_MAX     8               /* a video's files and streams */
#define PLACES_MAX  500

static char ssdp_addr[64] = SSDP_ADDR;
static int ssdp_port = SSDP_PORT;

void dlna_set_ssdp(const char *addr, int port)
{
    snprintf(ssdp_addr, sizeof(ssdp_addr), "%s", addr && *addr ? addr : SSDP_ADDR);
    ssdp_port = port > 0 ? port : SSDP_PORT;
}

static long ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- addresses -------------------------------------------------------------------- */

/* scheme://host[:port] of url, into out */
static void origin(const char *url, char *out, size_t size)
{
    const char *h = strstr(url, "://");
    size_t n = h ? (size_t)(h + 3 - url) + strcspn(h + 3, "/?#") : strlen(url);
    if (n >= size)
        n = size - 1;
    memcpy(out, url, n);
    out[n] = 0;
}

/* rel made absolute against base (a document's address) */
static void resolve(const char *base, const char *rel, char *out, size_t size)
{
    char o[256];
    if (!rel)
        rel = "";
    if (!strncasecmp(rel, "http://", 7) || !strncasecmp(rel, "https://", 8)) {
        snprintf(out, size, "%s", rel);
        return;
    }
    origin(base, o, sizeof(o));
    if (*rel == '/') {
        snprintf(out, size, "%s%s", o, rel);
    } else {
        const char *path = base + strlen(o), *slash = strrchr(path, '/');
        int dir = slash ? (int)(slash + 1 - base) : (int)strlen(o);
        snprintf(out, size, "%.*s%s%s", dir, base, slash ? "" : "/", rel);
    }
}

/* host[:port] of url, for showing */
static void host_of(const char *url, char *out, size_t size)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    snprintf(out, size, "%.*s", (int)strcspn(h, "/?#"), h);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The %-escapes of s undone */
static void unescape(const char *s, char *out, size_t size)
{
    size_t o = 0;
    while (*s && o + 1 < size) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            out[o++] = (char)(hexval(s[1]) << 4 | hexval(s[2]));
            s += 3;
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = 0;
}

/* ---- finding servers (SSDP) and their descriptions ------------------------------- */

/* M-SEARCH sent to addr:port; each answer's LOCATION added to locs (no
   repeats). The number of locations, or -1 if it couldn't be sent. */
static int ssdp_search(const char *addr, int port, int wait_ms, char (*locs)[256], int max)
{
    struct sockaddr_in to;
    char msg[400], buf[1600];
    int s, n = 0, sent = 0;
    long end;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = inet_addr(addr);
    if (to.sin_addr.s_addr == INADDR_NONE)
        return -1;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
#ifdef IP_MULTICAST_TTL
    {
        unsigned char ttl = 2;      /* the local network (one router at most) */
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (void *)&ttl, sizeof(ttl));
    }
#endif
    snprintf(msg, sizeof(msg),
             "M-SEARCH * HTTP/1.1\r\nHOST: %s:%d\r\nMAN: \"ssdp:discover\"\r\nMX: %d\r\n"
             "ST: urn:schemas-upnp-org:device:MediaServer:1\r\nUSER-AGENT: RISCOS/5 UPnP/1.0 Matinee/1\r\n\r\n",
             addr, port, wait_ms >= 2000 ? 2 : 1);
    for (int i = 0; i < 2; i++)     /* twice: UDP can be lost */
        if (sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to)) >= 0)
            sent++;
    if (!sent) {
        close(s);
        return -1;
    }
    end = ms_now() + wait_ms;
    for (;;) {
        long left = end - ms_now();
        struct timeval tv;
        fd_set fds;
        int r;
        char *l, *loc = NULL;
        if (left <= 0)
            break;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        if (select(s + 1, &fds, NULL, NULL, &tv) <= 0)
            break;
        r = (int)recv(s, buf, sizeof(buf) - 1, 0);
        if (r <= 0)
            continue;
        buf[r] = 0;
        if (strncmp(buf, "HTTP/1.1 200", 12) && strncmp(buf, "HTTP/1.0 200", 12))
            continue;
        for (l = buf; l && *l; l = strstr(l, "\n") ? strstr(l, "\n") + 1 : NULL)
            if (!strncasecmp(l, "LOCATION:", 9)) {
                loc = l + 9;
                while (*loc == ' ' || *loc == '\t')
                    loc++;
                loc[strcspn(loc, "\r\n")] = 0;
                break;
            }
        if (!loc || !*loc || strlen(loc) >= 256)
            continue;
        {
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(locs[i], loc))
                    dup = 1;
            if (!dup && n < max)
                snprintf(locs[n++], 256, "%s", loc);
        }
    }
    close(s);
    return n;
}

typedef struct {
    char name[64], udn[96], ctl[256], svc[96];
} desc_t;

/* The device (dev, or one in its deviceList) with a ContentDirectory; its
   service in *svc */
static const xml_node *find_cd(const xml_node *dev, const xml_node **svc)
{
    const xml_node *list = xml_child(dev, "serviceList"), *d;
    for (const xml_node *s = list ? list->child : NULL; s; s = s->next) {
        const char *t = xml_text(s, "serviceType");
        if (!strcmp(s->name, "service") && t && strstr(t, ":service:ContentDirectory:")) {
            *svc = s;
            return dev;
        }
    }
    list = xml_child(dev, "deviceList");
    for (d = list ? list->child : NULL; d; d = d->next) {
        const xml_node *f = strcmp(d->name, "device") ? NULL : find_cd(d, svc);
        if (f)
            return f;
    }
    return NULL;
}

/* The device description at location: 0 = ok; -1 can't read it (err);
   -2 not a media server (err) */
static int desc_read(const char *location, desc_t *d, char *err, size_t errlen)
{
    net_buf b;
    xml_node *root;
    const xml_node *dev, *svc = NULL;
    const char *base, *ctl;
    memset(d, 0, sizeof(*d));
    if (net_fetch(location, NULL, NULL, &b, PROBE_TIMEOUT, err, errlen) != 0)
        return -1;
    root = xml_parse(b.data);
    net_buf_free(&b);
    dev = root && !strcmp(root->name, "root") ? xml_child(root, "device") : NULL;
    dev = dev ? find_cd(dev, &svc) : NULL;
    ctl = svc ? xml_text(svc, "controlURL") : NULL;
    if (!dev || !ctl || !*ctl) {
        snprintf(err, errlen, "%s isn't a media server (no ContentDirectory)", location);
        xml_free(root);
        return -2;
    }
    base = xml_text(root, "URLBase");
    resolve(base && *base ? base : location, ctl, d->ctl, sizeof(d->ctl));
    snprintf(d->svc, sizeof(d->svc), "%s", xml_text(svc, "serviceType"));
    snprintf(d->name, sizeof(d->name), "%s", xml_text(dev, "friendlyName") ? xml_text(dev, "friendlyName") : "DLNA server");
    snprintf(d->udn, sizeof(d->udn), "%s", xml_text(dev, "UDN") ? xml_text(dev, "UDN") : location);
    xml_free(root);
    return 0;
}

int dlna_discover(dlna_server *out, int max, int wait_ms, char *err, size_t errlen)
{
    char locs[DLNA_FOUND * 2][256], e[256];
    int nl = ssdp_search(ssdp_addr, ssdp_port, wait_ms, locs, DLNA_FOUND * 2), n = 0;
    if (err && errlen)
        *err = 0;
    if (nl < 0) {
        snprintf(err, errlen, "can't send on the network to look for servers");
        return -1;
    }
    for (int i = 0; i < nl && n < max; i++) {
        desc_t d;
        int dup = 0;
        if (desc_read(locs[i], &d, e, sizeof(e)) != 0)
            continue;
        for (int k = 0; k < n; k++)
            if (!strcmp(out[k].udn, d.udn))
                dup = 1;    /* one server, several addresses */
        if (dup)
            continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", d.name);
        snprintf(out[n].udn, sizeof(out[n].udn), "%s", d.udn);
        snprintf(out[n].location, sizeof(out[n].location), "%s", locs[i]);
        host_of(locs[i], out[n].host, sizeof(out[n].host));
        n++;
    }
    return n;
}

static void take(plex_ctx *c, const char *location, const desc_t *d)
{
    c->kind = SRV_DLNA;
    c->local = 1;
    snprintf(c->base, sizeof(c->base), "%s", location);
    snprintf(c->server_name, sizeof(c->server_name), "%s", d->name);
    snprintf(c->server_id, sizeof(c->server_id), "%s", d->udn);
    snprintf(c->ctl, sizeof(c->ctl), "%s", d->ctl);
    snprintf(c->svc, sizeof(c->svc), "%s", d->svc);
    c->token[0] = c->user_id[0] = c->user_name[0] = 0;
}

int dlna_connect(plex_ctx *c, const char *addr)
{
    char a[256], host[160], loc[300], locs[4][256];
    const char *h, *colon;
    desc_t d;
    int port = 0, e = -1, n;
    while (*addr == ' ')
        addr++;
    snprintf(a, sizeof(a), "%s", addr);
    a[strcspn(a, " \r\n")] = 0;
    h = strstr(a, "://");
    h = h ? h + 3 : a;
    /* a description's own address */
    if (h != a && strchr(h, '/') && strchr(h, '/')[1]) {
        if ((e = desc_read(a, &d, c->err, sizeof(c->err))) == 0)
            take(c, a, &d);
        return e;
    }
    snprintf(host, sizeof(host), "%.*s", (int)strcspn(h, "/:"), h);
    colon = strchr(h, ':');
    if (colon)
        port = atoi(colon + 1);
    if (!*host) {
        px_err(c, "type the server's address%s", "");
        return -1;
    }
    /* asked over SSDP (to that machine): its answer says where */
    n = ssdp_search(host, !strcmp(ssdp_addr, SSDP_ADDR) ? SSDP_PORT : ssdp_port, 1200, locs, 4);
    for (int i = 0; i < n; i++)
        if ((e = desc_read(locs[i], &d, c->err, sizeof(c->err))) == 0) {
            take(c, locs[i], &d);
            return 0;
        }
    /* the usual places */
    {
        static const char *const paths[] = { "/rootDesc.xml", "/DeviceDescription.xml", "/description.xml", NULL };
        static const struct { int port; const char *path; } known[] = {
            { 8200, "/rootDesc.xml" },          /* MiniDLNA / ReadyMedia */
            { 32469, "/DeviceDescription.xml" }, /* Plex's DLNA */
            { 0, NULL } };
        for (int i = 0; port && paths[i]; i++) {
            snprintf(loc, sizeof(loc), "http://%s:%d%s", host, port, paths[i]);
            if ((e = desc_read(loc, &d, c->err, sizeof(c->err))) == 0) {
                take(c, loc, &d);
                return 0;
            }
        }
        for (int i = 0; !port && known[i].path; i++) {
            snprintf(loc, sizeof(loc), "http://%s:%d%s", host, known[i].port, known[i].path);
            if ((e = desc_read(loc, &d, c->err, sizeof(c->err))) == 0) {
                take(c, loc, &d);
                return 0;
            }
        }
    }
    if (e != -2)
        snprintf(c->err, sizeof(c->err), "no DLNA server answers at %s%s", host,
                 port ? "" : " (give its port too, or its description's address)");
    return e == -2 ? -2 : -1;
}

/* The control address read again if it isn't known (after starting up) */
static int ensure(plex_ctx *c)
{
    desc_t d;
    if (*c->ctl)
        return 0;
    if (!*c->base) {
        px_err(c, "no server chosen%s", "");
        return -1;
    }
    if (desc_read(c->base, &d, c->err, sizeof(c->err)) != 0)
        return -1;
    snprintf(c->ctl, sizeof(c->ctl), "%s", d.ctl);
    snprintf(c->svc, sizeof(c->svc), "%s", d.svc);
    return 0;
}

/* ---- SOAP ------------------------------------------------------------------------- */

/* An action of ContentDirectory's (args: its arguments as XML); the
   answer's tree (the caller frees it), or NULL (c->err) */
static xml_node *soap(plex_ctx *c, const char *action, const char *args)
{
    char headers[300], *body;
    const char *svc = *c->svc ? c->svc : "urn:schemas-upnp-org:service:ContentDirectory:1";
    net_buf b;
    xml_node *root;
    if (ensure(c) != 0)
        return NULL;
    body = malloc(strlen(args) + strlen(svc) + 600);
    if (!body) {
        px_err(c, "out of memory%s", "");
        return NULL;
    }
    sprintf(body,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
            "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>", action, svc, args, action);
    snprintf(headers, sizeof(headers), "SOAPACTION: \"%s#%s\"\r\n", svc, action);
    if (net_fetch(c->ctl, headers, body, &b, API_TIMEOUT, c->err, sizeof(c->err)) != 0) {
        free(body);
        return NULL;
    }
    free(body);
    root = xml_parse(b.data);
    net_buf_free(&b);
    if (!root)
        px_err(c, "the server's answer wasn't understood%s", "");
    return root;
}

/* ---- places: where you got to, and watched, kept here --------------------------- */

typedef struct {
    char udn[96], id[200];
    int64_t pos, dur;
    int watched;
    long stamp;
    char *title, *thumb;
} place;

static place *places;
static int nplaces, places_read, places_dirty;
static char places_r[300], places_w[300];

void dlna_places_files(const char *read_path, const char *write_path)
{
    snprintf(places_r, sizeof(places_r), "%s", read_path ? read_path : "");
    snprintf(places_w, sizeof(places_w), "%s", write_path ? write_path : "");
    for (int i = 0; i < nplaces; i++) {
        free(places[i].title);
        free(places[i].thumb);
    }
    free(places);
    places = NULL;
    nplaces = places_read = 0;
}

/* A field for the file: no tabs or line ends */
static void clean(char *s)
{
    for (; s && *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static place *place_add(void)
{
    place *p;
    if (nplaces >= PLACES_MAX) {    /* the oldest makes room */
        int old = 0;
        for (int i = 1; i < nplaces; i++)
            if (places[i].stamp < places[old].stamp)
                old = i;
        free(places[old].title);
        free(places[old].thumb);
        places[old] = places[--nplaces];
    }
    p = realloc(places, (size_t)(nplaces + 1) * sizeof(place));
    if (!p)
        return NULL;
    places = p;
    p = &places[nplaces++];
    memset(p, 0, sizeof(*p));
    return p;
}

static void places_load(void)
{
    FILE *f;
    char line[1200];
    if (places_read)
        return;
    places_read = 1;
    if (!*places_r || !(f = fopen(places_r, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        char *fld[8], *p = line;
        int n = 0;
        place *pl;
        line[strcspn(line, "\r\n")] = 0;
        while (n < 8) {
            fld[n++] = p;
            p = strchr(p, '\t');
            if (!p)
                break;
            *p++ = 0;
        }
        if (n < 8 || !*fld[0] || !*fld[1] || !(pl = place_add()))
            continue;
        snprintf(pl->udn, sizeof(pl->udn), "%s", fld[0]);
        snprintf(pl->id, sizeof(pl->id), "%s", fld[1]);
        pl->pos = strtoll(fld[2], NULL, 10);
        pl->dur = strtoll(fld[3], NULL, 10);
        pl->watched = atoi(fld[4]) != 0;
        pl->stamp = atol(fld[5]);
        pl->title = px_dup(fld[6]);
        pl->thumb = *fld[7] ? px_dup(fld[7]) : NULL;
    }
    fclose(f);
}

static void places_save(void)
{
    FILE *f;
    if (!places_dirty || !*places_w || !(f = fopen(places_w, "w")))
        return;
    for (int i = 0; i < nplaces; i++)
        fprintf(f, "%s\t%s\t%lld\t%lld\t%d\t%ld\t%s\t%s\n", places[i].udn, places[i].id, (long long)places[i].pos,
                (long long)places[i].dur, places[i].watched, places[i].stamp, places[i].title ? places[i].title : "",
                places[i].thumb ? places[i].thumb : "");
    fclose(f);
    places_dirty = 0;
}

static place *place_find(const char *udn, const char *id, int make)
{
    place *p;
    places_load();
    for (int i = 0; i < nplaces; i++)
        if (!strcmp(places[i].id, id) && !strcmp(places[i].udn, udn))
            return &places[i];
    if (!make || !(p = place_add()))
        return NULL;
    snprintf(p->udn, sizeof(p->udn), "%s", udn);
    snprintf(p->id, sizeof(p->id), "%s", id);
    return p;
}

static long stamp_next(void)
{
    long t = (long)time(NULL);
    for (int i = 0; i < nplaces; i++)       /* always after the last, if the clock is wrong */
        if (places[i].stamp >= t)
            t = places[i].stamp + 1;
    return t;
}

/* An item's place, its title and picture kept up to date (for Continue watching) */
static place *place_of(plex_ctx *c, const plex_item *it)
{
    place *p;
    if (!it || it->kind != PI_VIDEO || !it->rating_key || strlen(it->rating_key) >= sizeof(p->id)) {
        px_err(c, "only a video's place is kept%s", "");
        return NULL;
    }
    p = place_find(c->server_id, it->rating_key, 1);
    if (!p) {
        px_err(c, "out of memory%s", "");
        return NULL;
    }
    if (it->title && (!p->title || strcmp(p->title, it->title))) {
        free(p->title);
        p->title = px_dup(it->title);
        clean(p->title);
    }
    if (it->thumb && (!p->thumb || strcmp(p->thumb, it->thumb))) {
        free(p->thumb);
        p->thumb = px_dup(it->thumb);
        clean(p->thumb);
    }
    if (it->duration_ms > 0)
        p->dur = it->duration_ms;
    return p;
}

int dlna_timeline(plex_ctx *c, const plex_item *it, const char *state, int64_t time_ms, int64_t duration_ms)
{
    place *p = place_of(c, it);
    int was;
    if (!p)
        return -1;
    was = p->watched;
    if (duration_ms > 0)
        p->dur = duration_ms;
    p->stamp = stamp_next();
    if (p->dur > 0 && time_ms >= p->dur * 9 / 10) {     /* as Plex: 90% is watched */
        p->watched = 1;
        p->pos = 0;
    } else if (time_ms >= 10000) {
        p->pos = time_ms;
    }
    places_dirty = 1;
    /* written when it stops or pauses (not every few seconds), or is watched */
    if (strcmp(state, "playing") || p->watched != was)
        places_save();
    return 0;
}

int dlna_mark(plex_ctx *c, const plex_item *it, int watched)
{
    place *p;
    if (it->kind != PI_VIDEO) {
        px_err(c, "a DLNA server's folders can't be marked: open it and mark the videos%s", "");
        return -1;
    }
    if (!(p = place_of(c, it)))
        return -1;
    p->watched = watched != 0;
    p->pos = 0;
    p->stamp = stamp_next();
    places_dirty = 1;
    places_save();
    return 0;
}

int dlna_remove_continue(plex_ctx *c, const plex_item *it)
{
    place *p = it->rating_key ? place_find(c->server_id, it->rating_key, 0) : NULL;
    if (p && p->pos) {
        p->pos = 0;
        places_dirty = 1;
        places_save();
    }
    return 0;
}

/* Continue watching: the server's videos started and not finished, the
   latest first */
static int on_deck(plex_ctx *c, int size, plex_list *out)
{
    int cap = 0;
    places_load();
    for (;;) {
        place *best = NULL;
        for (int i = 0; i < nplaces; i++) {
            place *p = &places[i];
            int done = 0;
            if (p->watched || p->pos <= 0 || strcmp(p->udn, c->server_id))
                continue;
            for (int k = 0; k < out->n; k++)
                if (!strcmp(out->v[k].rating_key, p->id))
                    done = 1;
            if (!done && (!best || p->stamp > best->stamp))
                best = p;
        }
        if (!best || out->n >= size || px_grow(out, &cap))
            break;
        {
            plex_item *it = &out->v[out->n++];
            char k[300];
            memset(it, 0, sizeof(*it));
            it->kind = PI_VIDEO;
            it->title = px_dup(best->title && *best->title ? best->title : "?");
            it->rating_key = px_dup(best->id);
            snprintf(k, sizeof(k), "/library/metadata/%s", best->id);
            it->key = px_dup(k);
            it->type = px_dup("movie");
            it->thumb = best->thumb ? px_dup(best->thumb) : NULL;
            it->view_offset_ms = best->pos;
            it->duration_ms = best->dur;
        }
    }
    out->total = out->n;
    snprintf(out->title, sizeof(out->title), "Continue watching");
    return 0;
}

/* ---- DIDL-Lite: the lists ---------------------------------------------------------- */

typedef struct {
    char url[1024];
    char mime[64], pn[64];
    int ci;                         /* converted by the server (DLNA.ORG_CI=1) */
    int w, h, kbps, channels;
    int64_t size, dur_ms;
} res_t;

/* "H:MM:SS.fff" in ms, or 0 */
static int64_t hms(const char *s)
{
    int h = 0, m = 0;
    double sec = 0;
    if (!s || sscanf(s, "%d:%d:%lf", &h, &m, &sec) != 3)
        return 0;
    return ((int64_t)h * 3600 + m * 60) * 1000 + (int64_t)(sec * 1000);
}

static void res_read(plex_ctx *c, const xml_node *r, res_t *o)
{
    const char *pi = xml_attr_get(r, "protocolInfo"), *v;
    memset(o, 0, sizeof(*o));
    resolve(c->base, r->text, o->url, sizeof(o->url));
    if (pi) {                       /* http-get:*:video/mp4:DLNA.ORG_PN=AVC_MP4_...;DLNA.ORG_CI=1;... */
        const char *m = strchr(pi, ':');
        m = m ? strchr(m + 1, ':') : NULL;
        if (m) {
            snprintf(o->mime, sizeof(o->mime), "%.*s", (int)strcspn(m + 1, ":"), m + 1);
            const char *pn = strstr(m, "DLNA.ORG_PN=");
            if (pn)
                snprintf(o->pn, sizeof(o->pn), "%.*s", (int)strcspn(pn + 12, ";"), pn + 12);
            o->ci = strstr(m, "DLNA.ORG_CI=1") != NULL;
        }
    }
    if ((v = xml_attr_get(r, "resolution")) != NULL)
        sscanf(v, "%dx%d", &o->w, &o->h);
    if ((v = xml_attr_get(r, "bitrate")) != NULL)      /* bytes a second, as UPnP has it */
        o->kbps = (int)(strtoll(v, NULL, 10) * 8 / 1000);
    if ((v = xml_attr_get(r, "nrAudioChannels")) != NULL)
        o->channels = atoi(v);
    if ((v = xml_attr_get(r, "size")) != NULL)
        o->size = strtoll(v, NULL, 10);
    o->dur_ms = hms(xml_attr_get(r, "duration"));
}

/* A video's res, up to max; the number */
static int res_all(plex_ctx *c, const xml_node *e, res_t *out, int max)
{
    int n = 0;
    for (const xml_node *r = e->child; r && n < max; r = r->next)
        if (!strcmp(r->name, "res") && *r->text)
            res_read(c, r, &out[n++]);
    return n;
}

/* The file itself: the first not converted (else the first) */
static int res_original(const res_t *r, int n)
{
    for (int i = 0; i < n; i++)
        if (!r[i].ci)
            return i;
    return n ? 0 : -1;
}

/* What the type and the DLNA profile say: Matinee's (Plex's) names */
static const char *container_of(const char *mime)
{
    static const struct { const char *mime, *c; } t[] = {
        { "video/mp4", "mp4" }, { "video/x-m4v", "mp4" }, { "video/x-matroska", "mkv" }, { "video/x-mkv", "mkv" },
        { "video/avi", "avi" }, { "video/x-msvideo", "avi" }, { "video/divx", "avi" }, { "video/x-divx", "avi" },
        { "video/mpeg", "mpeg" }, { "video/mp2t", "mpegts" }, { "video/vnd.dlna.mpeg-tts", "mpegts" },
        { "video/quicktime", "mov" }, { "video/x-ms-wmv", "asf" }, { "video/x-ms-asf", "asf" },
        { "video/x-flv", "flv" }, { "video/webm", "webm" }, { "video/3gpp", "3gp" }, { NULL, NULL } };
    for (int i = 0; mime && t[i].mime; i++)
        if (!strcasecmp(mime, t[i].mime))
            return t[i].c;
    return NULL;
}

static const char *vcodec_of(const char *pn)
{
    if (!pn || !*pn)
        return NULL;
    if (strstr(pn, "HEVC"))
        return "hevc";
    if (strstr(pn, "AVC"))
        return "h264";
    if (strstr(pn, "MPEG4_P2"))
        return "mpeg4";
    if (!strncmp(pn, "MPEG1", 5))
        return "mpeg1video";
    if (!strncmp(pn, "MPEG_PS", 7) || !strncmp(pn, "MPEG_TS", 7) || !strncmp(pn, "MPEG_ES", 7))
        return "mpeg2video";
    if (!strncmp(pn, "WMV", 3))
        return "wmv3";
    if (!strncmp(pn, "VC1", 3))
        return "vc1";
    return NULL;
}

static const char *acodec_of(const char *pn)
{
    if (!pn || !*pn)
        return NULL;
    if (strstr(pn, "_AAC") || strstr(pn, "_HEAAC"))
        return "aac";
    if (strstr(pn, "_EAC3"))
        return "eac3";
    if (strstr(pn, "_AC3"))
        return "ac3";
    if (strstr(pn, "_MP3") || strstr(pn, "_MPEG1_L3"))
        return "mp3";
    if (strstr(pn, "_MPEG1_L2") || strstr(pn, "_MPEG2_L2"))
        return "mp2";
    if (strstr(pn, "_LPCM"))
        return "pcm";
    return NULL;
}

/* The file's details from its res, into it */
static void res_into(plex_item *it, const res_t *r)
{
    const char *cn = container_of(r->mime), *vc = vcodec_of(r->pn), *ac = acodec_of(r->pn);
    const char *path = strstr(r->url, "://"), *slash, *dot;
    char name[200];
    free(it->container); free(it->vcodec); free(it->acodec); free(it->part_key); free(it->part_file);
    it->container = cn ? px_dup(cn) : NULL;
    it->vcodec = vc ? px_dup(vc) : NULL;
    it->acodec = ac ? px_dup(ac) : NULL;
    it->width = r->w;
    it->height = r->h;
    it->bitrate_kbps = r->kbps;
    it->channels = r->channels;
    it->part_size = r->size;
    if (r->dur_ms > 0)
        it->duration_ms = r->dur_ms;
    it->part_key = px_dup(r->url);  /* the whole address (plex_part_url gives it as it is) */
    /* its name, for saving: the address's last part, else the title */
    path = path ? path + 3 : r->url;
    slash = strrchr(path, '/');
    snprintf(name, sizeof(name), "%.*s", slash ? (int)strcspn(slash + 1, "?#") : 0, slash ? slash + 1 : "");
    dot = strrchr(name, '.');
    if (!*name || !dot || dot == name || strlen(dot) > 6)
        snprintf(name, sizeof(name), "%.150s.%s", it->title ? it->title : "video", cn ? cn : "mpg");
    it->part_file = px_dup(name);
}

/* The children called name, their texts joined with ", " (NULL if none) */
static char *joined(const xml_node *e, const char *name)
{
    char t[600] = "";
    size_t n = 0;
    for (const xml_node *k = e->child; k; k = k->next)
        if (!strcmp(k->name, name) && *k->text && n + strlen(k->text) + 3 < sizeof(t))
            n += (size_t)snprintf(t + n, sizeof(t) - n, "%s%s", n ? ", " : "", k->text);
    return n ? px_dup(t) : NULL;
}

/* One container or item of a DIDL-Lite list, added to out */
static void add_entry(plex_ctx *c, plex_list *out, int *cap, const xml_node *e)
{
    plex_item *it;
    const char *id = xml_attr_get(e, "id"), *title = xml_text(e, "title"), *cls = xml_text(e, "class");
    const char *art = xml_text(e, "albumArtURI"), *date = xml_text(e, "date");
    char eid[400], k[480], sub[64] = "";
    if (!id || px_grow(out, cap))
        return;
    it = &out->v[out->n++];
    memset(it, 0, sizeof(*it));
    net_escape(id, eid, sizeof(eid));
    it->rating_key = px_dup(eid);
    it->title = px_dup(title ? title : "");
    if (art && *art) {
        char u[1024];
        resolve(c->base, art, u, sizeof(u));
        it->thumb = px_dup(u);
    }
    if (date && strlen(date) >= 4) {
        it->year = atoi(date);
        it->released = px_dup(date);
    }
    if (!cls)
        cls = "";
    if (!strcmp(e->name, "container")) {
        const char *cc = xml_attr_get(e, "childCount");
        it->kind = PI_FOLDER;
        it->type = px_dup("folder");
        snprintf(k, sizeof(k), "/library/metadata/%s/children", eid);
        it->key = px_dup(k);
        if (cc && *cc)
            snprintf(sub, sizeof(sub), "%d item%s", atoi(cc), atoi(cc) == 1 ? "" : "s");
    } else if (!strncmp(cls, "object.item.videoItem", 21)) {
        res_t r[RES_MAX];
        int n = res_all(c, e, r, RES_MAX), o = res_original(r, n);
        const char *ep = xml_text(e, "episodeNumber"), *se = xml_text(e, "episodeSeason");
        place *p;
        it->kind = PI_VIDEO;
        it->type = px_dup("movie");
        snprintf(k, sizeof(k), "/library/metadata/%s", eid);
        it->key = px_dup(k);
        if (o >= 0)
            res_into(it, &r[o]);
        it->summary = xml_text(e, "longDescription") && *xml_text(e, "longDescription") ?
                      px_dup(xml_text(e, "longDescription")) :
                      xml_text(e, "description") && *xml_text(e, "description") ? px_dup(xml_text(e, "description")) : NULL;
        it->genres = joined(e, "genre");
        it->directors = joined(e, "director");
        {
            int na = 0;
            for (const xml_node *a = e->child; a; a = a->next)
                if (!strcmp(a->name, "actor") && *a->text)
                    na++;
            if (na && (it->cast = calloc((size_t)na, sizeof(plex_person))) != NULL)
                for (const xml_node *a = e->child; a; a = a->next)
                    if (!strcmp(a->name, "actor") && *a->text)
                        it->cast[it->ncast++].name = px_dup(a->text);
        }
        if (ep && atoi(ep) > 0) {
            it->index = atoi(ep);
            it->parent_index = se ? atoi(se) : 0;
            if (it->parent_index > 0)
                snprintf(sub, sizeof(sub), "S%d E%d", it->parent_index, it->index);
            else
                snprintf(sub, sizeof(sub), "Episode %d", it->index);
        } else if (it->year > 1800) {
            snprintf(sub, sizeof(sub), "%d", it->year);
        } else if (it->duration_ms >= 60000) {
            long m = (long)(it->duration_ms / 60000);
            if (m >= 60)
                snprintf(sub, sizeof(sub), "%ldh %02ldm", m / 60, m % 60);
            else
                snprintf(sub, sizeof(sub), "%ld min", m);
        }
        if ((p = place_find(c->server_id, eid, 0)) != NULL) {     /* where you got to, kept here */
            it->watched = p->watched;
            it->view_offset_ms = p->watched ? 0 : p->pos;
        }
    } else {                        /* music and pictures: shown, not played */
        it->kind = PI_OTHER;
        it->type = px_dup(!strncmp(cls, "object.item.audioItem", 21) ? "track" :
                          !strncmp(cls, "object.item.imageItem", 21) ? "photo" : "item");
        snprintf(sub, sizeof(sub), "%s", !strncmp(cls, "object.item.audioItem", 21) ? "Music" :
                                         !strncmp(cls, "object.item.imageItem", 21) ? "Picture" : "");
    }
    if (*sub)
        it->subtitle = px_dup(sub);
}

/* A Browse or Search answer's list into out; its TotalMatches (-1 if the
   answer wasn't one) */
static int add_result(plex_ctx *c, const xml_node *resp, const char *what, plex_list *out)
{
    const xml_node *r = xml_find(resp, what);
    const char *result = r ? xml_text(r, "Result") : NULL, *tm = r ? xml_text(r, "TotalMatches") : NULL;
    xml_node *didl;
    int cap = 0;
    if (!result) {
        px_err(c, "the server's answer had no list%s", "");
        return -1;
    }
    didl = xml_parse(result);
    for (const xml_node *e = didl ? didl->child : NULL; e; e = e->next)
        if (!strcmp(e->name, "container") || !strcmp(e->name, "item"))
            add_entry(c, out, &cap, e);
    xml_free(didl);
    return tm ? atoi(tm) : 0;
}

/* Browse: an object's children (meta 0, from start, count of them) or
   itself (meta 1) */
static int browse(plex_ctx *c, const char *id, int meta, int start, int count, plex_list *out)
{
    char args[1400], eid[700];
    xml_node *resp;
    int total;
    xml_escape(id, eid, sizeof(eid));
    snprintf(args, sizeof(args),
             "<ObjectID>%s</ObjectID><BrowseFlag>%s</BrowseFlag><Filter>*</Filter>"
             "<StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>",
             eid, meta ? "BrowseMetadata" : "BrowseDirectChildren", meta ? 0 : start, meta ? 1 : count);
    if (!(resp = soap(c, "Browse", args)))
        return -1;
    total = add_result(c, resp, "BrowseResponse", out);
    xml_free(resp);
    if (total < 0)
        return -1;
    /* the whole list's size (plex_list_more compares it with what it has);
       a server that doesn't say (0): what's here */
    out->total = meta ? out->n : total > start + out->n ? total : start + out->n;
    return 0;
}

/* ---- plex.c's calls ------------------------------------------------------------- */

/* The id (unescaped) in a /library/metadata/<eid>... path, and what follows it */
static int path_id(const char *path, char *id, size_t size, const char **rest)
{
    const char *p = path + strlen("/library/metadata/");
    size_t n = strcspn(p, "/?");
    char e[700];
    if (strncmp(path, "/library/metadata/", 18) || !n || n >= sizeof(e))
        return -1;
    snprintf(e, sizeof(e), "%.*s", (int)n, p);
    unescape(e, id, size);
    *rest = p + n;
    return 0;
}

/* Music, pictures: not libraries for films (if there are others) */
static int not_video(const char *title)
{
    static const char *const no[] = { "Music", "Audio", "Pictures", "Photos", "Photo", "Images", "Playlists", NULL };
    for (int i = 0; title && no[i]; i++)
        if (!strcasecmp(title, no[i]))
            return 1;
    return 0;
}

int dlna_fetch(plex_ctx *c, const char *path, int start, int size, plex_list *out)
{
    char p[512], id[400];
    const char *rest;
    memset(out, 0, sizeof(*out));
    snprintf(p, sizeof(p), "%.*s", (int)strcspn(path, "?"), path);
    if (!strcmp(p, "/library/sections")) {
        int keep = 0;
        if (browse(c, "0", 0, 0, LIBS_MAX, out) != 0)
            return -1;
        for (int i = 0; i < out->n; i++)
            if (out->v[i].kind == PI_FOLDER && !not_video(out->v[i].title))
                keep++;
        for (int i = 0; i < out->n; i++)
            if (out->v[i].kind != PI_FOLDER || (keep && not_video(out->v[i].title))) {
                px_item_free(&out->v[i]);
                memmove(&out->v[i], &out->v[i + 1], (size_t)(out->n - i - 1) * sizeof(plex_item));
                out->n--, i--;
            }
        out->total = out->n;
        snprintf(out->title, sizeof(out->title), "%s", c->server_name);
        return 0;
    }
    if (!strcmp(p, "/library/onDeck"))
        return on_deck(c, size, out);
    if (path_id(p, id, sizeof(id), &rest) == 0) {
        if (!*rest) {               /* its details */
            if (browse(c, id, 1, 0, 1, out) != 0)
                return -1;
            if (out->n)
                snprintf(out->title, sizeof(out->title), "%s", out->v[0].title);
            return 0;
        }
        if (!strcmp(rest, "/children")) {
            if (browse(c, id, 0, start, size, out) != 0)
                return -1;
            if (!start) {           /* the folder's own name, for the title */
                plex_list self;
                memset(&self, 0, sizeof(self));
                if (browse(c, id, 1, 0, 1, &self) == 0 && self.n)
                    snprintf(out->title, sizeof(out->title), "%s", self.v[0].title);
                plex_list_free(&self);
            }
            return 0;
        }
    }
    /* anything else (similar, extras...): a DLNA server has none */
    return 0;
}

int dlna_search(plex_ctx *c, const char *query, plex_list *out)
{
    char q[300], crit[700], ecrit[1600], args[2000];
    size_t n = 0;
    xml_node *resp;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "Search");
    if (!query || !*query)
        return 0;
    for (const char *s = query; *s && n + 3 < sizeof(q); s++) {     /* a quote in it escaped */
        if (*s == '"' || *s == '\\')
            q[n++] = '\\';
        q[n++] = *s;
    }
    q[n] = 0;
    snprintf(crit, sizeof(crit), "upnp:class derivedfrom \"object.item.videoItem\" and dc:title contains \"%s\"", q);
    xml_escape(crit, ecrit, sizeof(ecrit));
    snprintf(args, sizeof(args),
             "<ContainerID>0</ContainerID><SearchCriteria>%s</SearchCriteria><Filter>*</Filter>"
             "<StartingIndex>0</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>",
             ecrit, SEARCH_MAX);
    if (!(resp = soap(c, "Search", args))) {
        char e[256];
        snprintf(e, sizeof(e), "%s", c->err);
        px_err(c, "this server can't search (%s)", e);
        return -1;
    }
    if (add_result(c, resp, "SearchResponse", out) < 0) {
        xml_free(resp);
        return -1;
    }
    xml_free(resp);
    out->total = out->n;
    snprintf(out->title, sizeof(out->title), "Search");
    return 0;
}

int dlna_poster(plex_ctx *c, const char *thumb, char **jpeg, size_t *len)
{
    net_buf b;
    if (!thumb || !*thumb)
        return -1;
    if (net_fetch(thumb, NULL, NULL, &b, PROBE_TIMEOUT * 2, c->err, sizeof(c->err)) != 0)
        return -1;
    if (b.len < 4 || (unsigned char)b.data[0] != 0xFF || (unsigned char)b.data[1] != 0xD8) {
        net_buf_free(&b);           /* a PNG (some servers' folder pictures): not shown */
        px_err(c, "the picture isn't a JPEG%s", "");
        return -1;
    }
    *jpeg = b.data;
    *len = b.len;
    return 0;
}

/* ---- playing -------------------------------------------------------------------- */

static const char *const containers_ok[] = { "mp4", "mov", "mkv", "avi", "mpegts", "mpeg", NULL };

static int known_container(const char *cn)
{
    for (int i = 0; cn && containers_ok[i]; i++)
        if (!strcmp(cn, containers_ok[i]))
            return 1;
    return 0;
}

/* Whether Reel can play the file as it is: as caps_direct_ok when the
   server says the codec; when it doesn't (most files on MiniDLNA), by its
   type, size and bit rate (a picture bigger than the Quality allows is
   taken for HEVC if there's the HEVC block, as such files mostly are) */
static int direct_ok(const caps_t *k, const plex_item *t, char *why, size_t size)
{
    if (t->vcodec)
        return caps_direct_ok(k, t, why, size);
    if (t->container && !known_container(t->container)) {
        snprintf(why, size, "%s files are converted", t->container);
        return 0;
    }
    if ((t->width > k->max_w || t->height > k->max_h) || t->bitrate_kbps > k->max_kbps) {
        if (k->hevc && t->width <= CAPS_HEVC_W && t->height <= CAPS_HEVC_H && t->bitrate_kbps <= CAPS_HEVC_KBPS) {
            snprintf(why, size, "%dx%d, %.1f Mbit/s (taken for HEVC: the server doesn't say)", t->width, t->height,
                     t->bitrate_kbps / 1000.0);
            return 1;
        }
        if (t->width > k->max_w || t->height > k->max_h)
            snprintf(why, size, "%dx%d is bigger than %dx%d", t->width, t->height, k->max_w, k->max_h);
        else
            snprintf(why, size, "%.1f Mbit/s is more than %.0f", t->bitrate_kbps / 1000.0, k->max_kbps / 1000.0);
        return 0;
    }
    if (t->width > 0)
        snprintf(why, size, "%s %dx%d, %.1f Mbit/s", t->container ? t->container : "a file", t->width, t->height,
                 t->bitrate_kbps / 1000.0);
    else
        snprintf(why, size, "%s (the server doesn't say its size)", t->container ? t->container : "a file");
    return 1;
}

/* Of the converted streams, the biggest within the Quality's size (else
   the smallest); -1 if there are none */
static int res_converted(const res_t *r, int n, int orig, const caps_t *k)
{
    int best = -1, small = -1;
    for (int i = 0; i < n; i++) {
        long a = (long)r[i].w * r[i].h;
        if (i == orig || !strcmp(r[i].url, r[orig].url) || strncmp(r[i].mime, "video/", 6))
            continue;
        if ((r[i].w <= k->max_w && r[i].h <= k->max_h) && (best < 0 || a > (long)r[best].w * r[best].h))
            best = i;
        if (small < 0 || a < (long)r[small].w * r[small].h)
            small = i;
    }
    return best >= 0 ? best : small;
}

/* The last video's res, kept a minute (the details page asks how it will
   play each time it's laid out) */
static struct {
    char udn[96], id[200];
    long at;
    res_t r[RES_MAX];
    int n;
} last;

int dlna_play(const plex_ctx *c0, const plex_item *it, const caps_t *k, int allow_direct, long offset_s,
              const char *session, play_t *out)
{
    plex_ctx cc = *c0, *c = &cc;
    plex_item t;
    char why[160], id[400];
    int orig, conv, ok;
    (void)offset_s;                 /* the player seeks in the file */
    memset(out, 0, sizeof(*out));
    if (it->kind != PI_VIDEO || !it->rating_key) {
        snprintf(out->why, sizeof(out->why), "that isn't a video");
        return -1;
    }
    if (strcmp(last.udn, c->server_id) || strcmp(last.id, it->rating_key) || ms_now() - last.at > 60000 ||
        ms_now() < last.at) {
        char args[1400], eid[700];
        xml_node *resp;
        const xml_node *r, *e;
        xml_node *didl;
        unescape(it->rating_key, id, sizeof(id));
        xml_escape(id, eid, sizeof(eid));
        snprintf(args, sizeof(args),
                 "<ObjectID>%s</ObjectID><BrowseFlag>BrowseMetadata</BrowseFlag><Filter>*</Filter>"
                 "<StartingIndex>0</StartingIndex><RequestedCount>1</RequestedCount><SortCriteria></SortCriteria>", eid);
        if (!(resp = soap(c, "Browse", args))) {
            snprintf(out->why, sizeof(out->why), "Can't ask the server about it: %s", c->err);
            return -1;
        }
        r = xml_find(resp, "BrowseResponse");
        didl = r && xml_text(r, "Result") ? xml_parse(xml_text(r, "Result")) : NULL;
        xml_free(resp);
        for (e = didl ? didl->child : NULL; e && strcmp(e->name, "item"); e = e->next)
            ;
        memset(&last, 0, sizeof(last));
        if (e)
            last.n = res_all(c, e, last.r, RES_MAX);
        xml_free(didl);
        if (!last.n) {
            snprintf(out->why, sizeof(out->why), "The server gives no file for it");
            return -1;
        }
        snprintf(last.udn, sizeof(last.udn), "%s", c->server_id);
        snprintf(last.id, sizeof(last.id), "%s", it->rating_key);
        last.at = ms_now();
    }
    orig = res_original(last.r, last.n);
    conv = res_converted(last.r, last.n, orig, k);
    memset(&t, 0, sizeof(t));
    t.kind = PI_VIDEO;
    t.title = it->title;
    res_into(&t, &last.r[orig]);
    if (session && *session)
        snprintf(out->session, sizeof(out->session), "%s", session);
    else
        caps_session_id(c, out->session, sizeof(out->session));
    if (!allow_direct)
        snprintf(why, sizeof(why), "direct play is off");
    ok = allow_direct && direct_ok(k, &t, why, sizeof(why));
    if (ok || conv < 0) {           /* the file itself, even if not ideal, when there's nothing else */
        out->direct = 1;
        snprintf(out->url, sizeof(out->url), "%s", last.r[orig].url);
        if (ok)
            snprintf(out->why, sizeof(out->why), "Direct Play: %s", why);
        else
            snprintf(out->why, sizeof(out->why), "Direct Play (%s, but the server offers no converted stream)", why);
        snprintf(out->key, sizeof(out->key), "dlna:%s/%s", c->server_id, it->rating_key);
    } else {
        const res_t *r = &last.r[conv];
        snprintf(out->url, sizeof(out->url), "%s", r->url);
        if (r->w > 0)
            snprintf(out->why, sizeof(out->why), "Transcoded by the server (%s): %dx%d", why, r->w, r->h);
        else
            snprintf(out->why, sizeof(out->why), "Transcoded by the server (%s)", why);
    }
    free(t.container); free(t.vcodec); free(t.acodec); free(t.part_key); free(t.part_file);
    return 0;
}
