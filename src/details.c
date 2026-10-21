/*
 * details.c - the details page: a video's backdrop, title, labels, summary,
 * buttons, credits, cast, and the rows of other videos under them.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

static const int rel_order[REL_ROWS] = { REL_COLLECTION, REL_SIMILAR, REL_EXTRAS };

/* ---- the details page -------------------------------------------------------- */

/* A video's details in the browser window, in place of the grid: the
   backdrop across the top (under the bar), faded into the window, the
   title over its foot, then the buttons, subtitles, how it will play and
   the summary. Back (the button, Backspace or Escape) returns to the grid
   where it was. */

static int det_how_y, det_sum_y;                /* baselines, work area */
static int det_cred_y, det_cast_y;              /* the credits' first baseline; the cast heading's */
#define CAST_W  200                             /* a cast tile: the photo, the name and the part */
#define CAST_PH 144                             /* the photo's diameter */
#define CAST_H  (CAST_PH + 100)
#define CRED_X  300                             /* the credits' values, after their labels */
int det_wd = 1000, art_w, art_h;         /* the width laid out for; the backdrop's box */
static int art_fw, art_fh;                     /* the size fetched */
int art_due;                                    /* when to fetch it at the new size (0: not) */
static int det_pw, det_ph, det_pfw;             /* the poster's box (OS units), the width fetched */
static int det_tx, det_title_y, det_bx, det_stars_x, det_stars_y;       /* the text column's left; the title's baseline; buttons' left */
static int det_cred_x;                          /* the credits' labels' left */

const plex_item *det_item(void)
{
    return S.have_det && S.det.n ? &S.det.v[0] : NULL;
}

/* Is it the video the details are of? */
int det_is(const plex_item *it)
{
    const plex_item *d = det_item();
    return d && it && it->rating_key && d->rating_key && !strcmp(d->rating_key, it->rating_key);
}

/* The width the window shows */
int vis_width(void)
{
    int st[9];
    if (!S.browser_open)            /* the width it opens at (browser_open) */
        return (S.scr_w * WIN_PERCENT / 100 - SCROLL_W) & ~((1 << S.xeig) - 1);
    window_state(S.browser_w, st);
    return st[3] - st[1];
}

/* The backdrop's box: the window's width, 16:9, but at most 55% of the
   window's height (it grows and shrinks with the window) */
void det_art_box(void)
{
    int st[9], most = 720;
    if (S.browser_open) {
        window_state(S.browser_w, st);
        most = (st[4] - st[2]) * 55 / 100;
    }
    if (most < 360)
        most = 360;
    art_w = det_wd;
    art_h = art_w * 9 / 16;
    if (art_h > most)
        art_h = most;
    art_w &= ~((1 << S.xeig) - 1);
    art_h &= ~((1 << S.yeig) - 1);
    /* the poster on the left: 17% of the width, 2:3 */
    det_pw = det_wd * 17 / 100;
    if (det_pw < 232)
        det_pw = 232;
    if (det_pw > 480)
        det_pw = 480;
    det_pw &= ~((2 << S.xeig) - 1);
    det_ph = (det_pw * 3 / 2) & ~((1 << S.yeig) - 1);
}

/* The poster beside the title: a film's, or an episode's show's */
static const char *det_poster_thumb(const plex_item *it)
{
    return it->type && !strcmp(it->type, "episode") && it->show_thumb ? it->show_thumb : it->thumb;
}

/* What's still to come for the page shown, after it's shown (from null
   events, det_cast_step): the poster and the backdrop (as pictures do,
   pictures.c), and the rows' lists, each a request to the server that the
   desktop used to wait for before the page appeared. One list at a time,
   without waiting (bgreq.c) from a Plex server at an address; fetched the
   old way (still after the page is shown) otherwise. */
static int art_wanted;                          /* the poster or the backdrop still to come */
static struct {
    char path[REL_ROWS][260];
    int due[REL_ROWS];                          /* still to fetch */
    bgreq r;
    int on, k, cs0;                             /* a list on its way: which; when it was asked for */
} RQ = { .r = { .fd = -1 } };
#define RQ_TIMEOUT_CS 1500

static void rows_cancel(void)
{
    bgreq_close(&RQ.r);
    RQ.on = 0;
    for (int k = 0; k < REL_ROWS; k++)
        RQ.due[k] = 0;
}

/* A row's list on its way (the Wimp can sleep a little meanwhile) */
int det_waiting(void)
{
    return RQ.on;
}

/* The poster's and the backdrop's keys in the cache, at the sizes fetched */
static void art_keys(const plex_item *it, char *pkey, char *akey, size_t size)
{
    snprintf(pkey, size, "dposter:%s@%dx%d", det_poster_thumb(it) ? det_poster_thumb(it) : "", det_pw, det_ph);
    snprintf(akey, size, "art:%s@%dx%d", it->art ? it->art : "", art_fw, art_fh);
}

/* The backdrop and the poster, for the boxes laid out for. wait 1: fetched
   now (the desktop waiting); 0: those not in the cache come later
   (det_art_step) */
void det_fetch_art(int wait)
{
    const plex_item *it = det_item();
    char pkey[320], akey[320];
    S.det_art = S.det_poster = NULL;
    det_art_box();
    art_fw = art_w;
    art_fh = art_h;
    det_pfw = det_pw;
    art_due = 0;
    art_wanted = 0;
    if (!it)
        return;
    art_keys(it, pkey, akey, sizeof(pkey));
    if (det_poster_thumb(it) && !(S.det_poster = cache_find(pkey))) {
        if (wait)
            S.det_poster = poster_fetch(det_poster_thumb(it), pkey, det_pw >> S.xeig, det_ph >> S.yeig, 0);
        else
            art_wanted = 1;
    }
    if (it->art && !(S.det_art = cache_find(akey))) {
        if (wait)
            S.det_art = poster_fetch(it->art, akey, art_w >> S.xeig, art_h >> S.yeig, 1);
        else
            art_wanted = 1;
    }
    if (art_wanted)
        S.cast_wanted = 1;
    cache_trim();
}

/* The poster or the backdrop moved on: 1 if one is on its way (or came) */
static int det_art_step(const plex_item *it)
{
    char pkey[320], akey[320];
    if (!art_wanted)
        return 0;
    art_keys(it, pkey, akey, sizeof(pkey));
    if (det_poster_thumb(it) && !S.det_poster) {
        if (!(S.det_poster = poster_soon(det_poster_thumb(it), pkey, det_pw >> S.xeig, det_ph >> S.yeig, 0)))
            return 1;
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
        return 1;
    }
    if (it->art && !S.det_art) {
        if (!(S.det_art = poster_soon(it->art, akey, art_fw >> S.xeig, art_fh >> S.yeig, 1)))
            return 1;
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
        return 1;
    }
    art_wanted = 0;
    cache_trim();
    return 0;
}

/* Row k's list has come (or failed: an empty row): laid out and shown */
static void rows_got(int k, plex_list *l, int cs, int waited)
{
    const plex_item *v = det_item();
    if (k == REL_COLLECTION && v && v->rating_key) {    /* the others in it: not this one */
        for (int i = 0; i < l->n; i++)
            if (l->v[i].rating_key && !strcmp(l->v[i].rating_key, v->rating_key)) {
                plex_item_free(&l->v[i]);
                memmove(&l->v[i], &l->v[i + 1], (l->n - i - 1) * sizeof(plex_item));
                l->n--;
                i--;
            }
    }
    S.rel[k] = *l;
    player_log("details: %s: %d items, %d cs%s", S.rel_head[k], l->n, cs, waited ? ", the desktop waiting" : ", without waiting");
    det_repaint();
}

/* The rows' lists moved on: 1 if one is on its way (or came) */
static int det_rows_step(void)
{
    plex_request rq;
    plex_list l;
    if (RQ.on) {
        int e = bgreq_step(&RQ.r), k = RQ.k;
        char *body;
        size_t len;
        if (e == 0 && now_cs() - RQ.cs0 < RQ_TIMEOUT_CS)
            return 1;
        body = e > 0 && RQ.r.status == 200 ? bgreq_take(&RQ.r, &len) : NULL;
        bgreq_close(&RQ.r);
        RQ.on = 0;
        if (!body || plex_list_parse(body, RQ.path[k], &l) != 0)
            memset(&l, 0, sizeof(l));
        free(body);
        rows_got(k, &l, now_cs() - RQ.cs0, 0);
        return 1;
    }
    for (int o = 0; o < REL_ROWS; o++) {            /* in the order they're shown */
        int k = rel_order[o], t0;
        if (!RQ.due[k])
            continue;
        RQ.due[k] = 0;
        if (plex_list_request(&S.px, RQ.path[k], &rq) == 0 && bgreq_start(&RQ.r, rq.url, rq.headers, 1) == 0) {
            RQ.on = 1;
            RQ.k = k;
            RQ.cs0 = now_cs();
            return 1;
        }
        t0 = now_cs();
        if (plex_list_get(&S.px, RQ.path[k], &l) != 0)
            memset(&l, 0, sizeof(l));
        rows_got(k, &l, now_cs() - t0, 1);
        return 1;
    }
    return 0;
}

#ifdef MATINEE_TEST
/* The tests' Wimp: a row's list on its way arrives while the program sleeps */
void ui_test_rows_settle(void)
{
    for (int i = 0; RQ.on && i < 3000; i++) {
        struct timespec ts = { 0, 1000000 };
        if (det_rows_step() && !RQ.on)
            break;
        nanosleep(&ts, NULL);
    }
}
#endif

/* One video's details, from the server, into S.det; its rows' lists and
   (with_art) its poster and backdrop come after (det_cast_step). The same
   video again (a choice changed) keeps its rows. 0 = ok. */
int det_fetch(const plex_item *it, int with_art)
{
    plex_list d;
    int same, t0 = now_cs();
    hourglass(1);
    if (plex_details(&S.px, it, &d) != 0) {
        hourglass(0);
        report("Can't get the details: %s", S.px.err);
        return -1;
    }
    hourglass(0);
    player_log("details: %d cs, the desktop waiting", now_cs() - t0);
    same = S.have_det && S.det.n && d.n && S.det.v[0].rating_key && d.v[0].rating_key &&
           !strcmp(S.det.v[0].rating_key, d.v[0].rating_key);
    if (S.have_det)
        plex_list_free(&S.det);
    S.det = d;
    S.have_det = 1;
    S.det_art = S.det_poster = NULL;
    art_wanted = 0;
    for (int i = 0; i < (int)(sizeof(S.cast) / sizeof(S.cast[0])); i++)
        S.cast[i].photo = NULL;
    S.ncast = 0;
    S.cast_wanted = 1;              /* laid out by det_layout, fetched from null events */
    if (!same) {
        rows_cancel();
        for (int k = 0; k < REL_ROWS; k++) {    /* More like this (not for an episode), its extras, its collection */
            const plex_item *v = &S.det.v[0];
            if (S.rel[k].n || S.rel[k].v)
                plex_list_free(&S.rel[k]);
            memset(&S.rel[k], 0, sizeof(S.rel[k]));
            S.nrel[k] = 0;
            S.rel_head[k][0] = 0;
            if (!v->rating_key || (k == REL_SIMILAR && v->type && !strcmp(v->type, "episode")))
                continue;
            if (k == REL_COLLECTION) {
                if (!v->collection || !v->collection_key)
                    continue;
                snprintf(RQ.path[k], sizeof(RQ.path[k]), "%s", v->collection_key);
                latin1(v->collection, S.rel_head[k], sizeof(S.rel_head[k]));
            } else {
                snprintf(RQ.path[k], sizeof(RQ.path[k]), "/library/metadata/%s/%s", v->rating_key,
                         k == REL_EXTRAS ? "extras" : "similar");
                snprintf(S.rel_head[k], sizeof(S.rel_head[k]), "%s",
                         k == REL_EXTRAS ? "Trailers and extras" : "More like this");
            }
            RQ.due[k] = 1;
        }
    }
    if (with_art)
        det_fetch_art(0);
    return 0;
}

static void det_button(int id, const char *label, int *x, int *y)
{
    int w = draw_width(D_BOLD, label) + 64 + (id == D_PLAY ? 36 : id == D_SUBS ? 40 : 0);
    if (S.nbtn >= DET_BTN_MAX)
        return;
    if (*x + w > det_wd - 40 && *x > det_bx) {  /* a new row */
        *x = det_bx;
        *y -= BTN + 20;
    }
    S.btn[S.nbtn].id = id;
    S.btn[S.nbtn].x0 = *x;
    S.btn[S.nbtn].x1 = *x + w;
    S.btn[S.nbtn].y1 = *y;
    S.btn[S.nbtn].y0 = *y - BTN;
    snprintf(S.btn[S.nbtn].label, sizeof(S.btn[0].label), "%s", label);
    S.nbtn++;
    *x += w + 20;
}

/* "2008-04-10" as "10 April 2008" */
static void nice_date(const char *iso, char *out, size_t size)
{
    static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
    int y = 0, m = 0, d = 0;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12)
        snprintf(out, size, "%d %s %d", d, months[m - 1], y);
    else
        snprintf(out, size, "%s", iso);
}

/* The file, in a line: "1080p H.264 · AAC 5.1 · MKV · 4.2 GB" (UTF-8) */
static void tech_line(const plex_item *it, char *out, size_t size)
{
    static const char dot[] = "   \xc2\xb7   ";
    char res[16] = "", v[24] = "", a[32] = "", ct[16] = "";
    size_t n;
    if (it->height >= 2000) snprintf(res, sizeof(res), "4K ");
    else if (it->height >= 1000) snprintf(res, sizeof(res), "1080p ");
    else if (it->height >= 700) snprintf(res, sizeof(res), "720p ");
    else if (it->height > 0) snprintf(res, sizeof(res), "%dp ", it->height);
    if (it->vcodec)
        snprintf(v, sizeof(v), "%s", !strcmp(it->vcodec, "h264") ? "H.264" : !strcmp(it->vcodec, "hevc") ? "HEVC" :
                 !strcmp(it->vcodec, "mpeg2video") ? "MPEG-2" : it->vcodec);
    if (it->acodec)
        snprintf(a, sizeof(a), "%s%s", it->acodec, it->channels == 6 ? " 5.1" : it->channels == 8 ? " 7.1" :
                 it->channels == 2 ? " stereo" : it->channels == 1 ? " mono" : "");
    for (char *p = a; *p && *p != ' '; p++)             /* codec names in capitals */
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 32);
    snprintf(ct, sizeof(ct), "%s", it->container ? it->container : "");
    for (char *p = ct; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 32);
    n = snprintf(out, size, "%s%s", res, v);
    if (*a && n < size)
        n += snprintf(out + n, size - n, "%s%s", n ? dot : "", a);
    if (*ct && n < size)
        n += snprintf(out + n, size - n, "%s%s", n ? dot : "", ct);
    if (it->part_size > 0 && n < size)
        snprintf(out + n, size - n, "%s%.1f GB", n ? dot : "", it->part_size / 1e9);
}

/* The credits and the cast, under the summary from y; the page's height */
/* Under the title, the summary and the buttons: the cast (photos, names,
   parts) and the credits (who made it, when, the file). Side by side in a
   wide window, the credits on the right; else the credits first. */
static void det_layout_more(const plex_item *it, int y)
{
    char t[400], v[400];
    int n = 0, cols, wide = det_wd >= 1700, cast_w, cred_w, cast_top;
    cred_w = wide ? det_wd - det_wd * 56 / 100 - 40 : det_wd - 80;
    cast_w = wide ? det_wd * 56 / 100 - 80 : det_wd - 80;
#define CRED(label, ...) do { if (n < 8) { \
        snprintf(S.det_cred[n][0], sizeof(S.det_cred[0][0]), "%s", label); \
        snprintf(t, sizeof(t), __VA_ARGS__); latin1(t, v, sizeof(v)); \
        draw_fit(D_BODY, v, cred_w - CRED_X); \
        snprintf(S.det_cred[n][1], sizeof(S.det_cred[0][1]), "%s", v); n++; } } while (0)
    if (it->directors) CRED("Directed by", "%s", it->directors);
    if (it->writers) CRED("Written by", "%s", it->writers);
    if (it->studio) CRED("Studio", "%s", it->studio);
    if (it->released) {
        nice_date(it->released, v, sizeof(v));
        CRED(it->type && !strcmp(it->type, "episode") ? "First shown" : "Released", "%s", v);
    }
    if (it->country) CRED("Country", "%s", it->country);
    tech_line(it, v, sizeof(v));
    if (*v) {
        char l1[400];
        snprintf(l1, sizeof(l1), "%s", v);
        CRED("File", "%s", l1);
    }
#undef CRED
    S.det_ncred = n;
    if (wide) {                     /* the credits beside the cast */
        det_cred_x = det_wd * 56 / 100;
        det_cred_y = y - 36;
        cast_top = y;
    } else {
        det_cred_x = 40;
        det_cred_y = y - 12;
        y -= n * 40 + (n ? 40 : 0);
        cast_top = y;
    }
    /* the cast: rows of photos with the name and the part under them */
    y = cast_top;
    S.ncast = it->ncast < (int)(sizeof(S.cast) / sizeof(S.cast[0])) ? it->ncast : (int)(sizeof(S.cast) / sizeof(S.cast[0]));
    for (int k = 0; k < REL_ROWS; k++)
        for (int i = 0; i < 16; i++)
            S.relc[k][i].pic = NULL;
    det_cast_y = 0;
    if (S.ncast) {
        det_cast_y = y - 36;
        y -= 72;
        cols = (cast_w + 24) / (CAST_W + 24);
        if (cols < 1)
            cols = 1;
        for (int i = 0; i < S.ncast; i++) {
            const plex_person *p = &it->cast[i];
            S.cast[i].x0 = 40 + (i % cols) * (CAST_W + 24);
            S.cast[i].y0 = y - (i / cols) * (CAST_H + 24) - CAST_H;
            latin1(p->name, S.cast[i].name, sizeof(S.cast[i].name));
            latin1(p->role ? p->role : "", S.cast[i].role, sizeof(S.cast[i].role));
            draw_fit(D_BODY, S.cast[i].name, CAST_W);
            draw_fit(D_BODY, S.cast[i].role, CAST_W);
        }
        y -= ((S.ncast + cols - 1) / cols) * (CAST_H + 24);
    }
    if (wide && det_cred_y - n * 40 - 24 < y)
        y = det_cred_y - n * 40 - 24;
    /* the collection, More like this, extras: a row each, as many as fit */
    for (int o = 0; o < REL_ROWS; o++) {
        int k = rel_order[o];
        int cw = k == REL_EXTRAS ? EP_TW : TILE_W, ch = k == REL_EXTRAS ? EP_TH : POSTER_H,
            fit = (det_wd - 80 + GAP) / (cw + GAP);
        S.nrel[k] = S.rel[k].n < fit ? S.rel[k].n : fit;
        if (S.nrel[k] > 16)
            S.nrel[k] = 16;
        if (!S.nrel[k])
            continue;
        S.rel_y[k] = y - 56;
        y -= 84;
        for (int i = 0; i < S.nrel[k]; i++) {
            const plex_item *r = &S.rel[k].v[i];
            S.relc[k][i].x0 = 40 + i * (cw + GAP);
            S.relc[k][i].x1 = S.relc[k][i].x0 + cw;
            S.relc[k][i].y1 = y;
            S.relc[k][i].y0 = y - ch;
            latin1(r->title, S.relc[k][i].line[0], sizeof(S.relc[k][i].line[0]));
            latin1(r->subtitle ? r->subtitle : "", S.relc[k][i].line[1], sizeof(S.relc[k][i].line[1]));
            draw_fit(D_BOLD, S.relc[k][i].line[0], cw);
            draw_fit(D_BODY, S.relc[k][i].line[1], cw);
        }
        y -= ch + 96 + 40;
        S.rel_wanted = 1;
    }
    S.det_h = -y + 40;
}

/* What's still to come for the page shown (one thing each null event): the
   poster and backdrop, the rows' lists, the cast's photos, the rows'
   pictures. 1 if one is on its way (or came). */
int det_cast_step(void)
{
    const plex_item *it = det_item();
    if (S.page != PG_DETAILS || !S.browser_open || !it) {
        S.cast_wanted = 0;
        return 0;
    }
    if (det_art_step(it) || det_rows_step())
        return 1;
    for (int i = 0; i < S.ncast && i < it->ncast; i++) {
        const char *th = it->cast[i].thumb;
        char key[320];
        int px = CAST_PH >> S.xeig;
        if (!th || S.cast[i].photo)
            continue;
        snprintf(key, sizeof(key), "cast:%s@%d", th, px);
        if (!(S.cast[i].photo = cache_find(key))) {
            if (!(S.cast[i].photo = poster_soon(th, key, px, CAST_PH >> S.yeig, 2)))
                return 1;           /* on its way */
            cache_trim();
        }
        force_redraw(S.browser_w, S.cast[i].x0, S.cast[i].y0, S.cast[i].x0 + CAST_W, S.cast[i].y0 + CAST_H);
        return 1;
    }
    for (int o = 0; o < REL_ROWS; o++)     /* the collection, More like this, extras */
        for (int k = rel_order[o], i = 0; i < S.nrel[k]; i++) {
            const plex_item *r = &S.rel[k].v[i];
            char key[320];
            int cw = S.relc[k][i].x1 - S.relc[k][i].x0, ch = S.relc[k][i].y1 - S.relc[k][i].y0;
            if (!r->thumb || S.relc[k][i].pic)
                continue;
            if (k == REL_EXTRAS)
                snprintf(key, sizeof(key), "ep:%s@%dx%d", r->thumb, cw, ch);
            else
                snprintf(key, sizeof(key), "%s", r->thumb);     /* as on the grid */
            if (!(S.relc[k][i].pic = cache_find(key))) {
                if (!(S.relc[k][i].pic = poster_soon(r->thumb, key, cw >> S.xeig, ch >> S.yeig, k == REL_EXTRAS ? 3 : 0)))
                    return 1;       /* on its way */
                cache_trim();
            }
            force_redraw(S.browser_w, S.relc[k][i].x0 - 8, S.relc[k][i].y0 - 8, S.relc[k][i].x1 + 8, S.relc[k][i].y1 + 8);
            return 1;
        }
    S.cast_wanted = 0;
    return 0;
}

/* A label under the title: kind 0 a fact (year, running time...), 1 a genre */
static void det_chip(int kind, const char *text, int *x, int *y)
{
    int w = draw_width(kind ? D_BODY : D_BOLD, text) + 44;
    if (S.nchip >= (int)(sizeof(S.chip) / sizeof(S.chip[0])))
        return;
    if (*x + w > det_wd - 40 && *x > det_tx) {
        *x = det_tx;
        *y -= 68;
    }
    S.chip[S.nchip].x0 = *x;
    S.chip[S.nchip].x1 = *x + w;
    S.chip[S.nchip].y0 = *y - 56;
    S.chip[S.nchip].kind = kind;
    snprintf(S.chip[S.nchip].t, sizeof(S.chip[0].t), "%s", text);
    S.nchip++;
    *x += w + 16;
}

/* The texts and buttons, where they go, and the page's height. The
   backdrop across the top (faded into the page from the left and at its
   foot), the poster on the left over it, and beside the poster: the title,
   labels for the year, running time, age rating and scores, the genres,
   the summary, the buttons and how it will play. Then the cast and the
   credits. */
static void det_layout(void)
{
    const plex_item *it = det_item();
    static play_t p;
    caps_t k;
    char t[400], when[32], g[400];
    int x, y, tw, top = -HEADER_H, poster_foot;
    if (!it)
        return;
    det_wd = vis_width();
    det_art_box();                  /* plotted scaled to it until it's fetched at that size */
    if (((S.det_art || art_wanted) && (art_w != art_fw || art_h != art_fh)) ||
        ((S.det_poster || art_wanted) && det_pw != det_pfw))
        art_due = now_cs() + 50;    /* once the window's been left alone for a moment */
    det_tx = 40 + det_pw + 56;
    tw = det_wd - det_tx - 40;
    poster_foot = top - 96 - det_ph;
    /* the title */
    latin1(it->title, S.det_title, sizeof(S.det_title));
    draw_fit(D_TITLE, S.det_title, tw);
    det_title_y = top - 96 - 48;
    /* the labels: (an episode's place), year, how long, the age rating, the scores */
    S.nchip = 0;
    S.det_meta[0] = 0;
    x = det_tx;
    y = det_title_y - 44;
#define CHIP(...) do { snprintf(t, sizeof(t), __VA_ARGS__); det_chip(0, t, &x, &y); \
        if (S.det_meta[0]) strncat(S.det_meta, "   \xb7   ", sizeof(S.det_meta) - strlen(S.det_meta) - 1); \
        strncat(S.det_meta, t, sizeof(S.det_meta) - strlen(S.det_meta) - 1); } while (0)
    if (it->type && !strcmp(it->type, "episode") && it->subtitle)
        CHIP("%s", it->subtitle);
    if (it->year > 0)
        CHIP("%d", it->year);
    if (it->duration_ms > 0) {
        long m = (long)(it->duration_ms / 60000);
        if (m >= 60)
            CHIP("%ldh %02ldm", m / 60, m % 60);
        else
            CHIP("%ldm", m);
    }
    if (it->content_rating) {
        latin1(strchr(it->content_rating, '/') ? strchr(it->content_rating, '/') + 1 : it->content_rating, g,
               sizeof(g));             /* "gb/18": 18 */
        CHIP("%s", g);
    }
    if (it->rating > 0 && it->audience_rating > 0 && it->rating != it->audience_rating) {
        CHIP("Critics %.1f", it->rating);
        CHIP("Audience %.1f", it->audience_rating);
    } else if (it->rating > 0)
        CHIP("%.1f", it->rating);
#undef CHIP
    /* the genres, on a line of their own */
    if (it->genres) {
        char *gp, *e;
        latin1(it->genres, g, sizeof(g));
        x = det_tx;
        y -= 68;
        for (gp = g; *gp; gp = e) {
            e = strstr(gp, ", ");
            if (e) {
                *e = 0;
                e += 2;
            } else
                e = gp + strlen(gp);
            det_chip(1, gp, &x, &y);
        }
    }
    y -= 56 + 36;
    /* the summary */
    {
        static char sum[2400];
        latin1(it->summary ? it->summary : "", sum, sizeof(sum));
        /* over the backdrop's solid part (as the picture fades in, it'd be
           hard to read), but never very narrow */
        int sw = det_wd * 52 / 100 - det_tx;
        if (sw < 900)
            sw = tw < 900 ? tw : 900;
        S.det_nlines = draw_wrap(D_BODY, sum, sw, S.det_lines, 6);
    }
    det_sum_y = y - 12;
    y = det_sum_y - S.det_nlines * 40 - (S.det_nlines ? 24 : 0);
    /* the buttons */
    S.nbtn = 0;
    det_bx = x = det_tx;
    det_button(D_PLAY, "Play", &x, &y);
    if (it->view_offset_ms > 0) {
        hms(it->view_offset_ms, when, sizeof(when));
        snprintf(t, sizeof(t), "Resume from %s", when);
        det_button(D_RESUME, t, &x, &y);
    }
    det_button(D_START, "From the start", &x, &y);
    det_button(D_WATCHED, it->watched ? "Mark unwatched" : "Mark watched", &x, &y);
    if (it->nsubs) {
        int sel = plex_sub_selected(it);
        char name[120];
        latin1(sel >= 0 ? it->subs[sel].title : "None", name, sizeof(name));
        snprintf(t, sizeof(t), "Subtitles: %s", name);
        det_button(D_SUBS, t, &x, &y);
    }
    if (it->part_key && it->part_size <= FILE_MAX)
        det_button(D_SAVE, "Save file", &x, &y);
    y -= BTN + 20;
    /* your rating: five stars (click one to rate; the one you gave, to take it away) */
    if (it->rating_key && S.px.kind == SRV_PLEX && S.nbtn + 5 <= DET_BTN_MAX) {   /* Jellyfin keeps no stars */
        det_stars_y = y - 8;
        det_stars_x = det_tx + draw_width(D_BODY, "Your rating") + 24;
        for (int k = 0; k < 5; k++) {
            S.btn[S.nbtn].id = D_STAR + k;
            S.btn[S.nbtn].x0 = det_stars_x + k * 56;
            S.btn[S.nbtn].x1 = det_stars_x + k * 56 + 48;
            S.btn[S.nbtn].y1 = det_stars_y;
            S.btn[S.nbtn].y0 = det_stars_y - 48;
            snprintf(S.btn[S.nbtn].label, sizeof(S.btn[0].label), "%s",
                     it->user_rating >= 2 * (k + 1) - 0.5 ? "*" : "-");
            S.nbtn++;
        }
        y -= 48 + 44;
    } else
        det_stars_y = 0;
    /* how it will play (and that it has no subtitles) */
    caps_for(S.quality, &k);
    k.own_subs = S.player == PLAYER_BUILTIN;
    k.hevc = S.player == PLAYER_BUILTIN && player_hevc_block();
    k.dry = 1;                      /* nothing asked of the server for it */
    caps_play(&S.px, it, &k, S.direct, 1, &p);
    latin1(p.why, S.det_how, sizeof(S.det_how));
    if (!it->nsubs && strlen(S.det_how) + 20 < sizeof(S.det_how))
        strcat(S.det_how, ". No subtitles");
    S.det_how_n = draw_wrap(D_BODY, S.det_how, tw, S.det_how_l, 2);
    det_how_y = y - 16;
    y -= 24 + 40 * S.det_how_n;
    /* the rest under the poster or the text, whichever goes further down */
    if (y > poster_foot)
        y = poster_foot;
    det_layout_more(it, y - 96);
}

void det_redraw(int ox, int oy, int vis_w, int cy0, int cy1)
{
    const plex_item *it = det_item();
    int top = -HEADER_H, px0 = 40, py1 = top - 96, py0 = py1 - det_ph;
    (void)vis_w;
    if (!it)
        return;
    if (cy1 > top - art_h) {        /* the backdrop: across the top, faded into the page */
        if (S.det_art && S.det_art->area)
            plot_sprite(S.det_art, 0, top - art_h, art_w, top);
        else
            draw_rect(ox, oy + top - art_h, ox + det_wd, oy + top, RGB(30, 33, 39));
    }
    if (cy1 > py0 && cy0 < py1) {   /* the poster, on a shadow */
        draw_round(ox + px0 + 8, oy + py0 - 12, ox + px0 + det_pw + 8, oy + py1 - 8, 16, C_SHADOW, C_BG);
        if (S.det_poster && S.det_poster->area && det_pfw == det_pw)
            plot_sprite(S.det_poster, px0, py0, px0 + det_pw, py1);
        else
            draw_round(ox + px0, oy + py0, ox + px0 + det_pw, oy + py1, 12, C_CARD, C_BG);
        badges(it, ox + px0, oy + py0, ox + px0 + det_pw, oy + py1);
    }
    draw_text_over(D_TITLE, ox + det_tx, oy + det_title_y, S.det_title, C_TEXT);
    for (int i = 0; i < S.nchip; i++) {
        unsigned bg = S.chip[i].kind ? RGB(33, 36, 43) : C_CHIP;
        int y0 = oy + S.chip[i].y0;
        draw_round(ox + S.chip[i].x0, y0, ox + S.chip[i].x1, y0 + 56, 28, bg, DRAW_NONE);
        draw_text(S.chip[i].kind ? D_BODY : D_BOLD, ox + S.chip[i].x0 + 22, y0 + 18, S.chip[i].t,
                  S.chip[i].kind ? C_SUB : C_TEXT, bg);
    }
    for (int l = 0; l < S.det_nlines; l++)
        draw_text(D_BODY, ox + det_tx, oy + det_sum_y - l * 40, S.det_lines[l], C_TEXT, C_BG);
    for (int i = 0; i < S.nbtn; i++) {
        int x0 = ox + S.btn[i].x0, y0 = oy + S.btn[i].y0, x1 = ox + S.btn[i].x1, y1 = oy + S.btn[i].y1;
        unsigned bg = S.btn[i].id == D_PLAY ? C_ACCENT : C_CARD;
        int tx = x0 + 32;
        if (S.btn[i].id >= D_STAR && S.btn[i].id < D_STAR + 5) {      /* a star: lit, or grey */
            if (S.btn[i].id == D_STAR)
                draw_text_over(D_BODY, ox + det_tx, y0 + 12, "Your rating", C_SUB);
            draw_glyph(G_STAR, x0, y0, x1, y1, S.btn[i].label[0] == '*' ? C_GOLD : C_CARD, DRAW_NONE);
            continue;
        }
        draw_round(x0, y0, x1, y1, BTN / 2, bg, DRAW_NONE);
        if (S.btn[i].id == D_PLAY) {            /* a play triangle */
            draw_glyph(G_PLAY, tx - 4, y0 + 16, tx + 28, y1 - 16, C_TEXT, bg);
            tx += 36;
        }
        draw_text(D_BOLD, tx, y0 + 22, S.btn[i].label, C_TEXT, bg);
        if (S.btn[i].id == D_SUBS)              /* a menu arrow */
            draw_glyph(G_DOWN, x1 - 52, y0 + 16, x1 - 20, y1 - 16, C_TEXT, bg);
    }
    for (int l = 0; l < S.det_how_n; l++)
        draw_text(D_BODY, ox + det_tx, oy + det_how_y - l * 40, S.det_how_l[l], C_SUB, C_BG);
    for (int l = 0; l < S.det_ncred; l++) {
        draw_text(D_BODY, ox + det_cred_x, oy + det_cred_y - l * 40, S.det_cred[l][0], C_SUB, C_BG);
        draw_text(D_BOLD, ox + det_cred_x + CRED_X, oy + det_cred_y - l * 40, S.det_cred[l][1], C_TEXT, C_BG);
    }
    for (int k = 0; k < REL_ROWS; k++) {
        /* the row: its heading above the pictures, two lines of title below */
        if (!S.nrel[k] || S.relc[k][0].y0 - 100 > cy1 || S.rel_y[k] + 60 < cy0)
            continue;
        draw_text(D_HEAD, ox + 40, oy + S.rel_y[k], S.rel_head[k], C_TEXT, C_BG);
        for (int i = 0; i < S.nrel[k]; i++) {
            int x0 = S.relc[k][i].x0, y0 = S.relc[k][i].y0, x1 = S.relc[k][i].x1, y1 = S.relc[k][i].y1;
            draw_round(ox + x0 + 6, oy + y0 - 10, ox + x1 + 6, oy + y1 - 6, 16, C_SHADOW, C_BG);
            if (S.relc[k][i].pic && S.relc[k][i].pic->area)
                plot_sprite(S.relc[k][i].pic, x0, y0, x1, y1);
            else
                draw_round(ox + x0, oy + y0, ox + x1, oy + y1, 12, C_CARD, C_BG);
            if (k)                  /* a play triangle on a trailer */
                draw_glyph(G_PLAY, ox + (x0 + x1) / 2 - 24, oy + (y0 + y1) / 2 - 28, ox + (x0 + x1) / 2 + 28,
                           oy + (y0 + y1) / 2 + 28, C_TEXT, DRAW_NONE);
            else
                badges(&S.rel[k].v[i], ox + x0, oy + y0, ox + x1, oy + y1);
            draw_text(D_BOLD, ox + x0, oy + y0 - 40, S.relc[k][i].line[0], C_TEXT, C_BG);
            draw_text(D_BODY, ox + x0, oy + y0 - 80, S.relc[k][i].line[1], C_SUB, C_BG);
        }
    }
    if (det_cast_y) {
        draw_text(D_TITLE, ox + 40, oy + det_cast_y, "Cast", C_TEXT, C_BG);
        for (int i = 0; i < S.ncast; i++) {
            int x0 = S.cast[i].x0, y0 = S.cast[i].y0, px = x0 + (CAST_W - CAST_PH) / 2;
            int py0c = y0 + CAST_H - CAST_PH;
            if (y0 > cy1 || y0 + CAST_H < cy0)
                continue;
            if (S.cast[i].photo && S.cast[i].photo->area)
                plot_sprite(S.cast[i].photo, px, py0c, px + CAST_PH, py0c + CAST_PH);
            else
                draw_glyph(G_CIRCLE, ox + px, oy + py0c, ox + px + CAST_PH, oy + py0c + CAST_PH, C_CARD, C_BG);
            draw_text(D_BOLD, ox + x0 + (CAST_W - draw_width(D_BOLD, S.cast[i].name)) / 2, oy + py0c - 36,
                      S.cast[i].name, C_TEXT, C_BG);
            draw_text(D_BODY, ox + x0 + (CAST_W - draw_width(D_BODY, S.cast[i].role)) / 2, oy + py0c - 76,
                      S.cast[i].role, C_SUB, C_BG);
        }
    }
}

/* The page laid out again, its extent set, and redrawn */
void det_repaint(void)
{
    if (S.page != PG_DETAILS || !S.browser_open)
        return;
    det_layout();
    set_extent();
    set_where();
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
}

/* List item i's details, in the window */
void det_show(int i)
{
    const plex_item *it = i >= 0 && i < S.list.n ? &S.list.v[i] : NULL;
    int st[9];
    if (!it || it->kind != PI_VIDEO)
        return;
    if (!S.browser_open)
        browser_open();
    det_wd = vis_width();
    if (!det_is(it)) {
        if (det_fetch(it, 1) != 0)
            return;
    } else if (det_art_box(), !S.det_art || art_w != art_fw || art_h != art_fh)
        det_fetch_art(0);
    S.cast_wanted = 1;              /* what's still to come (det_cast_step) */
    window_state(S.browser_w, st);
    if (S.page == PG_GRID)
        S.grid_sy = st[6];          /* to come back to */
    S.page = PG_DETAILS;
    S.det_i = i;
    if (S.sel != i)
        S.sel = i;
    S.hover = -1;
    det_repaint();
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
    set_caret(S.browser_w, -1, NULL);
}

/* Back from the details to the grid, where it was */
void det_leave(void)
{
    int st[9];
    if (S.page != PG_DETAILS)
        return;
    S.page = PG_GRID;
    set_extent();
    set_where();
    window_state(S.browser_w, st);
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, S.grid_sy);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    S.posters_wanted = 1;
}

/* The details again from the server (a choice changed) */
void det_refresh(void)
{
    plex_item key;
    char rk[64];
    const plex_item *it = det_item();
    if (!it || !it->rating_key)
        return;
    snprintf(rk, sizeof(rk), "%s", it->rating_key);
    memset(&key, 0, sizeof(key));
    key.rating_key = rk;
    if (det_fetch(&key, S.page == PG_DETAILS) == 0)
        det_repaint();
}

/* The details button at a screen point, or 0 */
/* More like this or an extra at a screen point: 1000 * (k + 1) + i, or 0 */
int det_rel_at(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int k = 0; k < REL_ROWS; k++)
        for (int i = 0; i < S.nrel[k]; i++)
            if (wx >= S.relc[k][i].x0 && wx < S.relc[k][i].x1 && wy >= S.relc[k][i].y0 - 96 && wy < S.relc[k][i].y1)
                return 1000 * (k + 1) + i;
    return 0;
}

int det_button_at(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int i = 0; i < S.nbtn; i++)
        if (wx >= S.btn[i].x0 && wx < S.btn[i].x1 && wy >= S.btn[i].y0 && wy < S.btn[i].y1)
            return S.btn[i].id;
    return 0;
}
