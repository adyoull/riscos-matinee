/*
 * browse.c - the browser: its layout and drawing, the tabs under the bar, the
 * show page, a library's bar, the lists and where you've been, and search.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- the browser's layout ------------------------------------------------------ */

/* The show page's episode rows: a 16:9 picture, the title, the summary */
#define EP_ROW  (EP_TH + 40)
#define EP_GAP  16

void tile_box(int i, int *x0, int *y0, int *x1, int *y1)
{
    int col = i % S.cols, row = i / S.cols;
    if (S.home.on) {
        home_box(i, x0, y0, x1, y1);
        return;
    }
    if (S.show.on) {
        *x0 = 24;
        *x1 = S.show.w - 24;
        *y1 = -S.show.head_h - i * (EP_ROW + EP_GAP);
        *y0 = *y1 - EP_ROW;
        return;
    }
    *x0 = GAP + col * (TILE_W + GAP);
    *x1 = *x0 + TILE_W;
    *y1 = -HEADER_H - libbar_h() - GAP - row * (TILE_H + GAP);
    *y0 = *y1 - TILE_H;
}

int layout_cols(int width)
{
    int c = (width - GAP) / (TILE_W + GAP);
    return c < 1 ? 1 : c;
}

static int list_height(void)
{
    int rows = S.have_list ? (S.list.n + S.cols - 1) / S.cols : 0;
    if (S.show.on)
        return S.show.head_h + S.list.n * (EP_ROW + EP_GAP) + GAP;
    if (S.home.on)
        return S.home.h;
    return HEADER_H + libbar_h() + GAP + rows * (TILE_H + GAP);
}

void set_extent(void)
{
    int b[4];
    _kernel_swi_regs r;
    int h = S.page == PG_DETAILS ? S.det_h : S.page == PG_SIGNIN ? 1400 : S.page == PG_PLAYER ? 1100 : list_height();
    if (h < S.scr_h)
        h = S.scr_h;                /* at least the screen: the window can be made taller */
    b[0] = 0; b[1] = -h; b[2] = S.scr_w; b[3] = 0;
    r.r[0] = S.browser_w;
    r.r[1] = (intptr_t)b;
    swi(Wimp_SetExtent, &r);
}

void force_redraw(int w, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = x0; r.r[2] = y0; r.r[3] = x1; r.r[4] = y1;
    swi(Wimp_ForceRedraw, &r);
}

void redraw_tile(int i)
{
    int x0, y0, x1, y1;
    if (i < 0 || !S.browser_open || S.page != PG_GRID)      /* the grid's: not over the details */
        return;
    tile_box(i, &x0, &y0, &x1, &y1);
    force_redraw(S.browser_w, x0 - 16, y0 - 16, x1 + 20, y1 + 16);
}

/* An episode's picture on the show page: its own key (its size differs
   from a poster's) */
static void ep_key(const plex_item *it, char *out, size_t size)
{
    snprintf(out, size, "ep:%s@%dx%d", it->thumb ? it->thumb : "", EP_TW, EP_TH);
}

/* The show page's rows: "3. The title", how long (and how much is left),
   two lines of the summary */
static void make_disp_episodes(void)
{
    int tw = S.show.w - 24 - (40 + EP_TW + 40) - 40;
    if (tw > 1800)
        tw = 1800;                  /* lines any longer are hard to read */
    for (int i = 0; i < S.list.n; i++) {
        const plex_item *it = &S.list.v[i];
        disp_t *d = &S.disp[i];
        char t[400], key[300], sum[1200], lines[2][160];
        long m = (long)(it->duration_ms / 60000), left = (long)((it->duration_ms - it->view_offset_ms) / 60000);
        int n;
        if (it->index > 0)
            snprintf(t, sizeof(t), "%d. %s", it->index, it->title);
        else
            snprintf(t, sizeof(t), "%s", it->title);
        latin1(t, d->line[0], sizeof(d->line[0]));
        draw_fit(D_BOLD, d->line[0], tw * 2 / 3);
        if (it->view_offset_ms > 0 && m > 0)
            snprintf(d->line[1], sizeof(d->line[1]), "%ld min   \xb7   %ld min left", m, left > 0 ? left : 1);
        else if (m > 0)
            snprintf(d->line[1], sizeof(d->line[1]), "%ld min", m);
        else
            d->line[1][0] = 0;
        latin1(it->summary ? it->summary : "", sum, sizeof(sum));
        n = draw_wrap(D_BODY, sum, tw, lines, 2);
        for (int l = 0; l < 2; l++)
            snprintf(d->sum[l], sizeof(d->sum[l]), "%s", l < n ? lines[l] : "");
        ep_key(it, key, sizeof(key));
        d->poster = it->thumb ? cache_find(key) : NULL;
    }
}

/* The two lines under each poster, cut to fit */
void make_disp(void)
{
    free(S.disp);
    S.disp = calloc(S.list.n ? S.list.n : 1, sizeof(disp_t));
    if (!S.disp)
        return;
    if (S.show.on) {
        make_disp_episodes();
        return;
    }
    if (S.home.on) {
        for (int i = 0; i < S.list.n; i++) {
            const plex_item *it = &S.list.v[i];
            int c, r = home_row_of(i, &c), w = r >= 0 ? S.home.lay[r].cw - (S.home.row[r].kind == PR_LIBRARIES ? 64 : 0) : TILE_W;
            char key[320];
            int art;
            latin1(it->title, S.disp[i].line[0], sizeof(S.disp[i].line[0]));
            latin1(it->subtitle ? it->subtitle : "", S.disp[i].line[1], sizeof(S.disp[i].line[1]));
            draw_fit(r >= 0 && S.home.row[r].kind == PR_LIBRARIES ? D_TITLE : D_BOLD, S.disp[i].line[0], w);
            draw_fit(D_BODY, S.disp[i].line[1], w);
            S.disp[i].poster = home_pic(i, key, sizeof(key), &art) ? cache_find(key) : NULL;
        }
        return;
    }
    /* the grid: the lines are cut to fit as each tile is first drawn (a
       library of 2000 needn't measure 4000 lines before it's shown) */
    for (int i = 0; i < S.list.n; i++) {
        const plex_item *it = &S.list.v[i];
        latin1(it->title, S.disp[i].line[0], sizeof(S.disp[i].line[0]));
        latin1(it->subtitle ? it->subtitle : "", S.disp[i].line[1], sizeof(S.disp[i].line[1]));
        S.disp[i].poster = it->thumb ? cache_find(it->thumb) : NULL;
    }
}

/* A grid tile's two lines, cut to the tile's width (once) */
void fit_disp(int i)
{
    disp_t *d = &S.disp[i];
    if (d->fitted || S.home.on || S.show.on)
        return;
    draw_fit(D_BOLD, d->line[0], TILE_W);
    draw_fit(D_BODY, d->line[1], TILE_W);
    d->fitted = 1;
}

/* ---- the browser: drawing ------------------------------------------------------ */

static void plot(const icon_t *ic)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)ic;
    swi(Wimp_PlotIcon, &r);
}

/* A sprite from our cache, plotted in the middle of a work area box */
void plot_sprite(const poster_t *p, int x0, int y0, int x1, int y1)
{
    icon_t ic;
    memset(&ic, 0, sizeof(ic));
    ic.box[0] = x0; ic.box[1] = y0; ic.box[2] = x1; ic.box[3] = y1;
    ic.flags = 0x0000011Au;         /* sprite, centred, indirected */
    ic.data[0] = (int)(intptr_t)"p";
    ic.data[1] = (int)(intptr_t)p->area;
    ic.data[2] = 1;
    plot(&ic);
}

/* The header's buttons, work area: Back on the left, Refresh on the right
   of the width shown */
void header_button(int id, int vis_w, int *x0, int *y0, int *x1, int *y1)
{
    *y0 = -TOPBAR_H + 32;
    *y1 = *y0 + BTN;
    *x0 = id == B_BACK ? 28 : id == B_SEARCH ? vis_w - 28 - 80 - 20 - 80 : vis_w - 28 - 80;
    *x1 = *x0 + 80;
}

static int header_has(int id)
{
    return S.page != PG_SIGNIN && (id == B_BACK || id == B_REFRESH || (id == B_SEARCH && S.page == PG_GRID));
}

/* The grid is showing a search: its field is in the bar */
int searching(void)
{
    return S.page == PG_GRID && S.have_list && !strncmp(S.path, "search:", 7);
}

/* ox, oy: the work area's origin on the screen; vis_w: the width shown */
/* ---- Home and the libraries, as tabs under the bar ------------------------------------

   Always there on the browser's pages (not signing in), so a library is
   one click away from anywhere, as a TV app's top tabs. The one you're in
   (or came from) is light. */

int strip_h(void)
{
    return S.page != PG_SIGNIN && S.page != PG_PLAYER && S.libs.n > 0 ? STRIP_H : 0;
}

/* Tab k (0 Home, then the libraries): where it is, work area */
int tab_box(int k, int *x0, int *y0, int *x1, int *y1)
{
    int x = 28;
    if (k < 0 || k > S.libs.n || k >= LIB_TABS)
        return -1;
    for (int i = 0; i <= k; i++) {
        const char *t = i ? S.libs.v[i - 1].title : "Home";
        char l[80];
        int w;
        latin1(t, l, sizeof(l));
        w = draw_width(D_BOLD, l) + 56;
        if (i == k) {
            *x0 = x;
            *x1 = x + w;
            break;
        }
        x += w + 12;
    }
    *y1 = -TOPBAR_H - 12;
    *y0 = *y1 - 56;
    return 0;
}

/* The tab for where you are: Home on the home page; a library in it, or
   anywhere you went from it (its list in the history); else -1 */
int tab_current(void)
{
    if (!S.have_list)
        return -1;
    if (!*S.path)
        return 0;
    for (int i = 0; i < S.libs.n && i + 1 < LIB_TABS; i++) {
        const char *k = S.libs.v[i].key;
        char pre[64];
        int sec;
        if (!k || sscanf(k, "/library/sections/%d/", &sec) != 1)
            continue;
        snprintf(pre, sizeof(pre), "/library/sections/%d/", sec);   /* its films, collections... */
        if (!strncmp(S.path, pre, strlen(pre)) || (S.lib.on && S.lib.view == 2 && S.lib.sec == sec))
            return i + 1;
        for (int h = 0; h < S.nhist; h++)
            if (!strncmp(S.hist[h].path, pre, strlen(pre)))
                return i + 1;
    }
    return -1;
}

static void draw_tabs(int ox, int oy)
{
    int cur = tab_current(), vis_w;
    if (!strip_h())
        return;
    vis_w = vis_width();
    for (int k = 0; k <= S.libs.n && k < LIB_TABS; k++) {
        int x0, y0, x1, y1, on = k == cur;
        char l[80];
        if (tab_box(k, &x0, &y0, &x1, &y1) || x1 > vis_w - 28)
            break;                  /* no more room */
        latin1(k ? S.libs.v[k - 1].title : "Home", l, sizeof(l));
        if (on)
            draw_round(ox + x0, oy + y0, ox + x1, oy + y1, 28, C_TEXT, C_HEADER);
        draw_text(D_BOLD, ox + x0 + 28, oy + y0 + 18, l, on ? C_HEADER : k && S.libs.v[k - 1].kind == PI_OTHER ? C_SUB : C_TEXT,
                  on ? C_TEXT : C_HEADER);
    }
}

/* The tab at a screen point, or -1 */
int tab_hit(int sx, int sy)
{
    int st[9], wx, wy;
    if (!strip_h())
        return -1;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int k = 0; k <= S.libs.n && k < LIB_TABS; k++) {
        int x0, y0, x1, y1;
        if (tab_box(k, &x0, &y0, &x1, &y1) || x1 > st[3] - st[1] - 28)
            break;
        if (wx >= x0 && wx < x1 && wy >= y0 && wy < y1)
            return k;
    }
    return -1;
}

static void draw_header(int ox, int oy, int vis_w)
{
    int x0, y0, x1, y1, tx = 28 + 80 + 28, right = vis_w - 28;
    char where[256];
    draw_rect(ox, oy - HEADER_H, ox + S.scr_w, oy, C_HEADER);
    if (S.page == PG_SIGNIN)
        tx = 40;
    if (header_has(B_BACK)) {   /* a chevron: grey when there's nowhere to go back to */
        header_button(B_BACK, vis_w, &x0, &y0, &x1, &y1);
        draw_round(ox + x0, oy + y0, ox + x1, oy + y1, BTN / 2, C_CARD, C_HEADER);
        draw_glyph(G_BACK, ox + x0 + 20, oy + y0 + 12, ox + x1 - 20, oy + y1 - 12,
                   S.nhist || S.page == PG_DETAILS ? C_TEXT : C_SUB, C_CARD);
    }
    header_button(B_REFRESH, vis_w, &x0, &y0, &x1, &y1);
    if (header_has(B_REFRESH) && x0 > tx + 200) {
        draw_round(ox + x0, oy + y0, ox + x1, oy + y1, BTN / 2, C_CARD, C_HEADER);
        draw_glyph(G_REFRESH, ox + x0 + 16, oy + y0 + 8, ox + x1 - 16, oy + y1 - 8, C_TEXT, C_CARD);
        right = x0 - 28;
    }
    header_button(B_SEARCH, vis_w, &x0, &y0, &x1, &y1);
    if (header_has(B_SEARCH) && x0 > tx + 200) {
        int on = searching();
        draw_round(ox + x0, oy + y0, ox + x1, oy + y1, BTN / 2, on ? C_ACCENT : C_CARD, C_HEADER);
        draw_glyph(G_SEARCH, ox + x0 + 20, oy + y0 + 12, ox + x1 - 20, oy + y1 - 12, C_TEXT, on ? C_ACCENT : C_CARD);
        right = x0 - 28;
    }
    if (searching()) {          /* the field, where "where you are" would be */
        char q[120], n[40];
        int fx0 = tx, fx1 = right, fy0 = -TOPBAR_H + 32, fy1 = fy0 + BTN, qw, room = fx1 - fx0 - 64;
        snprintf(n, sizeof(n), "%d found", S.list.n);
        draw_round(ox + fx0, oy + fy0, ox + fx1, oy + fy1, BTN / 2, C_CARD, C_HEADER);
        snprintf(q, sizeof(q), "%s", S.query);
        /* how many were found, on the right, when there's room for it too */
        if (S.query[0] && !S.search_due && draw_width(D_BOLD, q) + draw_width(D_BODY, n) + 40 < room) {
            draw_text(D_BODY, ox + fx1 - 28 - draw_width(D_BODY, n), oy + fy0 + 22, n, C_SUB, C_CARD);
            room -= draw_width(D_BODY, n) + 40;
        }
        draw_fit(D_BOLD, q, room);
        qw = S.query[0] ? draw_width(D_BOLD, q) : 0;
        if (S.query[0])
            draw_text(D_BOLD, ox + fx0 + 28, oy + fy0 + 22, q, C_TEXT, C_CARD);
        else
            draw_text(D_BODY, ox + fx0 + 60, oy + fy0 + 22, "Search films and TV", C_SUB, C_CARD);
        draw_rect(ox + fx0 + 28 + qw + 4, oy + fy0 + 14, ox + fx0 + 28 + qw + 8, oy + fy1 - 14, C_ACCENT);   /* the caret */
        draw_tabs(ox, oy);
        return;
    }
    snprintf(where, sizeof(where), "%s", S.where);
    draw_fit(D_BOLD, where, right - tx);
    draw_text(D_BOLD, ox + tx, oy - 58, where, C_TEXT, C_HEADER);
    snprintf(where, sizeof(where), "%s", S.status);
    draw_fit(D_BODY, where, right - tx);
    draw_text(D_BODY, ox + tx, oy - 100, where, C_SUB, C_HEADER);
    draw_tabs(ox, oy);
}

static const char *kind_name(const plex_item *it)
{
    return it->kind == PI_VIDEO ? "Video" : it->kind == PI_FOLDER ? "Folder" : "Not yet";
}

/* A picture's badges (screen coordinates of the picture): how far it's
   been watched (a bar along its foot), a tick when it's been seen, or how
   many episodes of a show or season haven't */
/* Which badge: 1 a tick (watched), 2 the number not seen (a show, a series), 0 none */
int badge_kind(const plex_item *it)
{
    if (it->watched && it->rating_key && it->kind != PI_OTHER)
        return 1;
    if (it->kind == PI_FOLDER && it->unwatched > 0)
        return 2;
    return 0;
}

void badges(const plex_item *it, int x0, int y0, int x1, int y1)
{
    if (it->kind == PI_VIDEO && it->view_offset_ms > 0 && it->duration_ms > 0) {
        int part = (int)((int64_t)(x1 - x0 - 24) * it->view_offset_ms / it->duration_ms);
        draw_rect(x0 + 12, y0 + 8, x1 - 12, y0 + 18, RGB(20, 21, 26));
        draw_rect(x0 + 12, y0 + 8, x0 + 12 + (part < 8 ? 8 : part), y0 + 18, C_ACCENT);
    }
    if (badge_kind(it) == 1) {
        draw_glyph(G_CIRCLE, x1 - 60, y1 - 60, x1 - 8, y1 - 8, C_TEXT, DRAW_NONE);
        draw_glyph(G_TICK, x1 - 56, y1 - 56, x1 - 12, y1 - 12, RGB(15, 16, 20), C_TEXT);
    } else if (badge_kind(it) == 2) {
        char n[16];
        int w;
        snprintf(n, sizeof(n), "%d", it->unwatched);
        w = draw_width(D_BOLD, n) + 28;
        if (w < 52)
            w = 52;
        draw_round(x1 - 8 - w, y1 - 60, x1 - 8, y1 - 8, 26, C_ACCENT, DRAW_NONE);
        draw_text(D_BOLD, x1 - 8 - (w + draw_width(D_BOLD, n)) / 2, y1 - 44, n, C_TEXT, C_ACCENT);
    }
}

static void draw_tile(int i, int ox, int oy)
{
    const plex_item *it = &S.list.v[i];
    disp_t *d = &S.disp[i];
    int x0, y0, x1, y1, py0;
    fit_disp(i);
    tile_box(i, &x0, &y0, &x1, &y1);
    py0 = y1 - POSTER_H;
    /* a light border round the selected one (grey: pointed at); else a shadow */
    if (i == S.sel || i == S.hover) {
        unsigned c = i == S.sel ? C_TEXT : C_HOVER;
        draw_round(ox + x0 - 10, oy + py0 - 10, ox + x1 + 10, oy + y1 + 10, 22, c, C_BG);
        draw_round(ox + x0 - 4, oy + py0 - 4, ox + x1 + 4, oy + y1 + 4, 16, C_BG, c);
    } else {
        draw_round(ox + x0 + 6, oy + py0 - 10, ox + x1 + 6, oy + y1 - 6, 16, C_SHADOW, C_BG);
    }
    if (d->poster && d->poster->area) {
        plot_sprite(d->poster, x0, py0, x1, y1);
    } else {                        /* no poster (yet): a card */
        const char *k = kind_name(it);
        draw_round(ox + x0, oy + py0, ox + x1, oy + y1, 12, C_CARD, C_BG);
        draw_text(D_BODY, ox + (x0 + x1 - draw_width(D_BODY, k)) / 2, oy + py0 + POSTER_H / 2 - 8, k, C_SUB, C_CARD);
    }
    badges(it, ox + x0, oy + py0, ox + x1, oy + y1);
    draw_text(D_BOLD, ox + x0, oy + py0 - 40, d->line[0], it->kind == PI_OTHER ? C_SUB : C_TEXT, C_BG);
    draw_text(D_BODY, ox + x0, oy + py0 - 80, d->line[1], C_SUB, C_BG);
}

static void show_redraw(int ox, int oy, int cy0, int cy1);
static void draw_episode(int i, int ox, int oy);
static void lib_redraw(int ox, int oy);

void redraw(int *b)
{
    _kernel_swi_regs r;
    int more;
    if (player_owns(b[0]) || (b[0] == S.browser_w && S.page == PG_PLAYER)) {
        player_redraw(b);
        return;
    }
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_RedrawWindow, &r))
        return;
    more = r.r[0];
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6];     /* the work area's origin on the screen */
        int cy0 = b[8] - oy, cy1 = b[10] - oy;      /* the clip, work area y */
        draw_origin(ox, oy);
        if (b[0] == S.browser_w) {
            draw_rect(b[7], b[8], b[9] + 2, b[10] + 2, C_BG);
            if (S.page == PG_DETAILS)
                det_redraw(ox, oy, b[3] - b[1], cy0, cy1);
            else if (S.page == PG_SIGNIN)
                signin_redraw(ox, oy, b[3] - b[1]);
            else if (S.home.on && S.have_list && S.disp) {
                home_redraw(ox, oy, cy0, cy1);
                for (int i = 0; i < S.list.n; i++) {
                    int x0, y0, x1, y1;
                    tile_box(i, &x0, &y0, &x1, &y1);
                    if (x0 >= 0 && y0 - 20 <= cy1 && y1 + 20 >= cy0)
                        draw_home_card(i, ox, oy);
                }
            } else if (S.show.on && S.have_list && S.disp) {
                show_redraw(ox, oy, cy0, cy1);
                for (int i = 0; i < S.list.n; i++) {
                    int x0, y0, x1, y1;
                    tile_box(i, &x0, &y0, &x1, &y1);
                    if (y0 <= cy1 && y1 >= cy0)
                        draw_episode(i, ox, oy);
                }
            } else if (S.have_list && S.disp && S.cols > 0) {
                int top = -HEADER_H - libbar_h() - GAP;
                if (S.lib.on && cy1 > top)
                    lib_redraw(ox, oy);
                int rf = (top - cy1) / (TILE_H + GAP) - 1, rl = (top - cy0) / (TILE_H + GAP) + 1;
                if (rf < 0)
                    rf = 0;
                for (int row = rf; row <= rl; row++)
                    for (int c = 0; c < S.cols; c++) {
                        int i = row * S.cols + c;
                        if (i < S.list.n)
                            draw_tile(i, ox, oy);
                    }
            }
            /* the bar last: over whatever was drawn under it */
            if (cy1 > -HEADER_H)
                draw_header(ox, oy, b[3] - b[1]);
        }
        r.r[1] = (intptr_t)b;
        if (swi(Wimp_GetRectangle, &r))
            break;
        more = r.r[0];
    }
}

/* The first tile in view without a poster, fetched: 1 if one was */
int poster_step(void)
{
    int st[9], vtop, vbot, rf, rl;
    if (!S.browser_open || !S.have_list || !S.disp || S.page != PG_GRID) {
        S.posters_wanted = 0;
        return 0;
    }
    window_state(S.browser_w, st);
    vtop = st[6];
    vbot = st[6] - (st[4] - st[2]);
    if (S.home.on) {                /* the cards' pictures, in view */
        for (int i = 0; i < S.list.n; i++) {
            int x0, y0, x1, y1, art;
            char key[320];
            const char *pic;
            tile_box(i, &x0, &y0, &x1, &y1);
            if (x0 < 0 || S.disp[i].poster || y0 > vtop || y1 < vbot - 400)
                continue;
            if (!(pic = home_pic(i, key, sizeof(key), &art)))
                continue;
            if (!(S.disp[i].poster = cache_find(key))) {
                int c, r = home_row_of(i, &c);
                if (!(S.disp[i].poster = poster_soon(pic, key, S.home.lay[r].cw >> S.xeig, S.home.lay[r].ch >> S.yeig, art)))
                    return 1;       /* on its way */
                cache_trim();
            }
            redraw_tile(i);
            return 1;
        }
        S.posters_wanted = 0;
        return 0;
    }
    if (S.show.on) {                /* the episodes' pictures, in view and a row more */
        for (int i = 0; i < S.list.n; i++) {
            const plex_item *it = &S.list.v[i];
            int x0, y0, x1, y1;
            char key[300];
            tile_box(i, &x0, &y0, &x1, &y1);
            if (y1 < vbot - EP_ROW)
                break;
            if (!it->thumb || S.disp[i].poster || y0 > vtop)
                continue;
            ep_key(it, key, sizeof(key));
            if (!(S.disp[i].poster = cache_find(key))) {
                if (!(S.disp[i].poster = poster_soon(it->thumb, key, EP_TW >> S.xeig, EP_TH >> S.yeig, 3)))
                    return 1;       /* on its way */
                cache_trim();
            }
            redraw_tile(i);
            return 1;
        }
        S.posters_wanted = 0;
        return 0;
    }
    rf = (-HEADER_H - libbar_h() - GAP - vtop) / (TILE_H + GAP);
    rl = (-HEADER_H - libbar_h() - GAP - vbot) / (TILE_H + GAP) + 1;     /* and a row more */
    if (rf < 0)
        rf = 0;
    for (int i = rf * S.cols; i < (rl + 1) * S.cols && i < S.list.n; i++) {
        const plex_item *it = &S.list.v[i];
        if (!it->thumb || S.disp[i].poster)
            continue;
        if (!(S.disp[i].poster = cache_find(it->thumb))) {
            if (!(S.disp[i].poster = poster_soon(it->thumb, it->thumb, TILE_W >> S.xeig, POSTER_H >> S.yeig, 0)))
                return 1;           /* on its way */
            cache_trim();
        }
        /* the same poster may stand for others (a show's episodes on deck) */
        for (int j = i + 1; j < S.list.n; j++)
            if (!S.disp[j].poster && S.list.v[j].thumb && !strcmp(S.list.v[j].thumb, it->thumb))
                S.disp[j].poster = S.disp[i].poster;
        redraw_tile(i);
        return 1;
    }
    S.posters_wanted = 0;
    return 0;
}

/* ---- the show page ------------------------------------------------------------------

   A show opens as a page of its own (as Plex's apps show one): its
   backdrop across the top, its poster, title, labels and summary, Play
   (the next episode) and Mark watched, then its series as tabs and the
   series chosen as a list of episodes, each a 16:9 picture with its title,
   how long it is (and how much is left) and two lines of its summary. The
   list is S.list (the series' episodes), so selecting, opening, the menus
   and Back work as on the grid; S.show.on says how it's laid out. */

void show_fetch_art(void)
{
    const plex_item *it = S.show.have ? &S.show.det.v[0] : NULL;
    char key[320];
    S.show.art = S.show.poster = NULL;
    S.show.fw = S.show.w;
    S.show.due = 0;
    if (it && it->thumb) {
        snprintf(key, sizeof(key), "dposter:%s@%dx%d", it->thumb, S.show.pw, S.show.ph);
        if (!(S.show.poster = cache_find(key)))
            S.show.poster = poster_fetch(it->thumb, key, S.show.pw >> S.xeig, S.show.ph >> S.yeig, 0);
    }
    if (it && it->art) {
        snprintf(key, sizeof(key), "art:%s@%dx%d", it->art, S.show.art_w, S.show.art_h);
        if (!(S.show.art = cache_find(key)))
            S.show.art = poster_fetch(it->art, key, S.show.art_w >> S.xeig, S.show.art_h >> S.yeig, 1);
    }
    cache_trim();
}

static void show_chip(const char *t, int *x, int y)
{
    int w = draw_width(D_BOLD, t) + 44;
    if (S.show.nchip >= 8)
        return;
    S.show.chip[S.show.nchip].x0 = *x;
    S.show.chip[S.show.nchip].x1 = *x + w;
    S.show.chip[S.show.nchip].y0 = y - 56;
    snprintf(S.show.chip[S.show.nchip].t, sizeof(S.show.chip[0].t), "%s", t);
    S.show.nchip++;
    *x += w + 16;
}

static void show_button(int id, const char *t, int *x, int y)
{
    int w = draw_width(D_BOLD, t) + 64 + (id == SH_PLAY ? 36 : 0);
    S.show.btn[S.show.nbtn].id = id;
    S.show.btn[S.show.nbtn].x0 = *x;
    S.show.btn[S.show.nbtn].x1 = *x + w;
    S.show.btn[S.show.nbtn].y1 = y;
    S.show.btn[S.show.nbtn].y0 = y - BTN;
    snprintf(S.show.btn[S.show.nbtn].t, sizeof(S.show.btn[0].t), "%s", t);
    S.show.nbtn++;
    *x += w + 20;
}

/* Where everything on the show page goes, for the window's width */
void show_layout(void)
{
    const plex_item *it = &S.show.det.v[0], *se = &S.show.seasons.v[S.show.season];
    int st[9], w = vis_width(), wh = 1100, x, y, top = -HEADER_H, foot, sw, unw = 0;
    char t[400], g[400];
    if (S.browser_open) {
        window_state(S.browser_w, st);
        wh = st[4] - st[2];
    }
    S.show.w = w;
    S.show.art_w = w & ~((1 << S.xeig) - 1);
    S.show.art_h = w * 9 / 16 < wh * 45 / 100 ? w * 9 / 16 : wh * 45 / 100;
    if (S.show.art_h < 300)
        S.show.art_h = 300;
    S.show.art_h &= ~((1 << S.yeig) - 1);
    S.show.pw = w * 13 / 100 < 200 ? 200 : w * 13 / 100 > 360 ? 360 : w * 13 / 100;
    S.show.pw &= ~((2 << S.xeig) - 1);
    S.show.ph = (S.show.pw * 3 / 2) & ~((1 << S.yeig) - 1);
    if ((S.show.art || S.show.poster) && S.show.fw != w)
        S.show.due = now_cs() + 50;     /* the pictures at the new size, once resizing stops */
    S.show.tx = 40 + S.show.pw + 56;
    latin1(it->title, S.show.title, sizeof(S.show.title));
    draw_fit(D_TITLE, S.show.title, w - S.show.tx - 40);
    S.show.title_y = top - 64 - 48;
    /* the labels: first year, how many series, the age rating, the score, unwatched */
    S.show.nchip = 0;
    x = S.show.tx;
    y = S.show.title_y - 44;
    for (int i = 0; i < S.show.seasons.n; i++)
        unw += S.show.seasons.v[i].unwatched;
    if (it->year > 0) {
        snprintf(t, sizeof(t), "%d", it->year);
        show_chip(t, &x, y);
    }
    snprintf(t, sizeof(t), "%d series", S.show.seasons.n);
    show_chip(t, &x, y);
    if (it->content_rating) {
        latin1(strchr(it->content_rating, '/') ? strchr(it->content_rating, '/') + 1 : it->content_rating, g,
               sizeof(g));             /* "gb/18": 18 */
        show_chip(g, &x, y);
    }
    if (it->rating > 0) {
        snprintf(t, sizeof(t), "%.1f", it->rating);
        show_chip(t, &x, y);
    }
    if (unw > 0) {
        snprintf(t, sizeof(t), "%d unwatched", unw);
        show_chip(t, &x, y);
    }
    y -= 56 + 36;
    /* the summary: three lines, over the backdrop's solid part */
    {
        static char sum[2400];
        latin1(it->summary ? it->summary : "", sum, sizeof(sum));
        sw = w * 52 / 100 - S.show.tx;
        if (sw < 900)
            sw = w - S.show.tx - 40 < 900 ? w - S.show.tx - 40 : 900;
        S.show.nlines = draw_wrap(D_BODY, sum, sw, S.show.lines, 3);
    }
    S.show.sum_y = y - 12;
    y = S.show.sum_y - S.show.nlines * 40 - (S.show.nlines ? 24 : 0);
    /* Play (the episode in progress, or the first not seen), Mark watched */
    S.show.nbtn = 0;
    S.show.next = 0;
    for (int i = 0; i < S.list.n; i++)
        if (S.list.v[i].view_offset_ms > 0 || !S.list.v[i].watched) {
            S.show.next = i;
            break;
        }
    x = S.show.tx;
    if (S.list.n) {
        const plex_item *e = &S.list.v[S.show.next];
        snprintf(t, sizeof(t), "%s S%d E%d", e->view_offset_ms > 0 ? "Resume" : "Play", e->parent_index, e->index);
        show_button(SH_PLAY, t, &x, y);
    }
    show_button(SH_WATCHED, it->watched ? "Mark unwatched" : "Mark watched", &x, y);
    y -= BTN + 40;
    foot = top - 64 - S.show.ph - 40;
    if (y > foot)
        y = foot;
    /* the series, as tabs */
    S.show.ntab = 0;
    x = 40;
    for (int i = 0; i < S.show.seasons.n && S.show.ntab < 12; i++) {
        int tw;
        latin1(S.show.seasons.v[i].title, t, sizeof(t));
        snprintf(S.show.tab[S.show.ntab].t, sizeof(S.show.tab[0].t), "%s", t);
        tw = draw_width(D_BOLD, S.show.tab[S.show.ntab].t) + 56;
        if (x + tw > w - 40 && x > 40) {
            x = 40;
            y -= 56 + 16;
        }
        S.show.tab[S.show.ntab].x0 = x;
        S.show.tab[S.show.ntab].x1 = x + tw;
        S.show.tab[S.show.ntab].y1 = y;
        S.show.tab[S.show.ntab].y0 = y - 56;
        S.show.ntab++;
        x += tw + 16;
    }
    if (se->unwatched > 0)
        snprintf(S.show.count, sizeof(S.show.count), "%d episode%s   \xb7   %d unwatched", S.list.n,
                 S.list.n == 1 ? "" : "s", se->unwatched);
    else
        snprintf(S.show.count, sizeof(S.show.count), "%d episode%s", S.list.n, S.list.n == 1 ? "" : "s");
    if (x + draw_width(D_BODY, S.show.count) + 24 > w - 40) {
        x = 40;
        y -= 56 + 16;
    }
    S.show.count_x = x + 8;
    S.show.count_y = y - 38;
    S.show.head_h = -(y - 56 - 40);
}

/* The show page's header: the backdrop, the poster, the texts, the buttons, the tabs */
static void show_redraw(int ox, int oy, int cy0, int cy1)
{
    int top = -HEADER_H, px0 = 40, py1 = top - 64, py0 = py1 - S.show.ph;
    if (cy1 > top - S.show.art_h && cy0 < top) {
        if (S.show.art && S.show.art->area)
            plot_sprite(S.show.art, 0, top - S.show.art_h, S.show.art_w, top);
        else
            draw_rect(ox, oy + top - S.show.art_h, ox + S.show.w, oy + top, RGB(30, 33, 39));
    }
    if (cy1 > py0 - 12 && cy0 < py1) {
        draw_round(ox + px0 + 8, oy + py0 - 12, ox + px0 + S.show.pw + 8, oy + py1 - 8, 16, C_SHADOW, C_BG);
        if (S.show.poster && S.show.poster->area && S.show.fw == S.show.w)
            plot_sprite(S.show.poster, px0, py0, px0 + S.show.pw, py1);
        else
            draw_round(ox + px0, oy + py0, ox + px0 + S.show.pw, oy + py1, 12, C_CARD, C_BG);
        badges(&S.show.det.v[0], ox + px0, oy + py0, ox + px0 + S.show.pw, oy + py1);
    }
    draw_text_over(D_TITLE, ox + S.show.tx, oy + S.show.title_y, S.show.title, C_TEXT);
    for (int i = 0; i < S.show.nchip; i++) {
        int y0 = oy + S.show.chip[i].y0;
        draw_round(ox + S.show.chip[i].x0, y0, ox + S.show.chip[i].x1, y0 + 56, 28, C_CHIP, DRAW_NONE);
        draw_text(D_BOLD, ox + S.show.chip[i].x0 + 22, y0 + 18, S.show.chip[i].t, C_TEXT, C_CHIP);
    }
    for (int l = 0; l < S.show.nlines; l++)
        draw_text(D_BODY, ox + S.show.tx, oy + S.show.sum_y - l * 40, S.show.lines[l], C_TEXT, C_BG);
    for (int i = 0; i < S.show.nbtn; i++) {
        int x0 = ox + S.show.btn[i].x0, y0 = oy + S.show.btn[i].y0, x1 = ox + S.show.btn[i].x1, y1 = oy + S.show.btn[i].y1;
        unsigned bg = S.show.btn[i].id == SH_PLAY ? C_ACCENT : C_CARD;
        int tx = x0 + 32;
        draw_round(x0, y0, x1, y1, BTN / 2, bg, DRAW_NONE);
        if (S.show.btn[i].id == SH_PLAY) {
            draw_glyph(G_PLAY, tx - 4, y0 + 16, tx + 28, y1 - 16, C_TEXT, bg);
            tx += 36;
        }
        draw_text(D_BOLD, tx, y0 + 22, S.show.btn[i].t, C_TEXT, bg);
    }
    for (int i = 0; i < S.show.ntab; i++) {         /* the series shown: light, the others dark */
        int on = i == S.show.season, y0 = oy + S.show.tab[i].y0;
        unsigned bg = on ? C_TEXT : C_CARD;
        draw_round(ox + S.show.tab[i].x0, y0, ox + S.show.tab[i].x1, y0 + 56, 28, bg, C_BG);
        draw_text(D_BOLD, ox + S.show.tab[i].x0 + 28, y0 + 18, S.show.tab[i].t, on ? C_HEADER : C_TEXT, bg);
    }
    draw_text(D_BODY, ox + S.show.count_x, oy + S.show.count_y, S.show.count, C_SUB, C_BG);
}

/* An episode's row on the show page */
static void draw_episode(int i, int ox, int oy)
{
    const plex_item *it = &S.list.v[i];
    disp_t *d = &S.disp[i];
    int x0, y0, x1, y1, px0, py0, py1, tx, ty;
    unsigned rowbg = i == S.sel ? RGB(36, 39, 47) : i == S.hover ? RGB(30, 32, 39) : C_BG;
    tile_box(i, &x0, &y0, &x1, &y1);
    if (i == S.sel) {                               /* a light edge, as round a selected poster */
        draw_round(ox + x0 - 6, oy + y0 - 6, ox + x1 + 6, oy + y1 + 6, 30, C_TEXT, C_BG);
        draw_round(ox + x0, oy + y0, ox + x1, oy + y1, 24, rowbg, C_TEXT);
    } else if (rowbg != C_BG)
        draw_round(ox + x0, oy + y0, ox + x1, oy + y1, 24, rowbg, C_BG);
    px0 = x0 + 16;
    py1 = y1 - 20;
    py0 = py1 - EP_TH;
    if (d->poster && d->poster->area)
        plot_sprite(d->poster, px0, py0, px0 + EP_TW, py1);
    else
        draw_round(ox + px0, oy + py0, ox + px0 + EP_TW, oy + py1, 12, C_CARD, rowbg);
    badges(it, ox + px0, oy + py0, ox + px0 + EP_TW, oy + py1);
    tx = px0 + EP_TW + 40;
    ty = py1 - 36;
    draw_text(D_BOLD, ox + tx, oy + ty, d->line[0], C_TEXT, rowbg);
    draw_text(D_BODY, ox + tx + draw_width(D_BOLD, d->line[0]) + 40, oy + ty, d->line[1], C_SUB, rowbg);
    for (int l = 0; l < 2; l++)
        draw_text(D_BODY, ox + tx, oy + ty - 52 - l * 40, d->sum[l], RGB(205, 208, 214), rowbg);
}

/* A tab or a button on the show page, at a screen point: SH_TAB + the
   tab's number, SH_PLAY, SH_WATCHED; or 0 */
int show_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int i = 0; i < S.show.ntab; i++)
        if (wx >= S.show.tab[i].x0 && wx < S.show.tab[i].x1 && wy >= S.show.tab[i].y0 && wy < S.show.tab[i].y1)
            return SH_TAB + i;
    for (int i = 0; i < S.show.nbtn; i++)
        if (wx >= S.show.btn[i].x0 && wx < S.show.btn[i].x1 && wy >= S.show.btn[i].y0 && wy < S.show.btn[i].y1)
            return S.show.btn[i].id;
    return 0;
}

/* ---- a library's bar ------------------------------------------------------------------

   Over a library's posters: Library, Collections and Playlists (the
   videos' playlists), then Sort (Title, Date added, Year, Rating) and
   Unwatched; under them, A to Z: a letter jumps to the first title
   starting with it (sorted by title). They're the list's path, so Back and
   Refresh keep them: /library/sections/<n>/all?sort=...&unwatched=1,
   /library/sections/<n>/collections, /playlists?playlistType=video. */

const char *const sort_names[] = { "Title", "Date added", "Year", "Rating" };
static const char *const sort_keys[] = { "", "addedAt:desc", "year:desc", "rating:desc" };

int libbar_h(void)
{
    return S.lib.on && S.page == PG_GRID ? (S.lib.h ? S.lib.h : LIBBAR_H) : 0;
}

/* What the list's path says */
static void lib_parse(const char *path)
{
    int sec;
    const char *q = strchr(path, '?');
    S.lib.on = 0;
    if (sscanf(path, "/library/sections/%d/", &sec) == 1 &&
        (strstr(path, "/all") || strstr(path, "/collections"))) {
        S.lib.on = 1;
        S.lib.sec = sec;
        S.lib.view = strstr(path, "/collections") ? 1 : 0;
    } else if (!strncmp(path, "/playlists", 10) && S.lib.sec) {
        S.lib.on = 1;
        S.lib.view = 2;
    }
    S.lib.sort = 0;
    S.lib.unwatched = 0;
    for (int k = 1; q && k < SORTS; k++)
        if (strstr(q, sort_keys[k]))
            S.lib.sort = k;
    S.lib.unwatched = q && strstr(q, "unwatched=1") != NULL;
}

void lib_path(int view, int sort, int unwatched, char *out, size_t size)
{
    if (view == 2)
        snprintf(out, size, "/playlists?playlistType=video");
    else if (view == 1)
        snprintf(out, size, "/library/sections/%d/collections", S.lib.sec);
    else
        snprintf(out, size, "/library/sections/%d/all%s%s%s%s", S.lib.sec, sort || unwatched ? "?" : "",
                 sort ? "sort=" : "", sort ? sort_keys[sort] : "", unwatched ? (sort ? "&unwatched=1" : "unwatched=1") : "");
}

/* A title's letter for A to Z: 0 for # (not a letter), 1-26; "The", "A"
   and "An" left off, as Plex sorts them */
int az_letter(const char *t)
{
    if (!strncasecmp(t, "The ", 4)) t += 4;
    else if (!strncasecmp(t, "An ", 3)) t += 3;
    else if (!strncasecmp(t, "A ", 2)) t += 2;
    return (*t >= 'a' && *t <= 'z') ? *t - 'a' + 1 : (*t >= 'A' && *t <= 'Z') ? *t - 'A' + 1 : 0;
}

static void lib_button(int id, const char *t, int *x, int y, int right)
{
    int w = draw_width(D_BOLD, t) + 56 + (id >= LB_SORT && id < LB_UNWATCHED ? 40 : 0);
    if (S.lib.nbtn >= 6)
        return;
    if (right)
        *x -= w;
    S.lib.btn[S.lib.nbtn].id = id;
    S.lib.btn[S.lib.nbtn].x0 = *x;
    S.lib.btn[S.lib.nbtn].x1 = *x + w;
    S.lib.btn[S.lib.nbtn].y1 = y;
    S.lib.btn[S.lib.nbtn].y0 = y - 56;
    snprintf(S.lib.btn[S.lib.nbtn].t, sizeof(S.lib.btn[0].t), "%s", t);
    S.lib.nbtn++;
    if (right)
        *x -= 16;
    else
        *x += w + 16;
}

void lib_layout(void)
{
    int x = GAP, y = -HEADER_H - 16, w = vis_width();
    char t[64];
    S.lib.nbtn = 0;
    lib_button(LB_VIEW + 0, "Library", &x, y, 0);
    lib_button(LB_VIEW + 1, "Collections", &x, y, 0);
    lib_button(LB_VIEW + 2, "Playlists", &x, y, 0);
    S.lib.h = LIBBAR_H;
    if (S.lib.view == 0) {
        int xr = w - GAP, need;
        snprintf(t, sizeof(t), "Sort: %s", sort_names[S.lib.sort]);
        need = draw_width(D_BOLD, t) + 96 + 16 + draw_width(D_BOLD, "Unwatched") + 56 + 16;
        if (xr - need < x) {        /* too narrow: on a line of their own */
            xr = w - GAP;
            y -= 56 + 16;
            S.lib.h = LIBBAR_H + 56 + 16;
        }
        lib_button(LB_UNWATCHED, "Unwatched", &xr, y, 1);
        lib_button(LB_SORT, t, &xr, y, 1);
    }
    /* A to Z: each letter's first title (sorted by title only) */
    for (int k = 0; k < 27; k++)
        S.lib.az_first[k] = -1;
    if (S.lib.view == 0 && S.lib.sort == 0)
        for (int i = S.list.n - 1; i >= 0; i--)
            S.lib.az_first[az_letter(S.list.v[i].title)] = i;
    S.lib.az_y = y - 56 - 56;
}

/* A letter's box: k 0 (#) to 26 (Z) */
void az_box(int k, int *x0, int *y0, int *x1, int *y1)
{
    int w = vis_width(), step = (w - 2 * GAP) / 27;
    if (step > 56)
        step = 56;
    *x0 = GAP + k * step;
    *x1 = *x0 + step;
    *y1 = S.lib.az_y + 40;
    *y0 = S.lib.az_y - 16;
}

static void lib_redraw(int ox, int oy)
{
    for (int i = 0; i < S.lib.nbtn; i++) {
        int id = S.lib.btn[i].id, on = (id >= LB_VIEW && id < LB_VIEW + 3 && id - LB_VIEW == S.lib.view) ||
                                     (id == LB_UNWATCHED && S.lib.unwatched);
        unsigned bg = on ? C_TEXT : C_CARD;
        int x0 = ox + S.lib.btn[i].x0, y0 = oy + S.lib.btn[i].y0, x1 = ox + S.lib.btn[i].x1;
        draw_round(x0, y0, x1, y0 + 56, 28, bg, C_BG);
        draw_text(D_BOLD, x0 + 28, y0 + 18, S.lib.btn[i].t, on ? C_HEADER : C_TEXT, bg);
        if (id == LB_SORT)
            draw_glyph(G_DOWN, x1 - 52, y0 + 14, x1 - 24, y0 + 42, C_TEXT, bg);
    }
    if (S.lib.view == 0 && S.lib.sort == 0 && S.list.n > 0)
        for (int k = 0; k < 27; k++) {
            int x0, y0, x1, y1;
            char l[2] = { k ? (char)('A' + k - 1) : '#', 0 };
            az_box(k, &x0, &y0, &x1, &y1);
            draw_text(D_BOLD, ox + (x0 + x1 - draw_width(D_BOLD, l)) / 2, oy + S.lib.az_y, l,
                      S.lib.az_first[k] >= 0 ? C_TEXT : RGB(70, 74, 84), C_BG);
        }
}

/* A button or a letter at a screen point: LB_*, LB_AZ + k; or 0 */
int lib_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int i = 0; i < S.lib.nbtn; i++)
        if (wx >= S.lib.btn[i].x0 && wx < S.lib.btn[i].x1 && wy >= S.lib.btn[i].y0 && wy < S.lib.btn[i].y1)
            return S.lib.btn[i].id;
    if (S.lib.view == 0 && S.lib.sort == 0 && S.list.n > 0)
        for (int k = 0; k < 27; k++) {
            int x0, y0, x1, y1;
            az_box(k, &x0, &y0, &x1, &y1);
            if (wx >= x0 && wx < x1 && wy >= y0 && wy < y1)
                return LB_AZ + k;
        }
    return 0;
}

/* ---- the browser: lists --------------------------------------------------------- */

void set_where(void)
{
    char t[400];
    size_t n = 0;
    t[0] = 0;
    if (S.page == PG_SIGNIN) {
        snprintf(S.where, sizeof(S.where), "Sign in");
        snprintf(S.title, sizeof(S.title), APP);
    } else if (S.page == PG_PLAYER && S.pl.det.n) {
        latin1(S.pl.det.v[0].title, t, sizeof(t));
        snprintf(S.title, sizeof(S.title), "%s: %s", APP, t);
    } else {
        for (int i = 0; i < S.nhist && n < sizeof(t); i++)
            n += snprintf(t + n, sizeof(t) - n, "%s > ", S.hist[i].title);
        if (n < sizeof(t))
            n += snprintf(t + n, sizeof(t) - n, "%s", S.list.title);
        if (S.page == PG_DETAILS && S.have_det && n < sizeof(t))
            snprintf(t + n, sizeof(t) - n, " > %s", S.det.v[0].title);
        latin1(t, S.where, sizeof(S.where));
        latin1(S.px.server_name, t, sizeof(t));
        if (*S.user_title && S.nusers > 1) {        /* Plex Home: who's watching */
            char u[80];
            latin1(S.user_title, u, sizeof(u));
            snprintf(S.title, sizeof(S.title), "%s: %s (%s)", APP, t, u);
        } else
            snprintf(S.title, sizeof(S.title), "%s: %s", APP, t);
    }
    if (S.browser_open) {
        _kernel_swi_regs r;         /* the title bar, redrawn (RISC OS 5) */
        r.r[0] = S.browser_w;
        r.r[1] = 0x4B534154;
        r.r[2] = 3;
        swi(Wimp_ForceRedraw, &r);
        force_redraw(S.browser_w, 0, -HEADER_H, S.scr_w, 0);
    }
}

/* The top of the icon bar (window -2), OS units */
static int iconbar_top(void)
{
    int b[9];
    window_state(-2, b);
    return b[4] > 0 && b[4] < 512 ? b[4] : 134;
}

/* The window: 75% of the screen's width and of the height above the icon
   bar, in the middle of that (its title bar and scroll bar included) */
#define TITLE_H     40              /* the Wimp's title bar and scroll bar (about) */
void browser_open(void)
{
    int st[9], ib = iconbar_top(), w = S.scr_w * WIN_PERCENT / 100 - SCROLL_W,
        h = (S.scr_h - ib) * WIN_PERCENT / 100 - TITLE_H;
    w &= ~((1 << S.xeig) - 1);
    h &= ~((1 << S.yeig) - 1);
    if (player_mini()) {            /* playing in the mini player: the window again */
        player_set_mini(0);
        choices_save();
        return;
    }
    if (S.browser_open) {
        window_state(S.browser_w, st);
        open_front(S.browser_w, st[1], st[2], st[3], st[4], st[5], st[6]);
    } else {
        int x0 = (S.scr_w - w - SCROLL_W) / 2, y0 = ib + (S.scr_h - ib - h - TITLE_H) / 2;
        S.cols = layout_cols(w);
        S.width = w;
        set_extent();
        open_front(S.browser_w, x0, y0, x0 + w, y0 + h, 0, 0);
        S.browser_open = 1;
    }
    set_caret(S.browser_w, -1, NULL);
    S.posters_wanted = S.page == PG_GRID;
}

/* Shows the list at path ("" = the top). push: remember the one shown, for
   Back. 0 = ok. */
int show_list(const char *path, const char *back_title, int push, int sel)
{
    plex_list l;
    plex_row rows[8];
    int e, nrows = 0;
    hourglass(1);
    if (!*path) {                   /* the top: the home page */
        plex_list libs;
        e = plex_home(&S.px, &l, rows, 8, &nrows, &libs);
        if (e == 0) {
            if (S.libs.n || S.libs.v)
                plex_list_free(&S.libs);
            S.libs = libs;          /* the tabs */
        }
    } else
        e = plex_list_get(&S.px, path, &l);
    hourglass(0);
    if (e != 0) {
        report("Can't get the list: %s", S.px.err);
        return -1;
    }
    if (push && S.have_list) {
        if (S.nhist == HISTORY_MAX) {
            memmove(&S.hist[0], &S.hist[1], sizeof(S.hist[0]) * (HISTORY_MAX - 1));
            S.nhist--;
        }
        snprintf(S.hist[S.nhist].path, sizeof(S.hist[0].path), "%s", S.path);
        snprintf(S.hist[S.nhist].title, sizeof(S.hist[0].title), "%s", back_title);
        S.hist[S.nhist].sel = S.sel;
        S.nhist++;
    }
    if (S.have_list)
        plex_list_free(&S.list);
    free(S.disp);                   /* the old list's (cache_trim reads it): made again below */
    S.disp = NULL;
    S.list = l;
    S.have_list = 1;
    S.det_i = -1;                   /* the details (if kept) are of a video in the old list */
    S.hover = -1;
    S.page = PG_GRID;
    snprintf(S.path, sizeof(S.path), "%s", path);
    if (!strncmp(path, "search:", 7))       /* the field shows what was searched for */
        latin1(path + 7, S.query, sizeof(S.query));
    S.sel = sel >= 0 && sel < l.n ? sel : l.n ? 0 : -1;
    S.sel_want = sel >= l.n && sel < l.total ? sel : -1;
    /* a library's list: its bar */
    lib_parse(path);
    /* the top: the home page */
    S.home.on = !*path;
    if (S.home.on) {
        memcpy(S.home.row, rows, sizeof(rows));
        S.home.nrows = nrows;
        S.home.art = NULL;
        S.home.pick = 0;
        S.cols = 1;
        home_layout();
        hourglass(1);
        home_fetch_art();
        hourglass(0);
    }
    /* one of the show's series: the show page */
    S.show.on = 0;
    for (int i = 0; S.show.have && i < S.show.seasons.n; i++)
        if (S.show.seasons.v[i].key && !strcmp(S.show.seasons.v[i].key, path)) {
            S.show.on = 1;
            S.show.season = i;
        }
    if (S.show.on) {
        snprintf(S.list.title, sizeof(S.list.title), "%s", S.show.det.v[0].title);
        S.cols = 1;
        show_layout();
        if (!S.show.art && !S.show.poster) {
            hourglass(1);
            show_fetch_art();
            hourglass(0);
        }
    } else if (!S.home.on && (S.cols == 1 || S.cols != layout_cols(vis_width()))) {
        S.cols = layout_cols(vis_width());
    }
    make_disp();
    if (S.lib.on)
        lib_layout();
    /* a library of more than one fetch: the rest, a page at a time, between other things */
    S.more_wanted = l.total > l.n && *path && strncmp(path, "search:", 7) && !S.home.on && !S.show.on;
    set_where();
    if (!S.browser_open)
        browser_open();
    else {
        int st[9];
        set_extent();
        window_state(S.browser_w, st);
        open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);    /* back to the top */
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
        set_caret(S.browser_w, -1, NULL);
    }
    S.posters_wanted = 1;
    if (!strncmp(path, "search:", 7))
        set_status(path[7] ? "%d found." : "Type what to look for.", l.n);
    else if (!l.n)
        set_status("Nothing here.");
    else if (l.total > l.n)
        set_status(S.more_wanted ? "%d of %d: the rest coming." : "The first %d of %d.", l.n, l.total);
    else
        set_status("%d item%s.", l.n, l.n == 1 ? "" : "s");
    return 0;
}

void go_back(void)
{
    hist_t h;
    if (S.page == PG_DETAILS) {
        det_leave();
        return;
    }
    if (!S.nhist || S.page != PG_GRID)
        return;
    h = S.hist[--S.nhist];
    if (show_list(h.path, "", 0, h.sel) != 0)
        S.nhist++;                  /* still where it was */
}

/* ---- search ------------------------------------------------------------------------

   The Search button (or / or Ctrl-F on the grid) shows an empty list with
   a field in the bar; typing goes into it, and a moment after the last key
   (SEARCH_WAIT) the server is asked (/hubs/search, every library). The
   results are a list like any other ("search:<words>"), so Back returns to
   them from a film's details or a show, and Refresh asks again. */

#define SEARCH_WAIT 40              /* cs after the last key */

/* Latin-1 (typed) as UTF-8 (for the server) */
static void to_utf8(const char *s, char *out, size_t size)
{
    size_t o = 0;
    for (; *s && o + 3 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80)
            out[o++] = (char)c;
        else {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

void search_now(void)
{
    char path[256];
    size_t n = snprintf(path, sizeof(path), "search:");
    to_utf8(S.query, path + n, sizeof(path) - n);
    S.search_due = 0;
    show_list(path, "", 0, 0);
}

void search_open(void)
{
    if (searching())
        return;
    S.query[0] = 0;
    S.search_due = 0;
    show_list("search:", S.list.title[0] ? S.list.title : "Home", S.have_list, 0);
}

/* A key on the search page: 1 if it was the field's */
int search_key(int k)
{
    size_t n = strlen(S.query);
    if ((k >= 32 && k < 127) || (k >= 160 && k <= 255)) {
        if (n + 1 < sizeof(S.query)) {
            S.query[n] = (char)k;
            S.query[n + 1] = 0;
        }
    } else if (k == 8 || k == 0x7F) {
        if (!n)
            return 0;               /* an empty field: Back */
        S.query[n - 1] = 0;
    } else if (k == 21) {           /* Ctrl-U */
        S.query[0] = 0;
    } else if (k == 13 && S.search_due) {
        search_now();
        return 1;
    } else
        return 0;
    S.search_due = now_cs() + SEARCH_WAIT;
    if (!S.search_due)
        S.search_due = 1;
    force_redraw(S.browser_w, 0, -HEADER_H, S.scr_w, 0);
    return 1;
}

void refresh_list(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s", S.path);
    show_list(path, "", 0, S.sel);
}

/* A tab: Home, or a library (Back from it goes Home) */
void tab_open(int k)
{
    const plex_item *lib;
    char path[256];
    if (k == 0) {
        browser_top();
        return;
    }
    if (k < 1 || k > S.libs.n)
        return;
    lib = &S.libs.v[k - 1];
    if (lib->kind != PI_FOLDER || !lib->key) {
        set_status("%s libraries can't be opened yet: films and TV only.", lib->subtitle ? lib->subtitle : "These");
        return;
    }
    snprintf(path, sizeof(path), "%s", lib->key);
    S.nhist = 1;
    snprintf(S.hist[0].path, sizeof(S.hist[0].path), "%s", "");
    snprintf(S.hist[0].title, sizeof(S.hist[0].title), "%s", S.px.server_name);
    S.hist[0].sel = 0;
    if (show_list(path, "", 0, 0) != 0)
        S.nhist = 0;
}

/* The list shown let go, and the pages made from it (home, show, library bar) off */

void list_forget(void)
{
    if (S.have_list)
        plex_list_free(&S.list);
    S.have_list = 0;
    free(S.disp);
    S.disp = NULL;
    S.home.on = S.show.on = S.lib.on = 0;
    S.home.nrows = S.home.npick = 0;
    S.more_wanted = 0;
    S.sel_want = -1;
}

void browser_top(void)
{
    if (S.nusers < 0 && *S.px.account_token)
        users_get();                /* Plex Home: who's watching, for the title */
    S.nhist = 0;
    if (S.page == PG_PLAYER)
        builtin_stop(1);            /* not under the grid */
    list_forget();
    show_list("", "", 0, 0);
}

int tile_at(int sx, int sy)
{
    int st[9], wx, wy;
    if (!S.have_list)
        return -1;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    if (!S.home.on && !S.show.on && S.cols > 0) {
        /* the grid: the row and column under the point (10 times a second
           while the pointer's over the window: no walk through a big list) */
        int top = -HEADER_H - libbar_h() - GAP + GAP / 2, row, col, i, x0, y0, x1, y1;
        if (wy > top || wx < GAP / 2)
            return -1;
        row = (top - wy) / (TILE_H + GAP);
        col = (wx - GAP / 2) / (TILE_W + GAP);
        i = row * S.cols + col;
        if (col >= S.cols || i >= S.list.n)
            return -1;
        tile_box(i, &x0, &y0, &x1, &y1);
        return wx >= x0 - GAP / 2 && wx < x1 + GAP / 2 && wy >= y0 - GAP / 2 && wy < y1 + GAP / 2 ? i : -1;
    }
    for (int i = 0; i < S.list.n; i++) {
        int x0, y0, x1, y1;
        tile_box(i, &x0, &y0, &x1, &y1);
        if (!S.home.on && y1 < wy - (S.show.on ? EP_ROW : TILE_H) - GAP)
            break;
        if (wx >= x0 - GAP / 2 && wx < x1 + GAP / 2 && wy >= y0 - GAP / 2 && wy < y1 + GAP / 2)
            return i;
    }
    return -1;
}

/* The header button at a screen point: B_BACK, B_REFRESH or 0 */
int header_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int id = B_BACK; id <= B_SEARCH; id++) {
        int x0, y0, x1, y1;
        if ((id > B_REFRESH && id < B_SEARCH) || !header_has(id))
            continue;
        header_button(id, st[3] - st[1], &x0, &y0, &x1, &y1);
        if (wx >= x0 && wx < x1 && wy >= y0 && wy < y1)
            return id;
    }
    return 0;
}

void set_hover(int i)
{
    int old = S.hover;
    if (i == old)
        return;
    S.hover = i;
    redraw_tile(old);
    redraw_tile(i);
}

void select_tile(int i)
{
    int old = S.sel;
    S.sel_want = -1;                /* chosen now: not the one Back came to */
    if (i == old)
        return;
    S.sel = i;
    redraw_tile(old);
    redraw_tile(i);
    if (i >= 0) {                   /* keep it in view */
        int st[9], x0, y0, x1, y1, vh;
        window_state(S.browser_w, st);
        tile_box(i, &x0, &y0, &x1, &y1);
        vh = st[4] - st[2];
        if (y1 + GAP > st[6] || y0 - GAP < st[6] - vh) {
            int sy = y1 + GAP > st[6] ? (i < S.cols ? 0 : y1 + GAP) : y0 - GAP + vh;
            open_front(S.browser_w, st[1], st[2], st[3], st[4], st[5], sy);
            S.posters_wanted = 1;
        }
    }
}
