/*
 * ui.c - Matinee's desktop front end.
 *
 * An icon on the icon bar. Select on it opens the browser (or, before
 * signing in, the sign-in window); Menu gives:
 *
 *   Info >, Sign in..., Servers >, Player > (ReelEGL / Reel),
 *   Quality > (1080p / 720p / 480p), Poster size > (Small / Medium /
 *   Large), Direct play when possible, Sign out, Quit
 *
 * Sign-in window: a code to type at plex.tv/link (checked every 2
 * seconds), or a server's address and token typed by hand.
 *
 * Browser window, dark, like a TV app (draw.c draws it all): a bar with
 * Back, where you are, a status line and Refresh, above a grid of posters
 * with shadows. Click selects (a blue ring; the pointer's poster gets a
 * grey one); double-click opens a library, show or season, or a video's
 * details; Return opens or plays; Backspace or Escape goes back; the arrow
 * keys move the selection; I shows the details. Menu on a poster: Play,
 * Details, Resume from h:mm:ss, Play from start, Subtitles >, Save original
 * file >, Mark watched, Mark unwatched, Back, Refresh.
 *
 * Details window: the video's backdrop, faded into the window, its title,
 * year, running time, rating and summary, how it will play, buttons for
 * Play, Resume, From the start, Save and Mark watched, and its subtitles
 * (chosen on the server, which burns them into a converted stream).
 *
 * Playing: the yt-dlp style JSON that Reel reads (handoff.c) is written to
 * <Wimp$ScrapDir>.Matinee.PlayN, typed as video/mp4, and given to the
 * chosen player with Message_DataOpen, sent to that task alone. If it isn't
 * running (or doesn't claim it), the player is started with the file:
 * *Run <ReelEGL$Dir>.!Run <file>.
 *
 * Saving: the Plex file itself, 256KB each null event, up to 4GB-1 (the
 * most RISC OS's filing systems hold); bigger parts are refused at once.
 *
 * Posters: one fetched each null event, for the tiles in view, at the
 * tile's size in pixels, drawn into a 32bpp sprite by SpriteExtend
 * (JPEG_PlotScaled with output switched to the sprite) and plotted with
 * Wimp_PlotIcon.
 *
 * Choices: Choices:Matinee.Choices, written to <Choices$Write>.Matinee.
 * Matinee$ChoicesDir (a directory) replaces both, for the host test.
 *
 * The front end's files (ui_int.h is what they share):
 *   ui.c        the windows, the main loop and its events; the host test's hooks
 *   choices.c   Choices
 *   pictures.c  posters, photos and backdrops: the cache, fetching without waiting
 *   browse.c    the browser: layout, drawing, tabs, the show page, a library's
 *               bar, the lists, search
 *   home.c      the home page
 *   details.c   the details page
 *   signin.c    the sign-in page, and the servers
 *   playback.c  handing a video to Reel; the built-in player's side
 *   saving.c    saving the original file; the speed test
 *   menus.c     the menus; Plex Home
 *
 * Plain Wimp SWIs, as in Reel: no Toolbox, no Templates.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"
#include "proginfo.h"

#ifdef __riscos__
/* UnixLib: the heap in a dynamic area of its own (the posters live there) */
const char *const __dynamic_da_name = "Matinee Heap";
/* The posters, and the player's pictures and packets. 128MB: an area
   takes logical address space for its whole maximum when it's made, and
   on a machine whose DALimit allows more, 512MB used to be taken however
   little was in it. UnixLib adds "Matinee Heap 2" (and more) if the heap
   needs it. */
int __dynamic_da_max_size = 128 << 20;
#endif

const ui_size sizes[3] = {
    { 168, 252, "Small" }, { 232, 348, "Medium" }, { 320, 480, "Large" }
};

ui_state S;

/* ---- small helpers ------------------------------------------------------ */

/* ---- small helpers ------------------------------------------------------ */

int now_cs(void)
{
    _kernel_swi_regs r;
    return swi(OS_ReadMonotonicTime, &r) ? (int)(clock() * 100 / CLOCKS_PER_SEC) : r.r[0];
}

void hourglass(int on)
{
    _kernel_swi_regs r;
    swi(on ? Hourglass_On : Hourglass_Off, &r);
}

void report(const char *fmt, ...)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    va_list a;
    va_start(a, fmt);
    e.errnum = 0;
    vsnprintf(e.errmess, sizeof(e.errmess), fmt, a);
    va_end(a);
    r.r[0] = (intptr_t)&e;
    r.r[1] = 1 | 16;                /* OK, no "Error from" */
    r.r[2] = (intptr_t)APP;
    swi(Wimp_ReportError, &r);
}

/* 1 if the user clicks OK */
int ask(const char *text)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s", text);
    r.r[0] = (intptr_t)&e;
    r.r[1] = 1 | 2 | 16;            /* OK and Cancel */
    r.r[2] = (intptr_t)APP;
    return !swi(Wimp_ReportError, &r) && r.r[1] == 1;
}

/* UTF-8 (as Plex sends it) to the desktop's Latin-1 */
void latin1(const char *s, char *out, size_t size)
{
    size_t o = 0;
    if (!size)
        return;
    while (s && *s && o + 4 < size) {
        unsigned char c = (unsigned char)*s;
        unsigned u;
        int n;
        if (c < 0x80) { u = c; n = 1; }
        else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { u = ((c & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
        else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            u = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3;
        } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            u = 0x10000; n = 4;
        } else { u = '?'; n = 1; }
        s += n;
        switch (u) {
        case 0x2018: case 0x2019: case 0x2032: out[o++] = '\''; break;
        case 0x201C: case 0x201D: out[o++] = '"'; break;
        case 0x2013: case 0x2014: case 0x2212: out[o++] = '-'; break;
        case 0x2026: out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; break;
        default: out[o++] = (char)(u < 0x20 ? ' ' : u <= 0xFF ? u : '?');
        }
    }
    out[o] = 0;
}

/* "h:mm:ss" (or "m:ss") */
void hms(int64_t ms, char *out, size_t size)
{
    long s = (long)(ms / 1000);
    if (s >= 3600)
        snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

void make_dir(const char *path)
{
    _kernel_swi_regs r;
    r.r[0] = 8;                     /* OS_File 8: create a directory */
    r.r[1] = (intptr_t)path;
    r.r[4] = 0;
    swi(OS_File, &r);
}

void set_type(const char *path, int type)
{
    _kernel_swi_regs r;
    r.r[0] = 18;                    /* OS_File 18: set the filetype */
    r.r[1] = (intptr_t)path;
    r.r[2] = type;
    swi(OS_File, &r);
}

/* A filetype from a MIME type ("video/mp4") or an extension ("mkv") */
int mime_type(int from, const char *what, int fallback)
{
    _kernel_swi_regs r;
    r.r[0] = from;                  /* 2: MIME type, 3: extension */
    r.r[1] = (intptr_t)what;
    r.r[2] = 0;                     /* to a filetype */
    if (swi(MimeMap_Translate, &r) || r.r[3] < 0 || r.r[3] > 0xFFF)
        return fallback;
    return r.r[3];
}

/* ---- windows: making them ------------------------------------------------- */

typedef struct {
    int vis[4];
    int sx, sy, behind, flags;
    unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
    int ext[4];
    int tflags, wbutton, sprites;
    short minw, minh;
    int title[3];
    int nicons;
} window_t;

static int create_window(window_t *w, const icon_t *icons, int n)
{
    static int block[(sizeof(window_t) + 16 * sizeof(icon_t)) / 4];
    _kernel_swi_regs r;
    w->nicons = n;
    memcpy(block, w, sizeof(*w));
    if (n)
        memcpy((char *)block + sizeof(*w), icons, n * sizeof(icon_t));
    r.r[1] = (intptr_t)block;
    return swi(Wimp_CreateWindow, &r) ? -1 : r.r[0];
}

static void window_defaults(window_t *w, int width, int height, char *title, int tlen, unsigned flags)
{
    memset(w, 0, sizeof(*w));
    w->vis[0] = 300; w->vis[1] = 300; w->vis[2] = 300 + width; w->vis[3] = 300 + height;
    w->behind = -1;
    w->flags = (int)flags;
    w->tfg = 7; w->tbg = 2; w->wfg = 7; w->wbg = 1; w->sofg = 3; w->sibg = 1; w->tfocus = 12;
    w->ext[1] = -height; w->ext[2] = width;
    w->tflags = 0x0000013D;         /* text, border, centred, filled, indirected */
    w->sprites = 1;
    w->title[0] = (int)(intptr_t)title;
    w->title[1] = -1;
    w->title[2] = tlen;
}

/* An icon indirected to text (validation may be NULL) */
static void icon_text(icon_t *i, int x0, int y0, int x1, int y1, unsigned flags, char *text, int size,
                      const char *validation)
{
    i->box[0] = x0; i->box[1] = y0; i->box[2] = x1; i->box[3] = y1;
    i->flags = flags | 0x101;       /* text, indirected */
    i->data[0] = (int)(intptr_t)text;
    i->data[1] = validation ? (int)(intptr_t)validation : -1;
    i->data[2] = size;
}

#define F_BUTTON  0x1700303Du       /* click; border, centred, filled; black on grey */
#define F_WRITE   0x0700F035u       /* writable; border, v centred, filled; black on white */

static char valid_write[] = "Ktar";
static char sv_title_text[] = "Save as";
char sv_sprite[16];
static char sv_ok_text[] = "Save";

static void make_windows(void)
{
    window_t w;
    icon_t ic[SV_COUNT];

    /* browser: all drawn by us (draw.c), so the Wimp doesn't fill it
       (work area colour 255); its extent follows the list */
    window_defaults(&w, 4 * (TILE_W + GAP) + GAP, 1000, S.title, sizeof(S.title), 0xBF000002u);
    w.wbg = 255;
    w.ext[1] = -1000;
    w.ext[2] = S.scr_w;
    w.wbutton = 10 << 12;           /* click, drag, double-click */
    w.minw = 2 * (168 + GAP) + GAP;
    w.minh = HEADER_H + 200;
    S.browser_w = create_window(&w, NULL, 0);

    /* the save box: a menu leaf of Save original file */
    window_defaults(&w, 400, 264, sv_title_text, sizeof(sv_title_text), 0x84000012u);
    memset(ic, 0, sizeof(ic));
    snprintf(sv_sprite, sizeof(sv_sprite), "file_ffd");
    ic[SV_FILE].box[0] = 136; ic[SV_FILE].box[1] = -124; ic[SV_FILE].box[2] = 264; ic[SV_FILE].box[3] = -20;
    ic[SV_FILE].flags = 0x1700611Au;    /* sprite, centred, indirected; click/drag */
    ic[SV_FILE].data[0] = (int)(intptr_t)sv_sprite;
    ic[SV_FILE].data[1] = 1;            /* the Wimp's sprites */
    ic[SV_FILE].data[2] = (int)strlen(sv_sprite);
    icon_text(&ic[SV_NAME], 12, -192, 388, -140, F_WRITE, S.sv_name, sizeof(S.sv_name), valid_write);
    icon_text(&ic[SV_OK], 248, -252, 388, -200, F_BUTTON, sv_ok_text, sizeof(sv_ok_text), NULL);
    S.save_w = create_window(&w, ic, SV_COUNT);
}

void window_state(int w, int *st)
{
    _kernel_swi_regs r;
    memset(st, 0, 9 * sizeof(int));     /* all callers' blocks are 9 words; zero if the Wimp refuses */
    st[0] = w;
    r.r[1] = (intptr_t)st;
    if (swi(Wimp_GetWindowState, &r)) {
        memset(st, 0, 9 * sizeof(int));
        st[0] = w;
    }
}

void open_front(int w, int x0, int y0, int x1, int y1, int sx, int sy)
{
    int b[8];
    _kernel_swi_regs r;
    b[0] = w; b[1] = x0; b[2] = y0; b[3] = x1; b[4] = y1; b[5] = sx; b[6] = sy; b[7] = -1;
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
}

void close_window(int w)
{
    _kernel_swi_regs r;
    int b[1];
    b[0] = w;
    r.r[1] = (intptr_t)b;
    swi(Wimp_CloseWindow, &r);
}

void set_caret(int w, int icon, const char *text)
{
    _kernel_swi_regs r;
    r.r[0] = w;
    r.r[1] = icon;
    r.r[2] = 0; r.r[3] = 0;
    r.r[4] = icon < 0 ? (1 << 25) : -1;         /* in the work area: an invisible caret */
    r.r[5] = text ? (int)strlen(text) : 0;
    swi(Wimp_SetCaretPosition, &r);
}

void si_set_status(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(S.si_status, sizeof(S.si_status), fmt, a);
    va_end(a);
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, -HEADER_H);
}

void set_status(const char *fmt, ...)
{
    char t[400];
    va_list a;
    va_start(a, fmt);
    vsnprintf(t, sizeof(t), fmt, a);
    va_end(a);
    latin1(t, S.status, sizeof(S.status));
    if (S.browser_open)
        force_redraw(S.browser_w, 0, -HEADER_H, S.scr_w, 0);
}

/* ---- the screen ------------------------------------------------------------- */

static int mode_var(int v, int def)
{
    _kernel_swi_regs r;
    r.r[0] = -1;
    r.r[1] = v;
    return swi(OS_ReadModeVariable, &r) ? def : r.r[2];
}

static void read_screen(void)
{
    S.xeig = mode_var(4, 1);
    S.yeig = mode_var(5, 1);
    S.scr_w = (mode_var(11, 1919) + 1) << S.xeig;
    S.scr_h = (mode_var(12, 1079) + 1) << S.yeig;
}

/* ---- events --------------------------------------------------------------------- */

/* More like this or the collection: that film's details, in place; an extra:
   play it, from the start */
static void det_open_rel(int k, int i)
{
    if (k < 0 || k >= REL_ROWS || i < 0 || i >= S.rel[k].n)
        return;
    if (k == REL_EXTRAS) {
        play_item(&S.rel[k].v[i], PLAY_START);
        return;
    }
    /* (det_fetch uses the item only to ask the server, before it lets
       S.rel go) */
    if (det_fetch(&S.rel[k].v[i], 1) != 0)
        return;
    S.det_i = -1;                   /* not one of the list's */
    det_repaint();
    set_where();
}

/* A button in the details window */
static void det_action(int id, int x, int y)
{
    const plex_item *it = det_item(), *shown = it;
    _kernel_swi_regs r;
    if (!it)
        return;
    if (S.det_i >= 0 && S.det_i < S.list.n && det_is(&S.list.v[S.det_i]))
        shown = &S.list.v[S.det_i];         /* its title as the list shows it */
    switch (id) {
    case D_PLAY:
        play_item(shown, PLAY_DEFAULT);
        break;
    case D_RESUME:
        play_item(shown, PLAY_RESUME);
        break;
    case D_START:
        play_item(shown, PLAY_START);
        break;
    case D_SAVE:
        if (S.save.active) {
            report("Already saving %s: stop that first (on a poster's menu).", S.save.title);
            break;
        }
        save_prepare(it);
        if (S.sv_ok && !too_big(S.sv_size, S.sv_title)) {
            r.r[1] = S.save_w;              /* the save box, as a menu */
            r.r[2] = x - 64;
            r.r[3] = y + 64;
            swi(Wimp_CreateMenu, &r);
            S.menu_kind = 0;
        }
        break;
    case D_WATCHED:
        mark(it, !it->watched);
        break;
    case D_STAR: case D_STAR + 1: case D_STAR + 2: case D_STAR + 3: case D_STAR + 4: {
        int r = 2 * (id - D_STAR + 1);
        rate(it, it->user_rating > r - 1 && it->user_rating < r + 1 ? -1 : r);   /* the same star: none */
        break;
    }
    case D_SUBS:
        subs_menu_build();
        S.menu_kind = 3;
        S.menu_x = x;
        S.menu_y = y;
        open_menu(&m_subs, x - 64, y);
        break;
    }
}

static void click(int *b)
{
    int w = b[3], i = b[4], buttons = b[2];
    if (w == -2) {
        if (buttons & 2)
            bar_menu_open(b[0]);
        else if (buttons & 5)
            bar_select();
        return;
    }
    if (w == S.save_w) {
        if (i == SV_FILE && (buttons & 0x50))
            drag_start();
        else if (i == SV_OK && (buttons & 5))
            save_ok();
        return;
    }
    if (player_owns(w) || (w == S.browser_w && S.page == PG_PLAYER)) {
        builtin_event(player_click(b));
        return;
    }
    if (w == S.browser_w) {
        int t, h = S.page != PG_SIGNIN ? header_hit(b[0], b[1]) : 0;
        set_caret(S.browser_w, -1, NULL);
        if (h && (buttons & 0x505)) {           /* Back, Refresh, Search */
            if (h == B_BACK)
                go_back();
            else if (h == B_SEARCH)
                search_open();
            else if (S.page == PG_DETAILS)
                det_refresh();
            else
                refresh_list();
            return;
        }
        if (S.page != PG_SIGNIN && (buttons & 0x505)) {
            int k = tab_hit(b[0], b[1]);
            if (k >= 0) {
                if (S.page == PG_DETAILS)
                    det_leave();
                tab_open(k);
                return;
            }
        }
        if (S.page == PG_SIGNIN) {
            int id = si_hit(b[0], b[1]);
            if (!(buttons & 0x505))
                return;
            if (id == S_NEWCODE)
                pin_new();
            else if (id == S_USE)
                use_manual();
            else if (id == S_PICK_JF)
                jf_add();
            else if (id == S_PICK_DLNA)
                dl_add();
            else if (id == S_DSEARCH)
                dl_search();
            else if (id == S_DUSE)
                dl_use(S.dl_addr);
            else if (id >= S_FOUND && id < S_FOUND + SI_FOUND_SHOWN && id - S_FOUND < S.nfound)
                dl_use(S.found[id - S_FOUND].location);
            else if (id == S_PICK_PLEX)
                signin_show(SI_PLEX);
            else if (id == S_BACK)
                signin_show(SI_PICK);
            else if (id == S_CANCEL)
                signin_cancel();
            else if (id == S_QC)
                qc_new();
            else if (id == S_LOGIN)
                jf_password();
            else if (si_field_of(id) >= 0) {
                S.field = si_field_of(id);
                si_redraw_fields();
            }
            return;
        }
        if (S.page == PG_DETAILS) {
            int id = det_button_at(b[0], b[1]), rel = id ? 0 : det_rel_at(b[0], b[1]);
            if (rel && (buttons & 0x505)) {         /* More like this: its details; an extra: play it */
                det_open_rel(rel / 1000 - 1, rel % 1000);
                return;
            }
            if ((buttons & 2) || ((buttons & 0x100) && !id))
                item_menu_open(b[0], b[1]);
            else if (id && (buttons & 0x505))
                det_action(id, b[0], b[1]);
            return;
        }
        if (S.lib.on && S.page == PG_GRID && (buttons & 0x505)) {
            int h2 = lib_hit(b[0], b[1]);
            if (h2) {
                lib_action(h2, b[0], b[1]);
                return;
            }
        }
        if (S.home.on && (buttons & 0x505)) {
            int h2 = home_hit(b[0], b[1]);
            if (h2) {
                home_action(h2);
                return;
            }
        }
        if (S.show.on && (buttons & 0x505)) {
            int h2 = show_hit(b[0], b[1]);
            if (h2 >= SH_TAB) {
                show_season(h2 - SH_TAB);
                return;
            }
            if (h2) {
                show_action(h2);
                return;
            }
        }
        t = tile_at(b[0], b[1]);
        if ((buttons & 2) || ((buttons & 0x100) && t >= 0)) {  /* Menu, or Adjust (a PC mouse's right button) */
            if (t >= 0)
                select_tile(t);
            item_menu_open(b[0], b[1]);
        } else if (buttons & 0x500) {           /* one click */
            select_tile(t);
        } else if (buttons & 5) {               /* double-click: a folder opens, a video's details */
            select_tile(t);
            if (t >= 0 && S.list.v[t].kind == PI_VIDEO)
                det_show(t);
            else if (t >= 0)
                open_item(t, PLAY_DEFAULT);
        }
    }
}

static void key(int *b)
{
    int w = b[0], k = b[6];
    _kernel_swi_regs r;
    if (w == S.save_w && k == 13) {
        save_ok();
        return;
    }
    if ((player_owns(w) || (w == S.browser_w && S.page == PG_PLAYER)) && S.pl.on && S.pl.det.n &&
        (k == 0x19E || k == 0x19F) && player_ready()) {
        builtin_chapter_step(k == 0x19E ? 1 : -1);      /* Page Down: the next chapter */
        return;
    }
    if (player_owns(w) || (w == S.browser_w && S.page == PG_PLAYER)) {
        int e = player_key(k);
        if (e >= 0) {
            builtin_event(e);
            return;
        }
    }
    if (w == S.browser_w && S.page == PG_SIGNIN) {
        int u = signin_key(k);
        if (u == 2)
            use_manual();
        else if (u == 3)
            jf_password();
        else if (u == 4)
            signin_cancel();
        else if (u == 5)
            dl_use(S.dl_addr);
        if (u)
            return;
    }
    if (w == S.browser_w && S.page == PG_DETAILS) {
        int n = S.have_list ? S.list.n : 0, s = S.det_i;
        switch (k) {
        case 13:
            det_action(D_PLAY, 0, 0);
            return;
        case 8: case 0x1B: case 0x7F:
            det_leave();
            return;
        case 0x18C: case 0x18D:     /* the video before or after it in the list */
            if (s >= 0)
                for (int j = s + (k == 0x18C ? -1 : 1); j >= 0 && j < n; j += k == 0x18C ? -1 : 1)
                    if (S.list.v[j].kind == PI_VIDEO) {
                        det_show(j);
                        break;
                    }
            return;
        }
    }
    if (w == S.browser_w && searching() && search_key(k))
        return;
    if (w == S.browser_w && S.page == PG_GRID) {
        int n = S.have_list ? S.list.n : 0, s = S.sel;
        switch (k) {
        case '/': case 6:           /* / or Ctrl-F: search */
            if (!searching()) {
                search_open();
                return;
            }
            break;
        case 8: case 0x1B: case 0x7F:
            go_back();
            return;
        case 13:
            if (s >= 0)
                open_item(s, PLAY_DEFAULT);
            return;
        case 'i': case 'I':
            if (s >= 0)
                det_show(s);
            return;
        case 0x18C: case 0x18D: case 0x18E: case 0x18F:
            if (S.home.on) {
                home_key(k);
                return;
            }
            break;
        }
        switch (k) {
        case 0x18C: if (s > 0) select_tile(s - 1); return;                 /* left */
        case 0x18D: if (s + 1 < n) select_tile(s + 1); return;             /* right */
        case 0x18E: if (s + S.cols < n) select_tile(s + S.cols); return;   /* down */
        case 0x18F: if (s - S.cols >= 0) select_tile(s - S.cols); return;  /* up */
        }
    }
    r.r[0] = k;
    swi(Wimp_ProcessKey, &r);
}

static void open_request(int *b)
{
    _kernel_swi_regs r;
    if (player_owns(b[0]) && player_mini()) {
        player_mini_open_request(b);        /* saved when it goes back, or at the end */
        return;
    }
    if (b[0] == S.browser_w && S.page == PG_PLAYER) {
        /* the player page doesn't scroll: the picture and the bar fill what's
           in view (the scroll bars are the other pages') */
        b[5] = 0;
        b[6] = 0;
    }
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
    if (b[0] == S.browser_w && S.page == PG_PLAYER) {
        player_layout();
        return;
    }
    if (b[0] == S.browser_w) {
        int w = b[3] - b[1], cols = layout_cols(w);
        S.posters_wanted = 1;       /* scrolled or resized: more may be in view */
        if (S.home.on) {            /* the home page: laid out again for a new size */
            int oh = S.home.hero_h;
            if (w != S.home.w || (home_layout(), S.home.hero_h != oh)) {
                home_layout();
                make_disp();
                if (S.page == PG_GRID) {
                    set_extent();
                    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
                }
            }
        } else if (S.show.on) {     /* the show page: laid out again for a new width */
            if (w != S.show.w) {
                show_layout();
                make_disp();
                if (S.page == PG_GRID) {
                    set_extent();
                    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
                }
            }
        } else if (cols != S.cols || (S.lib.on && w != S.width)) {
            S.cols = cols;
            S.width = w;
            if (S.lib.on)
                lib_layout();
            if (S.page == PG_GRID) {
                set_extent();
                force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
            }
        }
        if (S.page == PG_DETAILS) {
            int ow = art_w, oh = art_h;
            det_art_box();
            if (w != det_wd || art_w != ow || art_h != oh)
                det_repaint();      /* the text wrapped, the backdrop scaled to the new size */
        }
    }
}

static void close_request(int *b)
{
    if (b[0] == S.browser_w) {
        if (S.pl.on)
            builtin_stop(1);
        close_window(S.browser_w);
        S.browser_open = 0;
        S.in_browser = 0;
        if (S.page == PG_SIGNIN) {
            signin_close();
            S.page = PG_GRID;       /* opened again: from the start (bar_select) */
        }
    }
}

static int pending_find(int ref)
{
    for (int i = 0; i < PEND_MAX; i++)
        if (S.pend[i].ref && S.pend[i].ref == ref)
            return i;
    return -1;
}

/* 1 if it's time to stop */
static int message(int event, int *b)
{
    _kernel_swi_regs r;
    int i;
    switch (b[4]) {
    case MSG_QUIT:
        if (event != 19) {
            quit();
            return 1;
        }
        break;
    case MSG_PREQUIT:
        if (event != 19 && S.save.active) {
            int sender = b[1], flags = b[0] > 20 ? b[5] : 0;
            b[3] = b[2];
            r.r[0] = 19;            /* acknowledged: the desktop doesn't close yet */
            r.r[1] = (intptr_t)b;
            r.r[2] = sender;
            swi(Wimp_SendMessage, &r);
            if (ask("A file is being saved. Stop saving it and quit?")) {
                save_stop(NULL);
                if (!(flags & 1)) {  /* carry on shutting down (Ctrl-Shift-F12) */
                    int c[8];
                    r.r[1] = (intptr_t)c;
                    swi(Wimp_GetCaretPosition, &r);
                    c[6] = 0x1FC;
                    r.r[0] = 8;
                    r.r[1] = (intptr_t)c;
                    r.r[2] = sender;
                    swi(Wimp_SendMessage, &r);
                }
                quit();
                return 1;
            }
        }
        break;
    case MSG_DATAOPEN:              /* ours, back unclaimed: start the player */
        if (event == 19 && (i = pending_find(b[2])) >= 0) {
            pend_t p = S.pend[i];
            S.pend[i].ref = 0;
            start_player(p.player, p.path);
        }
        break;
    case MSG_DATALOADACK:           /* the player has read it */
        if ((i = pending_find(b[3])) >= 0) {
            remove(S.pend[i].path);
            set_status("%s (in %s)", S.play_why, player_names[S.pend[i].player]);
            S.pend[i].ref = 0;
        }
        break;
    case MSG_DATASAVEACK:           /* where to save the file */
        if (event != 19 && S.datasave_ref && b[3] == S.datasave_ref) {
            char path[256];
            S.datasave_ref = 0;
            snprintf(path, sizeof(path), "%s", (const char *)&b[11]);
            if (b[9] == -1) {
                report("Save it into a directory display: it's too big to give to another program.");
                break;
            }
            close_menus();
            save_start(path);
        }
        break;
    case MSG_MODECHANGE:
        read_screen();
        draw_init(S.xeig, S.yeig);
        if (S.pl.on)
            player_mode_change();
        cache_free_all();
        if (S.have_list)
            make_disp();
        if (S.page == PG_DETAILS)
            det_refresh();
        if (S.browser_open) {
            set_extent();
            force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
        }
        S.posters_wanted = 1;
        break;
    }
    return 0;
}

/* A library's next page, while the grid shows it (the server sends a
   library PAGE_SIZE at a time): added to the list, its tiles drawn, A to Z
   made again. 1 if one was fetched (or tried). */
static int list_more_step(void)
{
    int old = S.list.n, e, x0, y0, x1, y1;
    disp_t *d;
    if (!S.have_list || S.home.on || S.show.on) {
        S.more_wanted = 0;
        return 0;
    }
    if (S.page != PG_GRID || !S.browser_open)
        return 0;                   /* on with it when the grid's back */
    hourglass(1);
    e = plex_list_more(&S.px, S.path, &S.list);
    hourglass(0);
    if (e < 0) {
        S.more_wanted = 0;
        set_status("%d of %d: the rest couldn't be fetched (%s). Refresh to try again.", S.list.n, S.list.total,
                   S.px.err);
        return 1;
    }
    if (e == 0) {
        S.more_wanted = 0;
        set_status("%d item%s.", S.list.n, S.list.n == 1 ? "" : "s");
        return 0;
    }
    d = realloc(S.disp, (size_t)S.list.n * sizeof(disp_t));
    if (!d) {                       /* no room for their lines: the new page let go */
        for (int i = old; i < S.list.n; i++)
            plex_item_free(&S.list.v[i]);
        S.list.n = S.list.total = old;
        S.more_wanted = 0;
        set_status("The first %d: there isn't the memory for more.", old);
        return 1;
    }
    S.disp = d;
    memset(d + old, 0, (size_t)(S.list.n - old) * sizeof(disp_t));
    for (int i = old; i < S.list.n; i++) {      /* as make_disp's grid */
        const plex_item *it = &S.list.v[i];
        latin1(it->title, d[i].line[0], sizeof(d[i].line[0]));
        latin1(it->subtitle ? it->subtitle : "", d[i].line[1], sizeof(d[i].line[1]));
        d[i].poster = it->thumb ? cache_find(it->thumb) : NULL;
    }
    S.more_wanted = S.list.n < S.list.total;
    if (S.lib.on) {                 /* A to Z: letters further on */
        lib_layout();
        force_redraw(S.browser_w, 0, -HEADER_H - libbar_h(), S.scr_w, -HEADER_H);
    }
    set_extent();
    tile_box(old, &x0, &y0, &x1, &y1);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, y1 + GAP);
    if (S.sel_want >= 0 && S.sel_want < S.list.n) {     /* where Back came to */
        int was = S.sel;
        S.sel = S.sel_want;
        S.sel_want = -1;
        redraw_tile(was);
        redraw_tile(S.sel);
    }
    if (S.more_wanted)
        set_status("%d of %d: the rest coming.", S.list.n, S.list.total);
    else
        set_status("%d item%s.", S.list.n, S.list.n == 1 ? "" : "s");
    S.posters_wanted = 1;
    return 1;
}

/* What null events are for: saving, posters, and the sign-in checks */
static void nulls(void)
{
    if (S.save.active) {
        save_step();
        return;
    }
    if (S.speed.active) {
        speed_step();
        return;
    }
    if (S.pl.on) {
        builtin_nulls();
        return;
    }
    if (S.search_due && now_cs() - S.search_due >= 0) {
        if (searching())
            search_now();
        S.search_due = 0;
        return;
    }
    if (S.pin_id && S.browser_open && S.page == PG_SIGNIN && now_cs() - S.pin_next >= 0) {
        pin_check();
        return;
    }
    if (S.jf_secret[0] && S.si_mode == SI_JF && S.browser_open && S.page == PG_SIGNIN && now_cs() - S.qc_next >= 0) {
        qc_check();
        return;
    }
    if (S.home.due && S.home.on && S.page == PG_GRID && S.browser_open && now_cs() - S.home.due >= 0) {
        home_fetch_art();           /* the featured backdrop at the window's new size */
        force_redraw(S.browser_w, 0, -HEADER_H - S.home.hero_h, S.scr_w, -HEADER_H);
        return;
    }
    if (S.show.due && S.show.on && S.page == PG_GRID && S.browser_open && now_cs() - S.show.due >= 0) {
        show_fetch_art();           /* the backdrop and the poster at the window's new size */
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
        return;
    }
    if (art_due && S.page == PG_DETAILS && S.browser_open && now_cs() - art_due >= 0) {
        det_fetch_art();            /* the backdrop at the window's new size */
        det_repaint();
        return;
    }
    /* (after those: with pictures fetched without waiting, these go on for
       a while, and a resize's backdrop shouldn't wait for them) */
    if (S.cast_wanted && S.page == PG_DETAILS && det_cast_step())
        return;
    if (S.posters_wanted && poster_step())
        return;
    if (S.more_wanted && list_more_step())
        return;
    if (S.in_browser && S.browser_open) {   /* the poster under the pointer */
        int p[5];
        _kernel_swi_regs r;
        r.r[1] = (intptr_t)p;
        if (!swi(Wimp_GetPointerInfo, &r))
            set_hover(p[3] == S.browser_w && S.page == PG_GRID ? tile_at(p[0], p[1]) : -1);
    }
}

static void iconbar_icon(void)
{
    struct { int w, x0, y0, x1, y1, flags; char name[12]; } b;
    _kernel_swi_regs r;
    memset(&b, 0, sizeof(b));
    b.w = -1;
    b.x1 = 68; b.y1 = 68;
    b.flags = 0x301A;               /* sprite, centred, click */
    strcpy(b.name, ICON);
    r.r[0] = 0;
    r.r[1] = (intptr_t)&b;
    swi(Wimp_CreateIcon, &r);
    S.bar_icon = r.r[0];
}

static int already_running(void)
{
    return find_task(APP) != 0;
}

int matinee_main(int argc, char **argv)
{
    static const int messages[] = { MSG_DATASAVE, MSG_DATASAVEACK, MSG_DATALOAD, MSG_DATALOADACK,
                                    MSG_DATAOPEN, MSG_PREQUIT, MSG_MODECHANGE, 0 };
    int block[64];
    _kernel_swi_regs r;
    (void)argc;
    (void)argv;

    memset(&S, 0, sizeof(S));
    S.sel = -1;
    S.hover = -1;
    S.det_i = -1;
    S.cols = 4;
    r.r[0] = 380;
    r.r[1] = 0x4B534154;            /* "TASK" */
    r.r[2] = (intptr_t)APP;
    r.r[3] = (intptr_t)messages;
    if (swi(Wimp_Initialise, &r))
        return 1;
    S.task = r.r[1];
    if (already_running()) {
        r.r[0] = S.task;
        swi(Wimp_CloseDown, &r);
        return 0;
    }
    snprintf(S.agent, sizeof(S.agent), APP "/" MATINEE_VERSION " (RISC OS)");
    net_init(S.agent);
    choices_load();
    choices_save();                 /* the client id is kept from the start */
    read_screen();
    draw_init(S.xeig, S.yeig);
    snprintf(S.code, sizeof(S.code), "-");
    snprintf(S.title, sizeof(S.title), APP);
    make_windows();
    iconbar_icon();
    S.proginfo = proginfo_create(APP, PURPOSE, APP_AUTHOR, MATINEE_VERSION " (" MATINEE_DATE ")");
    player_init(S.task, S.overlay, S.pic_mode, S.volume);
    {
        /* the image cache: in Choices, or Matinee$Cache */
        const char *c = getenv(APP "$Cache"), *d = getenv(APP "$ChoicesDir");
        char dir[300];
        if (c && *c)
            snprintf(dir, sizeof(dir), "%s", c);
        else if (d && *d)
            snprintf(dir, sizeof(dir), "%s" SEP "Cache", d);
        else {
            make_dir("<Choices$Write>." APP);
            snprintf(dir, sizeof(dir), "<Choices$Write>." APP ".Cache");
        }
        imgcache_init(dir, (long long)S.cache_mb << 20);
    }

    for (;;) {
        int mask = 1 << 11 | 1 << 12;  /* no caret events */
        int reason = Wimp_Poll;
        if (S.save.active || S.speed.active) {
            /* null events at once */
        } else if (S.pl.on) {           /* the built-in player says how often */
            int cs = player_poll_cs();
            if (S.pl.upnext_cs && (cs < 0 || cs > 25))
                cs = 25;                /* the card's countdown */
            if (cs < 0 && player_paused() && !S.pl.p.direct)
                cs = S.pl.ping_cs - now_cs() > 0 ? S.pl.ping_cs - now_cs() : 1;   /* the keep-alive */
            if (cs > 0) {
                reason = Wimp_PollIdle;
                r.r[2] = now_cs() + cs;
            } else if (cs < 0)
                mask |= 1;
        } else if (S.search_due && searching()) {
            reason = Wimp_PollIdle;         /* the search, a moment after the last key */
            r.r[2] = S.search_due;
        } else if (S.home.due && S.home.on && S.page == PG_GRID && S.browser_open) {
            reason = Wimp_PollIdle;
            r.r[2] = S.home.due;
        } else if (S.show.due && S.show.on && S.page == PG_GRID && S.browser_open) {
            reason = Wimp_PollIdle;         /* the show's pictures at the new size, once resizing stops */
            r.r[2] = S.show.due;
        } else if (art_due && S.page == PG_DETAILS && S.browser_open) {
            reason = Wimp_PollIdle;         /* the backdrop at the new size, once resizing stops */
            r.r[2] = art_due;
        } else if (S.posters_wanted || (S.cast_wanted && S.page == PG_DETAILS && S.browser_open) ||
                   (S.more_wanted && S.page == PG_GRID && S.browser_open)) {
            if (pf_waiting()) {     /* a picture's answer: the rest of the desktop runs meanwhile */
                reason = Wimp_PollIdle;
                r.r[2] = now_cs() + 1;
            }                       /* (else null events at once) */
        } else if (S.pin_id && S.browser_open && S.page == PG_SIGNIN) {
            reason = Wimp_PollIdle;
            r.r[2] = S.pin_next;
        } else if (S.jf_secret[0] && S.si_mode == SI_JF && S.browser_open && S.page == PG_SIGNIN) {
            reason = Wimp_PollIdle;
            r.r[2] = S.qc_next;
        } else if (S.in_browser && S.browser_open) {
            reason = Wimp_PollIdle;         /* watching the pointer, 10 times a second */
            r.r[2] = now_cs() + 10;
        } else {
            mask |= 1;
        }
        r.r[0] = mask;
        r.r[1] = (intptr_t)block;
        if (swi(reason, &r))
            continue;
        switch (r.r[0]) {
        case 0:
            nulls();
            break;
        case 1:
            redraw(block);
            break;
        case 2:
            open_request(block);
            break;
        case 3:
            close_request(block);
            break;
        case 4:                     /* the pointer left a window */
            if (block[0] == S.browser_w) {
                S.in_browser = 0;
                set_hover(-1);
            }
            break;
        case 5:                     /* ...or came into one */
            if (block[0] == S.browser_w)
                S.in_browser = 1;
            break;
        case 6:
            click(block);
            break;
        case 7:
            drag_end();
            break;
        case 8:
            key(block);
            break;
        case 9:
            if (menu_select(block))
                return 0;
            break;
        case 17: case 18: case 19:
            if (message(r.r[0], block))
                return 0;
            break;
        }
    }
}

#ifdef MATINEE_TEST
int ui_test_tile_xy(int i, int *x, int *y)
{
    int st[9], x0, y0, x1, y1;
    if (!S.have_list || i < 0 || i >= S.list.n)
        return -1;
    window_state(S.browser_w, st);
    tile_box(i, &x0, &y0, &x1, &y1);
    *x = (x0 + x1) / 2 + st[1] - st[5];
    *y = (y0 + y1) / 2 + st[4] - st[6];
    return 0;
}
int ui_test_items(void) { return S.have_list ? S.list.n : -1; }
const char *ui_test_item(int i, int line)
{
    if (!S.disp || i < 0 || i >= S.list.n)
        return "";
    fit_disp(i);
    return S.disp[i].line[line & 1];
}
const char *ui_test_status(void) { return S.status; }
const char *ui_test_path(void) { return S.where; }
int ui_test_saving(void) { return S.save.active; }
int ui_test_speed(void) { return S.speed.active; }
int ui_test_sel(void) { return S.sel; }
int ui_test_posters(int *failed)
{
    int n = 0, f = 0;
    for (poster_t *p = S.cache; p; p = p->next)
        p->failed ? f++ : n++;
    if (failed)
        *failed = f;
    return n;
}
int ui_test_windows(int *browser, int *save)
{
    *browser = S.browser_w;
    *save = S.save_w;
    return S.browser_open;
}
int ui_test_page(void) { return S.page; }
const char *ui_test_signin(int what)
{
    return what == 0 ? S.code : what == 1 ? S.si_status : what == 2 ? S.si_addr : what == 3 ? S.si_tok :
           what == 4 ? S.jf_code : what == 5 ? S.jf_addr : what == 6 ? S.jf_user : S.px.server_name;
}
const char *ui_test_rel(int k, int *n)
{
    *n = k >= 0 && k < REL_ROWS ? S.nrel[k] : 0;
    return k >= 0 && k < REL_ROWS ? S.rel_head[k] : "";
}
/* A details row's picture i: the screen y of its foot (the return) and of its top */
int ui_test_rel_foot(int k, int i, int *top)
{
    int st[9];
    if (k < 0 || k >= REL_ROWS || i < 0 || i >= S.nrel[k])
        return *top = 0;
    window_state(S.browser_w, st);
    *top = S.relc[k][i].y1 + st[4] - st[6];
    return S.relc[k][i].y0 + st[4] - st[6];
}
int ui_test_signin_jf(void) { return S.page == PG_SIGNIN && S.si_mode == SI_JF; }
int ui_test_signin_mode(void) { return S.page == PG_SIGNIN ? S.si_mode : -1; }
int ui_test_jf_servers(void) { return S.njf; }
int ui_test_dl_servers(void) { return S.ndl; }
void ui_test_window_state(int w, int *st) { window_state(w, st); }
int ui_test_dlna_found(void) { return S.page == PG_SIGNIN && S.si_mode == SI_DLNA ? S.nfound : -1; }
int ui_test_server_kind(void) { return S.px.kind; }
int ui_test_field(void) { return S.field; }
int ui_test_button_xy(int w, int id, int *x, int *y)
{
    int st[9], x0 = 0, y0 = 0, x1 = 0, y1 = 0, found = 0;
    window_state(w, st);
    if (w == S.browser_w && (id == B_BACK || id == B_REFRESH)) {
        header_button(id, st[3] - st[1], &x0, &y0, &x1, &y1);
        found = 1;
    }
    if (w == S.browser_w && S.page == PG_SIGNIN) {
        si_layout();
        for (int i = 0; i < si_n; i++)
            if (si_box[i].id == id) {
                x0 = si_box[i].x0; y0 = si_box[i].y0; x1 = si_box[i].x1; y1 = si_box[i].y1;
                found = 1;
            }
    }
    if (w == S.browser_w && S.page == PG_DETAILS && id >= 1000 && id < 1000 * (REL_ROWS + 1) && (id % 1000) < S.nrel[id / 1000 - 1]) {
        int k = id / 1000 - 1, i = id % 1000;      /* More like this, extras */
        x0 = S.relc[k][i].x0; y0 = S.relc[k][i].y0; x1 = S.relc[k][i].x1; y1 = S.relc[k][i].y1;
        found = 1;
    }
    for (int i = 0; w == S.browser_w && S.page == PG_GRID && S.lib.on && i < S.lib.nbtn; i++)
        if (S.lib.btn[i].id == id) {
            x0 = S.lib.btn[i].x0; y0 = S.lib.btn[i].y0; x1 = S.lib.btn[i].x1; y1 = S.lib.btn[i].y1;
            found = 1;
        }
    if (w == S.browser_w && S.page == PG_GRID && S.lib.on && id >= LB_AZ && id < LB_AZ + 27) {
        az_box(id - LB_AZ, &x0, &y0, &x1, &y1);
        found = 1;
    }
    for (int k = 0; w == S.browser_w && strip_h() && k <= S.libs.n && k < LIB_TABS; k++)
        if (TB_TAB + k == id && tab_box(k, &x0, &y0, &x1, &y1) == 0)
            found = 1;
    for (int i = 0; w == S.browser_w && S.page == PG_GRID && S.home.on && i < S.home.nbtn; i++)
        if (S.home.btn[i].id == id) {
            x0 = S.home.btn[i].x0; y0 = S.home.btn[i].y0; x1 = S.home.btn[i].x1; y1 = S.home.btn[i].y1;
            found = 1;
        }
    for (int k = 0; w == S.browser_w && S.page == PG_GRID && S.home.on && k < S.home.npick; k++)
        if (HB_DOT + k == id) {
            x0 = S.home.dot[k].x0; y0 = S.home.dot[k].y0; x1 = S.home.dot[k].x1; y1 = S.home.dot[k].y1;
            found = 1;
        }
    for (int r = 0; w == S.browser_w && S.page == PG_GRID && S.home.on && r < S.home.nrows; r++)
        if (HB_ROW + r == id) {
            x0 = 40; y0 = S.home.lay[r].title_y; x1 = S.home.lay[r].title_x1; y1 = S.home.lay[r].title_y + 40;
            found = 1;
        }
    for (int i = 0; w == S.browser_w && S.page == PG_GRID && S.show.on && i < S.show.nbtn; i++)
        if (S.show.btn[i].id == id) {
            x0 = S.show.btn[i].x0; y0 = S.show.btn[i].y0; x1 = S.show.btn[i].x1; y1 = S.show.btn[i].y1;
            found = 1;
        }
    for (int i = 0; w == S.browser_w && S.page == PG_GRID && S.show.on && i < S.show.ntab; i++)
        if (SH_TAB + i == id) {
            x0 = S.show.tab[i].x0; y0 = S.show.tab[i].y0; x1 = S.show.tab[i].x1; y1 = S.show.tab[i].y1;
            found = 1;
        }
    for (int i = 0; w == S.browser_w && S.page == PG_DETAILS && i < S.nbtn; i++)
        if (S.btn[i].id == id) {
            x0 = S.btn[i].x0; y0 = S.btn[i].y0; x1 = S.btn[i].x1; y1 = S.btn[i].y1;
            found = 1;
        }
    if (!found)
        return -1;
    *x = (x0 + x1) / 2 + st[1] - st[5];
    *y = (y0 + y1) / 2 + st[4] - st[6];
    return 0;
}
int ui_test_badge(int i) { return i >= 0 && i < S.list.n ? badge_kind(&S.list.v[i]) : -1; }
int ui_test_home(int *rows, int *pick) { *rows = S.home.nrows; *pick = S.home.pick; return S.home.on && S.page == PG_GRID; }
const char *ui_test_home_row(int r, int *start, int *n, int *vis)
{
    if (r < 0 || r >= S.home.nrows)
        return NULL;
    *start = S.home.row[r].start;
    *n = S.home.row[r].n;
    *vis = S.home.lay[r].vis;
    return S.home.row[r].title;
}
int ui_test_az_letter(const char *title) { return az_letter(title); }
int ui_test_lib(int *view, int *sort, int *unwatched)
{
    *view = S.lib.view;
    *sort = S.lib.sort;
    *unwatched = S.lib.unwatched;
    return S.lib.on && S.page == PG_GRID;
}
int ui_test_tab(int *current) { *current = tab_current(); return strip_h() ? S.libs.n + 1 : 0; }
int ui_test_show(int *season) { *season = S.show.season; return S.show.on && S.page == PG_GRID; }
const char *ui_test_user(int *nusers)
{
    *nusers = S.nusers;
    return S.user_title;
}

const char *ui_test_show_text(int what)
{
    static char t[400];
    if (what == 2) {                /* the buttons' labels */
        t[0] = 0;
        for (int i = 0; i < S.show.nbtn; i++)
            snprintf(t + strlen(t), sizeof(t) - strlen(t), "%s|", S.show.btn[i].t);
        return t;
    }
    return what == 0 ? S.show.title : S.show.count;
}
const char *ui_test_button(int id)
{
    for (int i = 0; i < S.nbtn; i++)
        if (S.btn[i].id == id)
            return S.btn[i].label;
    return NULL;
}
int ui_test_hover(void) { return S.hover; }
int ui_test_player(void) { return S.pl.on; }
int ui_test_upnext(void) { return S.pl.upnext_cs != 0; }
const char *ui_test_query(void) { return searching() ? S.query : ""; }
const char *ui_test_det(int what)
{
    return what == 0 ? S.det_title : what == 1 ? S.det_meta : what == 2 ? S.det_how :
           S.det_nlines ? S.det_lines[0] : "";
}
int ui_test_psize(void) { return S.psize; }
#endif

#ifndef MATINEE_NO_MAIN
int main(int argc, char **argv) { return matinee_main(argc, argv); }
#endif
