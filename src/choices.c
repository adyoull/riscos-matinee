/*
 * choices.c - Matinee's Choices: read from Choices:Matinee.Choices and written to
 * <Choices$Write>.Matinee (Matinee$ChoicesDir for the host test).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- Choices ------------------------------------------------------------ */

const char *player_names[PLAYER_COUNT] = { "Built-in", "ReelEGL", "Reel" };
const char *pic_names[PIC_COUNT] = { "Fit", "Fill (crop)", "Stretch" };

static FILE *choices_open(int write)
{
    char path[300];
    const char *dir = getenv(APP "$ChoicesDir");
    if (dir && *dir) {
        snprintf(path, sizeof(path), "%s/Choices", dir);
    } else if (write) {
        make_dir("<Choices$Write>." APP);
        snprintf(path, sizeof(path), "<Choices$Write>." APP ".Choices");
    } else {
        FILE *f;
        snprintf(path, sizeof(path), "Choices:" APP ".Choices");
        if ((f = fopen(path, "r")) != NULL)
            return f;
        /* the test builds were called PlexRO: their sign-in and choices
           carry over (written as Matinee's from then on) */
        snprintf(path, sizeof(path), "Choices:PlexRO.Choices");
    }
    return fopen(path, write ? "w" : "r");
}

/* A string fit for one Choices line: no line ends (a server's name could
   have one, and the next line would be read as another key), and no '|' in
   a Jellyfin server's fields */
static const char *cv(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; s && *s && n + 1 < size; s++)
        out[n++] = *s == '\n' || *s == '\r' || *s == '|' ? ' ' : *s;
    out[n] = 0;
    return out;
}

void choices_save(void)
{
    FILE *f = choices_open(1);
    static char c[12][300];         /* each string's line-safe copy */
    int mw, mr, mb;
    if (!f)
        return;
    player_mini_place(&mw, &mr, &mb);
    fprintf(f, "# " APP " choices\n"
            "client_id %s\naccount_token %s\nserver_base %s\nserver_token %s\n"
            "server_name %s\nserver_id %s\nserver_local %d\nplayer %s\nquality %d\ndirect_play %d\n"
            "poster_size %d\nchoices_version 2\nhardware_overlay %d\npicture %d\nvolume %d\nimage_cache_mb %d\n"
            "keep_on_top %d\nmini_width %d\nmini_right %d\nmini_bottom %d\nuser_uuid %s\nuser_title %s\n",
            cv(S.px.client_id, c[0], 300), cv(S.px.account_token, c[1], 300), cv(S.px.base, c[2], 300),
            cv(S.px.token, c[3], 300), cv(S.px.server_name, c[4], 300), cv(S.px.server_id, c[5], 300), S.px.local,
            player_names[S.player], S.quality, S.direct,
            S.psize, S.overlay, S.pic_mode, (int)(S.volume * 100 + 0.5), S.cache_mb,
            player_ontop(), mw, mr, mb, cv(S.user_uuid, c[6], 300), cv(S.user_title, c[7], 300));
    /* the server in use is Plex's or Jellyfin's; and the Jellyfin servers
       signed in to, a line each: name|id|address|token|user id|user name */
    fprintf(f, "server_kind %d\nserver_user_id %s\nserver_user_name %s\n", S.px.kind, cv(S.px.user_id, c[0], 300),
            cv(S.px.user_name, c[1], 300));
    for (int i = 0; i < S.njf; i++)
        fprintf(f, "jellyfin %s|%s|%s|%s|%s|%s\n", cv(S.jf[i].name, c[0], 300), cv(S.jf[i].id, c[1], 300),
                cv(S.jf[i].base, c[2], 300), cv(S.jf[i].token, c[3], 300), cv(S.jf[i].uid, c[4], 300),
                cv(S.jf[i].uname, c[5], 300));
    /* and the DLNA servers used: name|UDN|description's address */
    for (int i = 0; i < S.ndl; i++)
        fprintf(f, "dlna %s|%s|%s\n", cv(S.dl[i].name, c[0], 300), cv(S.dl[i].udn, c[1], 300),
                cv(S.dl[i].loc, c[2], 300));
    fclose(f);
}

/* The rest of "key value" as a string: 1 if line was that key */
static int value(const char *line, const char *key, char *out, size_t size)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) || (line[n] != ' ' && line[n] != '\n' && line[n] != 0))
        return 0;
    line += n;
    if (*line == ' ')
        line++;
    snprintf(out, size, "%s", line);
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}

/* A DLNA server's Choices line (name|UDN|description's address) */
static void dl_saved_parse(const char *v)
{
    dl_saved *d;
    char t[700], *f[3], *p = t;
    int n = 0;
    if (S.ndl >= DL_SAVED)
        return;
    snprintf(t, sizeof(t), "%s", v);
    while (n < 3) {
        f[n++] = p;
        p = strchr(p, '|');
        if (!p)
            break;
        *p++ = 0;
    }
    if (n < 3 || !*f[1] || !*f[2])
        return;
    d = &S.dl[S.ndl++];
    snprintf(d->name, sizeof(d->name), "%s", f[0]);
    snprintf(d->udn, sizeof(d->udn), "%s", f[1]);
    snprintf(d->loc, sizeof(d->loc), "%s", f[2]);
}

/* A Jellyfin server's Choices line (name|id|address|token|user id|user name) */
static void jf_saved_parse(const char *v)
{
    jf_saved *j;
    char t[700], *f[6], *p = t;
    int n = 0;
    if (S.njf >= JF_SAVED)
        return;
    snprintf(t, sizeof(t), "%s", v);
    while (n < 6) {
        f[n++] = p;
        p = strchr(p, '|');
        if (!p)
            break;
        *p++ = 0;
    }
    if (n < 6 || !*f[2] || !*f[3])
        return;
    j = &S.jf[S.njf++];
    snprintf(j->name, sizeof(j->name), "%s", f[0]);
    snprintf(j->id, sizeof(j->id), "%s", f[1]);
    snprintf(j->base, sizeof(j->base), "%s", f[2]);
    snprintf(j->token, sizeof(j->token), "%s", f[3]);
    snprintf(j->uid, sizeof(j->uid), "%s", f[4]);
    snprintf(j->uname, sizeof(j->uname), "%s", f[5]);
}

void choices_load(void)
{
    char line[800], v[700], id[48] = "";
    FILE *f = choices_open(0);
    plex_ctx *c = &S.px;
    int version = 1, player = PLAYER_BUILTIN;
    S.player = PLAYER_BUILTIN;      /* the built-in player by default; ReelEGL and Reel the others */
    S.quality = Q_720;
    S.direct = 1;
    S.psize = 1;                    /* Medium */
    S.nusers = -1;                  /* Plex Home: not asked yet */
    S.overlay = 1;
    S.pic_mode = PIC_FIT;
    S.volume = 1;
    S.cache_mb = 64;
    {
        int mw = 0, mr = -2, mb = -2, top = 0;
        player_set_mini_place(0, -1, -1);           /* Reel's defaults: bottom right, above the icon bar */
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (value(line, "mini_width", v, sizeof(v))) mw = atoi(v);
                else if (value(line, "mini_right", v, sizeof(v))) mr = atoi(v);
                else if (value(line, "mini_bottom", v, sizeof(v))) mb = atoi(v);
                else if (value(line, "keep_on_top", v, sizeof(v))) top = atoi(v) != 0;
            }
            rewind(f);
            player_set_mini_place(mw, mr, mb);
        }
        player_set_ontop(top);
    }
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (value(line, "client_id", v, sizeof(v)))
                snprintf(id, sizeof(id), "%s", v);
            else if (value(line, "choices_version", v, sizeof(v)))
                version = atoi(v);
            else if (value(line, "player", v, sizeof(v)))
                player = !strcmp(v, "Reel") ? PLAYER_REEL : !strcmp(v, "ReelEGL") ? PLAYER_REELEGL : PLAYER_BUILTIN;
            else if (value(line, "hardware_overlay", v, sizeof(v)))
                S.overlay = atoi(v) != 0;
            else if (value(line, "picture", v, sizeof(v)))
                S.pic_mode = atoi(v) >= 0 && atoi(v) < PIC_COUNT ? atoi(v) : PIC_FIT;
            else if (value(line, "image_cache_mb", v, sizeof(v)))
                S.cache_mb = atoi(v) >= 0 ? atoi(v) : 64;
            else if (value(line, "volume", v, sizeof(v)))
                S.volume = atoi(v) >= 0 && atoi(v) <= 100 ? atoi(v) / 100.0 : 1;
            else if (value(line, "quality", v, sizeof(v)))
                S.quality = atoi(v) >= 0 && atoi(v) < Q_COUNT ? atoi(v) : Q_720;
            else if (value(line, "direct_play", v, sizeof(v)))
                S.direct = atoi(v) != 0;
            else if (value(line, "poster_size", v, sizeof(v)))
                S.psize = atoi(v) >= 0 && atoi(v) <= 2 ? atoi(v) : 1;
        }
    }
    /* test builds before the built-in player saved ReelEGL as the default,
       not as a choice: they start with the built-in player once */
    if (version >= 2)
        S.player = player;
    if (!*id) {                     /* made once: the server knows us by it */
        unsigned a = (unsigned)time(NULL), b = (unsigned)now_cs() * 2654435761u ^ (unsigned)clock();
        snprintf(id, sizeof(id), "matinee-%08x%08x", a, b);
    }
    plex_ctx_init(c, id, MATINEE_VERSION);
    {
        /* the name the server shows for this computer (the dashboard, the
           other apps' "play on"): the network's host name, if it has one */
        const char *h = getenv("Inet$HostName");
        if (h && *h)
            snprintf(c->device_name, sizeof(c->device_name), "%s", h);
    }
    if (f) {
        rewind(f);
        while (fgets(line, sizeof(line), f)) {
            if (value(line, "account_token", v, sizeof(v)))
                snprintf(c->account_token, sizeof(c->account_token), "%s", v);
            else if (value(line, "user_uuid", v, sizeof(v)))
                snprintf(S.user_uuid, sizeof(S.user_uuid), "%s", v);
            else if (value(line, "user_title", v, sizeof(v)))
                snprintf(S.user_title, sizeof(S.user_title), "%s", v);
            else if (value(line, "server_base", v, sizeof(v)))
                snprintf(c->base, sizeof(c->base), "%s", v);
            else if (value(line, "server_token", v, sizeof(v)))
                snprintf(c->token, sizeof(c->token), "%s", v);
            else if (value(line, "server_name", v, sizeof(v)))
                snprintf(c->server_name, sizeof(c->server_name), "%s", v);
            else if (value(line, "server_id", v, sizeof(v)))
                snprintf(c->server_id, sizeof(c->server_id), "%s", v);
            else if (value(line, "server_local", v, sizeof(v)))
                c->local = atoi(v) != 0;
            else if (value(line, "server_kind", v, sizeof(v)))
                c->kind = atoi(v) == SRV_JELLYFIN ? SRV_JELLYFIN : atoi(v) == SRV_DLNA ? SRV_DLNA : SRV_PLEX;
            else if (value(line, "server_user_id", v, sizeof(v)))
                snprintf(c->user_id, sizeof(c->user_id), "%s", v);
            else if (value(line, "server_user_name", v, sizeof(v)))
                snprintf(c->user_name, sizeof(c->user_name), "%s", v);
            else if (value(line, "jellyfin", v, sizeof(v)))
                jf_saved_parse(v);
            else if (value(line, "dlna", v, sizeof(v)))
                dl_saved_parse(v);
        }
        fclose(f);
    }
    {
        const char *tv = getenv(APP "$PlexTV");     /* another plex.tv (the tests' fake) */
        if (tv && *tv)
            snprintf(c->plextv, sizeof(c->plextv), "%s", tv);
    }
    {
        /* DLNA: where you got to in its videos, kept beside Choices; and
           where to look for servers (Matinee$SSDP addr:port: the tests' fake) */
        const char *dir = getenv(APP "$ChoicesDir"), *ssdp = getenv(APP "$SSDP");
        char r[300], w[300];
        if (dir && *dir) {
            snprintf(r, sizeof(r), "%s/Places", dir);
            snprintf(w, sizeof(w), "%s/Places", dir);
        } else {
            snprintf(r, sizeof(r), "Choices:" APP ".Places");
            snprintf(w, sizeof(w), "<Choices$Write>." APP ".Places");
        }
        dlna_places_files(r, w);
        if (ssdp && *ssdp) {
            char a[64], *colon;
            snprintf(a, sizeof(a), "%s", ssdp);
            colon = strchr(a, ':');
            if (colon)
                *colon++ = 0;
            dlna_set_ssdp(a, colon ? atoi(colon) : 0);
        }
    }
}
