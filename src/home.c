/*
 * home.c - the home page: a featured item across the top, then rows of posters.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- the home page ------------------------------------------------------------------

   As Plex's apps (and Plezy) open: a featured item across the top (one of
   Continue watching: its backdrop fading into the page, its title,
   labels, summary, Resume and Details, and dots for the others), then
   rows: Continue watching (16:9 pictures with how far you got),
   Recently added in each film and TV library (posters), and the
   libraries. A row shows as many as fit; its title opens the whole list.
   The rows' items are S.list, one row after another (plex_home), so
   selecting, opening, menus and Back are the grid's; the arrow keys move
   along a row and between rows. */

#define HOME_TITLE_H 84             /* a row's title */
#define HOME_GAP     56             /* between rows */
#define HOME_TEXT_H  96             /* the two lines under a card */
#define LIB_W        400            /* a library's card */
#define LIB_H        150
#define HERO_PICKS   5

const plex_item *home_hero(void)
{
    int i = S.home.row[0].start + S.home.pick;
    if (!S.home.on || !S.home.npick || !S.have_list || S.home.nrows < 1 || i < 0 || i >= S.list.n)
        return NULL;
    return &S.list.v[i];
}

/* Which row item i is in, and its place in the row (-1: none) */
int home_row_of(int i, int *col)
{
    for (int r = 0; r < S.home.nrows; r++)
        if (i >= S.home.row[r].start && i < S.home.row[r].start + S.home.row[r].n) {
            *col = i - S.home.row[r].start;
            return r;
        }
    *col = 0;
    return -1;
}

static void home_lib_box(int i, int r, int c, int *x0, int *y0, int *x1, int *y1);

void home_box(int i, int *x0, int *y0, int *x1, int *y1)
{
    int c, r = home_row_of(i, &c);
    if (r < 0 || c >= S.home.lay[r].vis) {          /* not shown: out of the way */
        *x0 = *x1 = -1000;
        *y0 = *y1 = 100000;
        return;
    }
    if (S.home.row[r].kind == PR_LIBRARIES) {
        home_lib_box(i, r, c, x0, y0, x1, y1);
        return;
    }
    *x0 = 40 + c * (S.home.lay[r].cw + GAP);
    *x1 = *x0 + S.home.lay[r].cw;
    *y1 = S.home.lay[r].top;
    *y0 = *y1 - S.home.lay[r].ch - (S.home.row[r].kind == PR_LIBRARIES ? 0 : HOME_TEXT_H);
}

/* A picture on the home page: its key and what's fetched (a row of
   Continue watching shows backdrops, 16:9) */
const char *home_pic(int i, char *key, size_t size, int *art)
{
    const plex_item *it = &S.list.v[i];
    int c, r = home_row_of(i, &c);
    *art = 0;
    if (r < 0)
        return NULL;
    if (S.home.row[r].kind == PR_CONTINUE) {
        const char *t = it->art ? it->art : it->thumb;
        if (!t)
            return NULL;
        snprintf(key, size, "home:%s@%dx%d", t, S.home.lay[r].cw, S.home.lay[r].ch);
        *art = 3;
        return t;
    }
    if (S.home.row[r].kind == PR_RECENT && it->thumb) {
        snprintf(key, size, "%s", it->thumb);       /* as on the grid */
        return it->thumb;
    }
    return NULL;
}

static void home_chip(const char *t, int *x, int y)
{
    int w = draw_width(D_BOLD, t) + 44;
    if (S.home.nchip >= 6)
        return;
    S.home.chip[S.home.nchip].x0 = *x;
    S.home.chip[S.home.nchip].x1 = *x + w;
    S.home.chip[S.home.nchip].y0 = y - 56;
    snprintf(S.home.chip[S.home.nchip].t, sizeof(S.home.chip[0].t), "%s", t);
    S.home.nchip++;
    *x += w + 16;
}

/* Where everything goes, for the window's size */
void home_layout(void)
{
    const plex_item *h;
    int st[9], w = vis_width(), wh = 1100, y, x, top = -HEADER_H;
    char t[400], g[200];
    if (S.browser_open) {
        window_state(S.browser_w, st);
        wh = st[4] - st[2];
    }
    S.home.w = w;
    S.home.npick = S.home.nrows && S.home.row[0].kind == PR_CONTINUE ?
                   (S.home.row[0].n < HERO_PICKS ? S.home.row[0].n : HERO_PICKS) : 0;
    if (S.home.pick >= S.home.npick)
        S.home.pick = 0;
    h = home_hero();
    y = top;
    S.home.hero_h = 0;
    if (h) {                        /* the featured part */
        int hh = wh * 50 / 100;
        if (hh < 560) hh = 560;
        if (hh > w * 9 / 16) hh = w * 9 / 16;
        S.home.hero_h = hh & ~((1 << S.yeig) - 1);
        S.home.art_w = w & ~((1 << S.xeig) - 1);
        if (S.home.art && S.home.fw != w)
            S.home.due = now_cs() + 50;
        snprintf(S.home.label, sizeof(S.home.label), "CONTINUE WATCHING");
        latin1(h->title, S.home.title, sizeof(S.home.title));
        draw_fit(D_HERO, S.home.title, w * 55 / 100);
        S.home.title_y = top - S.home.hero_h * 30 / 100;
        S.home.nchip = 0;
        x = 40;
        if (h->type && !strcmp(h->type, "episode") && h->subtitle) {
            latin1(h->subtitle, g, sizeof(g));
            draw_fit(D_BOLD, g, 600);
            home_chip(g, &x, S.home.title_y - 44);
        }
        if (h->year > 0) {
            snprintf(t, sizeof(t), "%d", h->year);
            home_chip(t, &x, S.home.title_y - 44);
        }
        if (h->duration_ms > 0) {
            long m = (long)(h->duration_ms / 60000);
            if (m >= 60)
                snprintf(t, sizeof(t), "%ldh %02ldm", m / 60, m % 60);
            else
                snprintf(t, sizeof(t), "%ldm", m);
            home_chip(t, &x, S.home.title_y - 44);
        }
        if (h->content_rating) {
            latin1(strchr(h->content_rating, '/') ? strchr(h->content_rating, '/') + 1 : h->content_rating, g, sizeof(g));
            home_chip(g, &x, S.home.title_y - 44);
        }
        {
            static char sum[2400];
            int sw = w * 48 / 100 - 40;
            latin1(h->summary ? h->summary : "", sum, sizeof(sum));
            S.home.nlines = draw_wrap(D_BODY, sum, sw < 700 ? 700 : sw, S.home.lines, 2);
        }
        S.home.sum_y = S.home.title_y - 44 - 56 - 48;
        /* Resume (or Play), how long is left, Details */
        {
            int by = S.home.sum_y - S.home.nlines * 40 - 24, bw;
            const char *pl = h->view_offset_ms > 0 ? "Resume" : "Play";
            S.home.nbtn = 0;
            x = 40;
            bw = draw_width(D_BOLD, pl) + 64 + 36;
            S.home.btn[0] = (typeof(S.home.btn[0])){ HB_PLAY, x, by - BTN, x + bw, by, "" };
            snprintf(S.home.btn[0].t, sizeof(S.home.btn[0].t), "%s", pl);
            x += bw + 24;
            S.home.left[0] = 0;
            if (h->view_offset_ms > 0 && h->duration_ms > h->view_offset_ms) {
                snprintf(S.home.left, sizeof(S.home.left), "%ld min left",
                         (long)((h->duration_ms - h->view_offset_ms) / 60000) + 1);
                S.home.left_x = x;
                x += draw_width(D_BODY, S.home.left) + 32;
            }
            bw = draw_width(D_BOLD, "Details") + 64;
            S.home.btn[1] = (typeof(S.home.btn[1])){ HB_DETAILS, x, by - BTN, x + bw, by, "Details" };
            S.home.nbtn = 2;
        }
        /* the dots: the others to feature */
        for (int k = 0; k < S.home.npick; k++) {
            int dx = w - 60 - (S.home.npick - k) * 44;
            S.home.dot[k].x0 = dx;
            S.home.dot[k].x1 = dx + (k == S.home.pick ? 52 : 20);
            S.home.dot[k].y0 = top - S.home.hero_h + 40;
            S.home.dot[k].y1 = S.home.dot[k].y0 + 20;
        }
        y = top - S.home.hero_h + 24;
    } else {
        y = top - 24;
    }
    /* the rows */
    for (int r = 0; r < S.home.nrows; r++) {
        int kind = S.home.row[r].kind, fit;
        S.home.lay[r].title_y = y - 56;
        S.home.lay[r].title_x1 = 40 + draw_width(D_HEAD, S.home.row[r].title) + (S.home.row[r].path[0] ? 56 : 0);
        S.home.lay[r].top = y - HOME_TITLE_H;
        if (kind == PR_CONTINUE) {
            int cw = w * 20 / 100;
            if (cw < 400) cw = 400;
            if (cw > 640) cw = 640;
            S.home.lay[r].cw = cw & ~((2 << S.xeig) - 1);
            S.home.lay[r].ch = (S.home.lay[r].cw * 9 / 16) & ~((1 << S.yeig) - 1);
        } else if (kind == PR_RECENT) {
            S.home.lay[r].cw = TILE_W;
            S.home.lay[r].ch = POSTER_H;
        } else {
            S.home.lay[r].cw = LIB_W;
            S.home.lay[r].ch = LIB_H;
        }
        fit = (w - 80 + GAP) / (S.home.lay[r].cw + GAP);
        if (fit < 1)
            fit = 1;
        if (kind == PR_LIBRARIES && fit < S.home.row[r].n) {    /* the libraries wrap */
            int rows = (S.home.row[r].n + fit - 1) / fit;
            S.home.lay[r].vis = S.home.row[r].n;
            y = S.home.lay[r].top - rows * (LIB_H + GAP) - HOME_GAP;
            continue;
        }
        S.home.lay[r].vis = fit < S.home.row[r].n ? fit : S.home.row[r].n;
        y = S.home.lay[r].top - S.home.lay[r].ch - (kind == PR_LIBRARIES ? 0 : HOME_TEXT_H) - HOME_GAP;
    }
    S.home.h = -y + 40;
}

/* The libraries wrap onto more lines: their place */
static void home_lib_box(int i, int r, int c, int *x0, int *y0, int *x1, int *y1)
{
    int fit = (S.home.w - 80 + GAP) / (LIB_W + GAP);
    (void)i;
    if (fit < 1)
        fit = 1;
    *x0 = 40 + (c % fit) * (LIB_W + GAP);
    *x1 = *x0 + LIB_W;
    *y1 = S.home.lay[r].top - (c / fit) * (LIB_H + GAP);
    *y0 = *y1 - LIB_H;
}

void home_fetch_art(void)
{
    const plex_item *h = home_hero();
    char key[320];
    S.home.art = NULL;
    S.home.fw = S.home.w;
    S.home.due = 0;
    if (!h || !(h->art || h->thumb))
        return;
    snprintf(key, sizeof(key), "art:%s@%dx%d", h->art ? h->art : h->thumb, S.home.art_w, S.home.hero_h);
    if (!(S.home.art = cache_find(key)))
        S.home.art = poster_fetch(h->art ? h->art : h->thumb, key, S.home.art_w >> S.xeig, S.home.hero_h >> S.yeig, 1);
    cache_trim();
}

void home_redraw(int ox, int oy, int cy0, int cy1)
{
    const plex_item *h = home_hero();
    int top = -HEADER_H;
    if (h && cy1 > top - S.home.hero_h) {
        if (S.home.art && S.home.art->area)
            plot_sprite(S.home.art, 0, top - S.home.hero_h, S.home.art_w, top);
        draw_text_over(D_BOLD, ox + 40, oy + S.home.title_y + 76, S.home.label, C_SUB);
        draw_text_over(D_HERO, ox + 40, oy + S.home.title_y, S.home.title, C_TEXT);
        for (int i = 0; i < S.home.nchip; i++) {
            int y0 = oy + S.home.chip[i].y0;
            draw_round(ox + S.home.chip[i].x0, y0, ox + S.home.chip[i].x1, y0 + 56, 28, C_CHIP, DRAW_NONE);
            draw_text(D_BOLD, ox + S.home.chip[i].x0 + 22, y0 + 18, S.home.chip[i].t, C_TEXT, C_CHIP);
        }
        for (int l = 0; l < S.home.nlines; l++)
            draw_text(D_BODY, ox + 40, oy + S.home.sum_y - l * 40, S.home.lines[l], C_TEXT, C_BG);
        for (int i = 0; i < S.home.nbtn; i++) {
            int x0 = ox + S.home.btn[i].x0, y0 = oy + S.home.btn[i].y0, x1 = ox + S.home.btn[i].x1, y1 = oy + S.home.btn[i].y1;
            unsigned bg = S.home.btn[i].id == HB_PLAY ? C_ACCENT : C_CARD;
            int tx = x0 + 32;
            draw_round(x0, y0, x1, y1, BTN / 2, bg, DRAW_NONE);
            if (S.home.btn[i].id == HB_PLAY) {
                draw_glyph(G_PLAY, tx - 4, y0 + 16, tx + 28, y1 - 16, C_TEXT, bg);
                tx += 36;
                if (h->view_offset_ms > 0 && h->duration_ms > 0) {     /* how far, under the label */
                    int part = (int)((int64_t)(x1 - x0 - 64) * h->view_offset_ms / h->duration_ms);
                    draw_rect(x0 + 32, y0 + 10, x1 - 32, y0 + 16, RGB(30, 90, 160));
                    draw_rect(x0 + 32, y0 + 10, x0 + 32 + part, y0 + 16, C_TEXT);
                }
            }
            draw_text(D_BOLD, tx, y0 + 22, S.home.btn[i].t, C_TEXT, bg);
        }
        if (S.home.left[0])
            draw_text(D_BODY, ox + S.home.left_x, oy + S.home.btn[0].y0 + 22, S.home.left, C_SUB, C_BG);
        for (int k = 0; S.home.npick > 1 && k < S.home.npick; k++)
            draw_round(ox + S.home.dot[k].x0, oy + S.home.dot[k].y0, ox + S.home.dot[k].x1, oy + S.home.dot[k].y1, 10,
                       k == S.home.pick ? C_TEXT : C_CARD_HI, DRAW_NONE);
    }
    for (int r = 0; r < S.home.nrows; r++) {
        if (S.home.lay[r].title_y + 60 < cy0 || S.home.lay[r].title_y - 20 > cy1)
            continue;
        draw_text(D_HEAD, ox + 40, oy + S.home.lay[r].title_y, S.home.row[r].title, C_TEXT, C_BG);
        if (S.home.row[r].path[0])                  /* the whole list: a chevron after the title */
            draw_glyph(G_NEXT, ox + S.home.lay[r].title_x1 - 36, oy + S.home.lay[r].title_y - 2,
                       ox + S.home.lay[r].title_x1 - 8, oy + S.home.lay[r].title_y + 30, C_SUB, C_BG);
    }
}

/* A card on the home page */
void draw_home_card(int i, int ox, int oy)
{
    const plex_item *it = &S.list.v[i];
    disp_t *d = &S.disp[i];
    int c, r = home_row_of(i, &c), x0, y0, x1, y1, py0;
    tile_box(i, &x0, &y0, &x1, &y1);
    if (r < 0 || x0 < 0)
        return;
    if (S.home.row[r].kind == PR_LIBRARIES) {
        unsigned bg = i == S.sel ? C_CARD_HI : C_CARD;
        if (i == S.sel || i == S.hover) {
            unsigned c2 = i == S.sel ? C_TEXT : C_HOVER;
            draw_round(ox + x0 - 10, oy + y0 - 10, ox + x1 + 10, oy + y1 + 10, 30, c2, C_BG);
            draw_round(ox + x0 - 4, oy + y0 - 4, ox + x1 + 4, oy + y1 + 4, 26, C_BG, c2);
        }
        draw_round(ox + x0, oy + y0, ox + x1, oy + y1, 24, bg, C_BG);
        draw_text(D_TITLE, ox + x0 + 32, oy + y1 - 72, d->line[0], it->kind == PI_OTHER ? C_SUB : C_TEXT, bg);
        draw_text(D_BODY, ox + x0 + 32, oy + y0 + 32, d->line[1], C_SUB, bg);
        return;
    }
    py0 = y1 - S.home.lay[r].ch;
    if (i == S.sel || i == S.hover) {
        unsigned c2 = i == S.sel ? C_TEXT : C_HOVER;
        draw_round(ox + x0 - 10, oy + py0 - 10, ox + x1 + 10, oy + y1 + 10, 22, c2, C_BG);
        draw_round(ox + x0 - 4, oy + py0 - 4, ox + x1 + 4, oy + y1 + 4, 16, C_BG, c2);
    } else
        draw_round(ox + x0 + 6, oy + py0 - 10, ox + x1 + 6, oy + y1 - 6, 16, C_SHADOW, C_BG);
    if (d->poster && d->poster->area)
        plot_sprite(d->poster, x0, py0, x1, y1);
    else
        draw_round(ox + x0, oy + py0, ox + x1, oy + y1, 12, C_CARD, C_BG);
    badges(it, ox + x0, oy + py0, ox + x1, oy + y1);
    draw_text(D_BOLD, ox + x0, oy + py0 - 40, d->line[0], C_TEXT, C_BG);
    draw_text(D_BODY, ox + x0, oy + py0 - 80, d->line[1], C_SUB, C_BG);
}

/* The featured part's buttons and dots, a row's title: HB_PLAY,
   HB_DETAILS, HB_DOT + n, HB_ROW + n; or 0 */
int home_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int i = 0; home_hero() && i < S.home.nbtn; i++)
        if (wx >= S.home.btn[i].x0 && wx < S.home.btn[i].x1 && wy >= S.home.btn[i].y0 && wy < S.home.btn[i].y1)
            return S.home.btn[i].id;
    for (int k = 0; S.home.npick > 1 && k < S.home.npick; k++)
        if (wx >= S.home.dot[k].x0 - 8 && wx < S.home.dot[k].x1 + 8 && wy >= S.home.dot[k].y0 - 12 && wy < S.home.dot[k].y1 + 12)
            return HB_DOT + k;
    for (int r = 0; r < S.home.nrows; r++)
        if (S.home.row[r].path[0] && wx >= 40 && wx < S.home.lay[r].title_x1 && wy >= S.home.lay[r].title_y - 20 &&
            wy < S.home.lay[r].title_y + 60)
            return HB_ROW + r;
    return 0;
}

/* The arrow keys on the home page: along a row, or to the row above or below */
void home_key(int k)
{
    int c, r = home_row_of(S.sel, &c), nr;
    if (S.sel < 0) {
        select_tile(S.home.nrows ? S.home.row[0].start : -1);
        return;
    }
    if (k == 0x18C || k == 0x18D) {
        int nc = c + (k == 0x18D ? 1 : -1);
        if (nc >= 0 && nc < S.home.lay[r].vis)
            select_tile(S.home.row[r].start + nc);
        return;
    }
    nr = r + (k == 0x18E ? 1 : -1);
    if (nr < 0 || nr >= S.home.nrows)
        return;
    if (c >= S.home.lay[nr].vis)
        c = S.home.lay[nr].vis - 1;
    select_tile(S.home.row[nr].start + c);
}
