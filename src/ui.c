/*
 * ui.c - PlexRO's desktop front end.
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
 * <Wimp$ScrapDir>.PlexRO.PlayN, typed as video/mp4, and given to the
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
 * Choices: Choices:PlexRO.Choices, written to <Choices$Write>.PlexRO.
 * PlexRO$ChoicesDir (a directory) replaces both, for the host test.
 *
 * Plain Wimp SWIs, as in Reel: no Toolbox, no Templates.
 * Part of riscos-plex. GPL v2 or later.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <kernel.h>

#include "plex.h"
#include "caps.h"
#include "handoff.h"
#include "net.h"
#include "ui.h"
#include "version.h"
#include "proginfo.h"
#include "draw.h"
#include "player.h"

#define OS_File                    0x08
#define OS_ReadMonotonicTime       0x42
#define OS_ReadModeVariable        0x35
#define OS_SpriteOp                0x2E
#define Wimp_Initialise            0x400C0
#define Wimp_CreateWindow          0x400C1
#define Wimp_CreateIcon            0x400C2
#define Wimp_OpenWindow            0x400C5
#define Wimp_CloseWindow           0x400C6
#define Wimp_Poll                  0x400C7
#define Wimp_RedrawWindow          0x400C8
#define Wimp_GetRectangle          0x400CA
#define Wimp_GetWindowState        0x400CB
#define Wimp_SetIconState          0x400CD
#define Wimp_GetPointerInfo        0x400CF
#define Wimp_DragBox               0x400D0
#define Wimp_ForceRedraw           0x400D1
#define Wimp_SetCaretPosition      0x400D2
#define Wimp_GetCaretPosition      0x400D3
#define Wimp_CreateMenu            0x400D4
#define Wimp_SetExtent             0x400D7
#define Wimp_ProcessKey            0x400DC
#define Wimp_CloseDown             0x400DD
#define Wimp_StartTask             0x400DE
#define Wimp_ReportError           0x400DF
#define Wimp_PollIdle              0x400E1
#define Wimp_PlotIcon              0x400E2
#define Wimp_SendMessage           0x400E7
#define Wimp_TextOp                0x400F9
#define Hourglass_On               0x406C0
#define Hourglass_Off              0x406C1
#define DragASprite_Start          0x42400
#define DragASprite_Stop           0x42401
#define TaskManager_EnumerateTasks 0x42681
#define JPEG_Info                  0x49980
#define JPEG_PlotScaled            0x49982
#define MimeMap_Translate          0x50B00

#define MSG_QUIT        0
#define MSG_DATASAVE    1
#define MSG_DATASAVEACK 2
#define MSG_DATALOAD    3
#define MSG_DATALOADACK 4
#define MSG_DATAOPEN    5
#define MSG_PREQUIT     8
#define MSG_MODECHANGE  0x400C1

#define APP       "PlexRO"
#define ICON      "!plexro"
#define PURPOSE   "Plex client"

/* The browser's layout, OS units: posters are 2:3, in three sizes */
static const struct { int w, h; const char *name; } sizes[3] = {
    { 168, 252, "Small" }, { 232, 348, "Medium" }, { 320, 480, "Large" }
};
#define TILE_W    (sizes[S.psize].w)
#define POSTER_H  (sizes[S.psize].h)
#define TEXT_H    96                /* the title and a line under it */
#define TILE_H    (POSTER_H + TEXT_H)
#define GAP       36
#define HEADER_H  128               /* Back, where you are, the status line, Refresh */
#define BR_MIN_W  (2 * (TILE_W + GAP) + GAP)
#define BTN       64                /* a button's height */

/* The details page */
#define DET_BTN_MAX 8

#define SAVE_STEP (256 * 1024)      /* bytes saved each null event */
#define FILE_MAX  4294967295LL      /* 4GB-1: the biggest file FileCore holds */
#define PIN_EVERY 200               /* cs between sign-in checks */
#define POSTER_CACHE_MAX (48 << 20) /* bytes of poster sprites kept */
#define HISTORY_MAX 16
#define SERVERS_MAX 8
#define PEND_MAX  4

#ifdef __riscos__
/* UnixLib: the heap in a dynamic area of its own (the posters live there) */
const char *const __dynamic_da_name = "PlexRO Heap";
int __dynamic_da_max_size = 512 << 20;    /* the posters, and the player's pictures and packets */
#endif

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
static void force_redraw(int w, int x0, int y0, int x1, int y1);

/* ---- state -------------------------------------------------------------- */

typedef struct poster {
    struct poster *next;
    char thumb[256];
    int *area;                      /* a sprite area with one sprite, "p" (rounded: masked corners) */
    size_t bytes;
    int failed;
} poster_t;

typedef struct {
    char line[2][80];               /* Latin-1, cut to fit the tile */
    poster_t *poster;
} disp_t;

typedef struct {
    char path[256];
    char title[64];
    int sel;
} hist_t;

typedef struct {
    int ref;                        /* the DataOpen's my_ref */
    int player;
    char path[256];
} pend_t;

static struct {
    int task, bar_icon, proginfo;
    int browser_w, save_w;
    int browser_open;
    int page;                       /* what the window shows: PG_GRID, PG_DETAILS or PG_SIGNIN */
    int grid_sy;                    /* the grid's scroll, kept while a details page is shown */
    int field;                      /* the sign-in page's field with the caret: 0 address, 1 token */
    int xeig, yeig, scr_w, scr_h;   /* the screen, OS units */

    plex_ctx px;
    char agent[64];
    int player, quality, direct, psize;

    plex_server servers[SERVERS_MAX];
    int nservers;

    /* sign-in */
    long pin_id;
    int pin_next;                   /* monotonic time of the next check */
    char code[16], si_status[160], si_addr[256], si_tok[256];

    /* browser */
    plex_list list;
    int have_list;
    disp_t *disp;
    char path[256];                 /* the list shown */
    hist_t hist[HISTORY_MAX];
    int nhist;
    int sel, cols, width;
    char title[96], where[256], status[200];
    char query[100];                /* the search field (Latin-1), while a search is shown */
    int search_due;                 /* when to search for what's been typed (0: nothing to do) */
    int posters_wanted;             /* a scan for missing posters is due */
    int hover, in_browser;          /* the poster under the pointer (-1: none) */
    poster_t *cache;
    size_t cache_bytes;

    /* details */
    plex_list det;                  /* one item, from plex_details() */
    int have_det, det_i;            /* det_i: its place in the list shown (-1: gone) */
    poster_t *det_art;
    char det_title[120], det_meta[160], det_how[200], det_how_l[2][160], det_lines[10][160];
    int det_how_n;
    int det_nlines, det_h;
    /* the rest of the metadata: label and value lines, and the cast */
    char det_cred[8][2][160];
    int det_ncred;
    struct { poster_t *photo; char name[64], role[64]; int x0, y0; } cast[16];
    int ncast, cast_wanted;         /* cast_wanted: photos still to fetch (null events) */
    struct { int id, x0, y0, x1, y1; char label[48]; } btn[DET_BTN_MAX + 4];
    int nbtn;

    /* menus */
    int menu_kind;                  /* 1 icon bar, 2 item, 3 subtitles */
    int menu_x, menu_y;

    /* the built-in player: what it's playing, and for Plex */
    struct {
        int on;
        plex_list det;              /* the video's details (one item) */
        play_t p;                   /* how it's played (the stream's session) */
        plex_playing pq;            /* its play queue on the server, and the session */
        int prev_page;              /* the page to go back to */
        int prev_st[9];             /* and the window's size then (0: unchanged) */
        int tl_cs;                  /* when the server is told where it's got to next */
        int upnext_cs;              /* the next episode starts then (0: no card) */
        plex_list next;             /* the next episode */
        int menu_audio;             /* the Sound track submenu is the server's tracks */
    } pl;
    int overlay, pic_mode;          /* Choices for the built-in player */
    double volume;

    /* hand-offs waiting for DataLoadAck */
    pend_t pend[PEND_MAX];
    int play_n;
    char play_why[160];

    /* the save box, and a save in progress */
    char sv_name[256];
    char sv_leaf[200];
    char sv_url_key[256], sv_title[80];
    int64_t sv_size;
    int sv_type, sv_ok;             /* sv_ok: the item can be saved */
    int dragging, drag_sprite, datasave_ref;
    struct {
        int active;
        net_stream *ns;
        FILE *f;
        char path[256], title[80];
        int64_t size, done;
        int type;
        int t0;                     /* when it started (monotonic time) */
    } save;

    /* a speed test: part of a file read from the server, and timed */
    struct {
        int active;
        net_stream *ns;
        int t0, start_cs;           /* when the reading started; how long the server took to answer */
        int64_t done;
        int kbps;                   /* what the file needs (0: not known) */
        char title[80];
    } speed;
} S;

/* ---- small helpers ------------------------------------------------------ */

static int now_cs(void)
{
    _kernel_swi_regs r;
    return swi(OS_ReadMonotonicTime, &r) ? (int)(clock() * 100 / CLOCKS_PER_SEC) : r.r[0];
}

static void hourglass(int on)
{
    _kernel_swi_regs r;
    swi(on ? Hourglass_On : Hourglass_Off, &r);
}

static void report(const char *fmt, ...)
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
static int ask(const char *text)
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
static void latin1(const char *s, char *out, size_t size)
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
static void hms(int64_t ms, char *out, size_t size)
{
    long s = (long)(ms / 1000);
    if (s >= 3600)
        snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

static void make_dir(const char *path)
{
    _kernel_swi_regs r;
    r.r[0] = 8;                     /* OS_File 8: create a directory */
    r.r[1] = (intptr_t)path;
    r.r[4] = 0;
    swi(OS_File, &r);
}

static void set_type(const char *path, int type)
{
    _kernel_swi_regs r;
    r.r[0] = 18;                    /* OS_File 18: set the filetype */
    r.r[1] = (intptr_t)path;
    r.r[2] = type;
    swi(OS_File, &r);
}

/* A filetype from a MIME type ("video/mp4") or an extension ("mkv") */
static int mime_type(int from, const char *what, int fallback)
{
    _kernel_swi_regs r;
    r.r[0] = from;                  /* 2: MIME type, 3: extension */
    r.r[1] = (intptr_t)what;
    r.r[2] = 0;                     /* to a filetype */
    if (swi(MimeMap_Translate, &r) || r.r[3] < 0 || r.r[3] > 0xFFF)
        return fallback;
    return r.r[3];
}

#ifdef __riscos__
#define SEP "."
#else
#define SEP "/"                     /* the host test's files */
#endif

/* ---- Choices ------------------------------------------------------------ */

static const char *player_names[PLAYER_COUNT] = { "Built-in", "ReelEGL", "Reel" };
static const char *pic_names[PIC_COUNT] = { "Fit", "Fill (crop)", "Stretch" };

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
        snprintf(path, sizeof(path), "Choices:" APP ".Choices");
    }
    return fopen(path, write ? "w" : "r");
}

static void choices_save(void)
{
    FILE *f = choices_open(1);
    if (!f)
        return;
    fprintf(f, "# " APP " choices\n"
            "client_id %s\naccount_token %s\nserver_base %s\nserver_token %s\n"
            "server_name %s\nserver_id %s\nserver_local %d\nplayer %s\nquality %d\ndirect_play %d\n"
            "poster_size %d\nchoices_version 2\nhardware_overlay %d\npicture %d\nvolume %d\n",
            S.px.client_id, S.px.account_token, S.px.base, S.px.token,
            S.px.server_name, S.px.server_id, S.px.local, player_names[S.player], S.quality, S.direct,
            S.psize, S.overlay, S.pic_mode, (int)(S.volume * 100 + 0.5));
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

static void choices_load(void)
{
    char line[400], v[300], id[48] = "";
    FILE *f = choices_open(0);
    plex_ctx *c = &S.px;
    int version = 1, player = PLAYER_BUILTIN;
    S.player = PLAYER_BUILTIN;      /* the built-in player by default; ReelEGL and Reel the others */
    S.quality = Q_720;
    S.direct = 1;
    S.psize = 1;                    /* Medium */
    S.overlay = 1;
    S.pic_mode = PIC_FIT;
    S.volume = 1;
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
        snprintf(id, sizeof(id), "plexro-%08x%08x", a, b);
    }
    plex_ctx_init(c, id, PLEXRO_VERSION);
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
        }
        fclose(f);
    }
    {
        const char *tv = getenv(APP "$PlexTV");     /* another plex.tv (the tests' fake) */
        if (tv && *tv)
            snprintf(c->plextv, sizeof(c->plextv), "%s", tv);
    }
}

/* ---- windows: making them ------------------------------------------------- */

typedef struct { int box[4]; unsigned flags; int data[3]; } icon_t;

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
static char sv_sprite[16];
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


static void window_state(int w, int *st)
{
    _kernel_swi_regs r;
    st[0] = w;
    r.r[1] = (intptr_t)st;
    swi(Wimp_GetWindowState, &r);
}

static void open_front(int w, int x0, int y0, int x1, int y1, int sx, int sy)
{
    int b[8];
    _kernel_swi_regs r;
    b[0] = w; b[1] = x0; b[2] = y0; b[3] = x1; b[4] = y1; b[5] = sx; b[6] = sy; b[7] = -1;
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
}

static void close_window(int w)
{
    _kernel_swi_regs r;
    int b[1];
    b[0] = w;
    r.r[1] = (intptr_t)b;
    swi(Wimp_CloseWindow, &r);
}

static void set_caret(int w, int icon, const char *text)
{
    _kernel_swi_regs r;
    r.r[0] = w;
    r.r[1] = icon;
    r.r[2] = 0; r.r[3] = 0;
    r.r[4] = icon < 0 ? (1 << 25) : -1;         /* in the work area: an invisible caret */
    r.r[5] = text ? (int)strlen(text) : 0;
    swi(Wimp_SetCaretPosition, &r);
}

static void si_set_status(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(S.si_status, sizeof(S.si_status), fmt, a);
    va_end(a);
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, -HEADER_H);
}

static void set_status(const char *fmt, ...)
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

/* ---- posters ------------------------------------------------------------------ */

static void cache_free_all(void)
{
    while (S.cache) {
        poster_t *p = S.cache;
        S.cache = p->next;
        free(p->area);
        free(p);
    }
    S.cache_bytes = 0;
    for (int i = 0; S.disp && i < S.list.n; i++)
        S.disp[i].poster = NULL;
    S.det_art = NULL;
    for (int i = 0; i < S.ncast; i++)
        S.cast[i].photo = NULL;
    S.cast_wanted = S.ncast > 0;
}

/* Frees posters the list shown doesn't use, once the cache is big */
static void cache_trim(void)
{
    poster_t **pp = &S.cache;
    if (S.cache_bytes <= POSTER_CACHE_MAX)
        return;
    while (*pp) {
        poster_t *p = *pp;
        int used = 0;
        for (int i = 0; S.disp && i < S.list.n && !used; i++)
            used = S.disp[i].poster == p;
        for (int i = 0; i < S.ncast && !used; i++)
            used = S.cast[i].photo == p;
        if (!used && p != S.det_art) {
            *pp = p->next;
            S.cache_bytes -= p->bytes;
            free(p->area);
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

static poster_t *cache_find(const char *thumb)
{
    for (poster_t *p = S.cache; p; p = p->next)
        if (!strcmp(p->thumb, thumb))
            return p;
    return NULL;
}


/* A sprite area with one 32bpp sprite "p", w x h pixels at the screen's
   resolution, filled with the window's grey. Made by hand (the header's
   layout is fixed), so no SWI is needed. round > 0: a 1bpp mask that
   rounds its corners (round pixels). */
static int *sprite_make(int w, int h, int round, size_t *bytes)
{
    size_t image = (size_t)w * h * 4, words = (size_t)(w + 31) / 32;
    size_t mask = round > 0 ? words * 4 * h : 0, total = 16 + 44 + image + mask;
    int *a = malloc(total);
    int *s;
    unsigned *px;
    if (!a)
        return NULL;
    a[0] = (int)total;              /* area size */
    a[1] = 1;                       /* one sprite */
    a[2] = 16;                      /* the first at +16 */
    a[3] = (int)total;              /* free space after it */
    s = a + 4;
    s[0] = (int)(44 + image + mask);        /* to the next sprite */
    memset(&s[1], 0, 12);
    ((char *)&s[1])[0] = 'p';
    s[4] = w - 1;                   /* width in words - 1 (32bpp: one word a pixel) */
    s[5] = h - 1;
    s[6] = 0;
    s[7] = 31;
    s[8] = 44;                      /* image */
    s[9] = (int)(44 + (mask ? image : 0));  /* the mask (or none: the image again) */
    /* new-format mode word: 32bpp (type 6), the screen's dpi */
    s[10] = (int)(1u | ((unsigned)(180 >> S.xeig) << 1) | ((unsigned)(180 >> S.yeig) << 14) | (6u << 27));
    px = (unsigned *)(s + 11);
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i] = 0x001F1A18;         /* C_BG as 0xBBGGRR */
    if (mask)                       /* all solid; sprite_round() cuts the corners */
        memset((char *)s + 44 + image, 0xFF, mask);
    *bytes = total;
    return a;
}

/* The JPEG drawn into the sprite, in the middle: as big as fits, or
   (cover) filling it, cut at the edges. 0 = ok. */
static int jpeg_into(int *area, int w, int h, const char *jpeg, size_t len, int cover)
{
    _kernel_swi_regs r, out;
    int jw, jh, tw, th, scale[4];
    _kernel_oserror *e;

    r.r[0] = 1;                     /* the dimensions */
    r.r[1] = (intptr_t)jpeg;
    r.r[2] = (int)len;
    if (swi(JPEG_Info, &r))
        return -1;
    jw = r.r[2];
    jh = r.r[3];
    if (jw <= 0 || jh <= 0)
        return -1;
    if (((int64_t)w * jh <= (int64_t)h * jw) != !!cover) {   /* width decides */
        tw = w;
        th = (int)((int64_t)jh * w / jw);
    } else {
        th = h;
        tw = (int)((int64_t)jw * h / jh);
    }
    scale[0] = tw; scale[1] = th; scale[2] = jw; scale[3] = jh;

    r.r[0] = 60 + 512;              /* output to the sprite (by pointer) */
    r.r[1] = (intptr_t)area;
    r.r[2] = (intptr_t)(area + 4);
    r.r[3] = 0;
    if (_kernel_swi(OS_SpriteOp, &r, &out))
        return -1;
    r.r[0] = (intptr_t)jpeg;
    r.r[1] = ((w - tw) / 2) << S.xeig;
    r.r[2] = ((h - th) / 2) << S.yeig;
    r.r[3] = (intptr_t)scale;
    r.r[4] = (int)len;
    r.r[5] = 1;                     /* dithered */
    e = swi(JPEG_PlotScaled, &r);
    swi(OS_SpriteOp, &out);         /* output back where it was */
    return e ? -1 : 0;
}

/* Rounded corners, round pixels: outside the curve the mask hides the
   pixel; on it, the pixel is mixed with the window's grey by how much of
   it is inside (4 x 4 samples), so the edge is smooth */
static void sprite_round(int *area, int w, int h, int round)
{
    int *s = area + 4, words = (w + 31) / 32;
    unsigned *px = (unsigned *)(s + 11), *m = (unsigned *)((char *)s + s[9]);
    const int bg[3] = { 24, 26, 31 };
    double rr = round;
    if (s[9] == s[8] || round < 1)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = 0;
            if ((x >= round && x < w - round) || (y >= round && y < h - round))
                continue;           /* not in a corner */
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    double fx = x + (i + 0.5) / 4, fy = y + (j + 0.5) / 4;
                    double cx = fx < rr ? rr : fx > w - rr ? w - rr : fx;
                    double cy = fy < rr ? rr : fy > h - rr ? h - rr : fy;
                    n += (fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= rr * rr;
                }
            if (n < 8) {
                m[y * words + x / 32] &= ~(1u << (x & 31));
            } else if (n < 16) {
                unsigned p = px[y * w + x];
                int c[3] = { (int)(p & 255), (int)(p >> 8 & 255), (int)(p >> 16 & 255) };
                for (int k = 0; k < 3; k++)
                    c[k] = (c[k] * n + bg[k] * (16 - n)) / 16;
                px[y * w + x] = (unsigned)c[0] | (unsigned)c[1] << 8 | (unsigned)c[2] << 16;
            }
        }
}

/* The backdrop: darkened a little, and faded into the window's grey over
   its lower part, so the title reads on it */
static void sprite_fade(int *area, int w, int h)
{
    unsigned *px = (unsigned *)(area + 4 + 11);
    const int bg[3] = { 24, 26, 31 };
    int from = h * 45 / 100;
    for (int y = 0; y < h; y++) {
        /* k: how much of the grey, in 256ths */
        int k = y < from ? 40 : 40 + (216 * (y - from) * (y - from)) / ((h - from) * (h - from));
        for (int x = 0; x < w; x++) {
            unsigned p = px[y * w + x];
            int c[3] = { (int)(p & 255), (int)(p >> 8 & 255), (int)(p >> 16 & 255) };
            for (int i = 0; i < 3; i++)
                c[i] = (c[i] * (256 - k) + bg[i] * k) >> 8;
            px[y * w + x] = (unsigned)c[0] | (unsigned)c[1] << 8 | (unsigned)c[2] << 16;
        }
    }
}

/* A picture from the server (a poster, or a backdrop when art), into the
   cache under key */
/* art: 0 a poster (rounded corners), 1 a backdrop (faded), 2 a person's
   photo (round, filling the circle) */
static poster_t *poster_fetch(const char *thumb, const char *key, int w, int h, int art)
{
    int round = art == 2 ? w / 2 : art ? 0 : 12 >> S.xeig;
    poster_t *p = calloc(1, sizeof(*p));
    char *jpeg = NULL;
    size_t len = 0;
    if (!p)
        return NULL;
    snprintf(p->thumb, sizeof(p->thumb), "%s", key);
    if (plex_poster(&S.px, thumb, w, h, &jpeg, &len) != 0 ||
        !(p->area = sprite_make(w, h, round, &p->bytes)) ||
        jpeg_into(p->area, w, h, jpeg, len, art != 0) != 0) {
        free(p->area);
        p->area = NULL;
        p->bytes = 0;
        p->failed = 1;
    } else if (art == 1) {
        sprite_fade(p->area, w, h);
    } else {
        sprite_round(p->area, w, h, round);
    }
    free(jpeg);
    p->next = S.cache;
    S.cache = p;
    S.cache_bytes += p->bytes;
    return p;
}

/* ---- the browser's layout ------------------------------------------------------ */

static void tile_box(int i, int *x0, int *y0, int *x1, int *y1)
{
    int col = i % S.cols, row = i / S.cols;
    *x0 = GAP + col * (TILE_W + GAP);
    *x1 = *x0 + TILE_W;
    *y1 = -HEADER_H - GAP - row * (TILE_H + GAP);
    *y0 = *y1 - TILE_H;
}

static int layout_cols(int width)
{
    int c = (width - GAP) / (TILE_W + GAP);
    return c < 1 ? 1 : c;
}

static int list_height(void)
{
    int rows = S.have_list ? (S.list.n + S.cols - 1) / S.cols : 0;
    return HEADER_H + GAP + rows * (TILE_H + GAP);
}

static void set_extent(void)
{
    int b[4];
    _kernel_swi_regs r;
    int h = S.page == PG_DETAILS ? S.det_h : S.page == PG_SIGNIN || S.page == PG_PLAYER ? 1100 : list_height();
    if (h < S.scr_h)
        h = S.scr_h;                /* at least the screen: the window can be made taller */
    b[0] = 0; b[1] = -h; b[2] = S.scr_w; b[3] = 0;
    r.r[0] = S.browser_w;
    r.r[1] = (intptr_t)b;
    swi(Wimp_SetExtent, &r);
}

static void force_redraw(int w, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = x0; r.r[2] = y0; r.r[3] = x1; r.r[4] = y1;
    swi(Wimp_ForceRedraw, &r);
}

static void redraw_tile(int i)
{
    int x0, y0, x1, y1;
    if (i < 0 || !S.browser_open)
        return;
    tile_box(i, &x0, &y0, &x1, &y1);
    force_redraw(S.browser_w, x0 - 16, y0 - 16, x1 + 20, y1 + 16);
}

/* The two lines under each poster, cut to fit */
static void make_disp(void)
{
    free(S.disp);
    S.disp = calloc(S.list.n ? S.list.n : 1, sizeof(disp_t));
    if (!S.disp)
        return;
    for (int i = 0; i < S.list.n; i++) {
        const plex_item *it = &S.list.v[i];
        latin1(it->title, S.disp[i].line[0], sizeof(S.disp[i].line[0]));
        latin1(it->subtitle ? it->subtitle : "", S.disp[i].line[1], sizeof(S.disp[i].line[1]));
        draw_fit(D_BOLD, S.disp[i].line[0], TILE_W);
        draw_fit(D_BODY, S.disp[i].line[1], TILE_W);
        S.disp[i].poster = it->thumb ? cache_find(it->thumb) : NULL;
    }
}

/* ---- the browser: drawing ------------------------------------------------------ */

static void plot(const icon_t *ic)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)ic;
    swi(Wimp_PlotIcon, &r);
}

/* A sprite from our cache, plotted in the middle of a work area box */
static void plot_sprite(const poster_t *p, int x0, int y0, int x1, int y1)
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
static void header_button(int id, int vis_w, int *x0, int *y0, int *x1, int *y1)
{
    *y0 = -HEADER_H + 32;
    *y1 = *y0 + BTN;
    *x0 = id == B_BACK ? 28 : id == B_SEARCH ? vis_w - 28 - 80 - 20 - 80 : vis_w - 28 - 80;
    *x1 = *x0 + 80;
}

static int header_has(int id)
{
    return S.page != PG_SIGNIN && (id == B_BACK || id == B_REFRESH || (id == B_SEARCH && S.page == PG_GRID));
}

/* The grid is showing a search: its field is in the bar */
static int searching(void)
{
    return S.page == PG_GRID && S.have_list && !strncmp(S.path, "search:", 7);
}

/* ox, oy: the work area's origin on the screen; vis_w: the width shown */
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
        int fx0 = tx, fx1 = right, fy0 = -HEADER_H + 32, fy1 = fy0 + BTN, qw, room = fx1 - fx0 - 64;
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
        return;
    }
    snprintf(where, sizeof(where), "%s", S.where);
    draw_fit(D_BOLD, where, right - tx);
    draw_text(D_BOLD, ox + tx, oy - 58, where, C_TEXT, C_HEADER);
    snprintf(where, sizeof(where), "%s", S.status);
    draw_fit(D_BODY, where, right - tx);
    draw_text(D_BODY, ox + tx, oy - 100, where, C_SUB, C_HEADER);
}

static const char *kind_name(const plex_item *it)
{
    return it->kind == PI_VIDEO ? "Video" : it->kind == PI_FOLDER ? "Folder" : "Not yet";
}

static void draw_tile(int i, int ox, int oy)
{
    const plex_item *it = &S.list.v[i];
    disp_t *d = &S.disp[i];
    int x0, y0, x1, y1, py0;
    tile_box(i, &x0, &y0, &x1, &y1);
    py0 = y1 - POSTER_H;
    /* the ring round the selected (or pointed at) one, with a gap; or a
       shadow */
    if (i == S.sel || i == S.hover) {
        draw_round(ox + x0 - 12, oy + py0 - 12, ox + x1 + 12, oy + y1 + 12, 24,
                   i == S.sel ? C_ACCENT : C_HOVER, C_BG);
        draw_round(ox + x0 - 6, oy + py0 - 6, ox + x1 + 6, oy + y1 + 6, 18, C_BG, i == S.sel ? C_ACCENT : C_HOVER);
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
    if (it->kind == PI_VIDEO && it->view_offset_ms > 0 && it->duration_ms > 0) {
        /* how far it's been watched */
        int part = (int)((int64_t)(TILE_W - 32) * it->view_offset_ms / it->duration_ms);
        draw_rect(ox + x0 + 12, oy + py0 + 12, ox + x1 - 12, oy + py0 + 24, RGB(0, 0, 0));
        draw_rect(ox + x0 + 12, oy + py0 + 12, ox + x0 + 12 + (part < 8 ? 8 : part), oy + py0 + 24, C_ACCENT);
    } else if ((it->kind == PI_VIDEO || it->kind == PI_FOLDER) && !it->watched && it->rating_key) {
        /* not watched yet: a corner turned down */
        draw_tri(ox + x1 - 40, oy + y1, ox + x1, oy + y1, ox + x1, oy + y1 - 40, C_ACCENT);
    }
    draw_text(D_BOLD, ox + x0, oy + py0 - 40, d->line[0], it->kind == PI_OTHER ? C_SUB : C_TEXT, C_BG);
    draw_text(D_BODY, ox + x0, oy + py0 - 80, d->line[1], C_SUB, C_BG);
}

static void det_redraw(int ox, int oy, int vis_w, int cy0, int cy1);
static void signin_redraw(int ox, int oy, int vis_w);

static void redraw(int *b)
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
            else if (S.have_list && S.disp && S.cols > 0) {
                int top = -HEADER_H - GAP;
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
static int poster_step(void)
{
    int st[9], vtop, vbot, rf, rl;
    if (!S.browser_open || !S.have_list || !S.disp || S.page != PG_GRID) {
        S.posters_wanted = 0;
        return 0;
    }
    window_state(S.browser_w, st);
    vtop = st[6];
    vbot = st[6] - (st[4] - st[2]);
    rf = (-HEADER_H - GAP - vtop) / (TILE_H + GAP);
    rl = (-HEADER_H - GAP - vbot) / (TILE_H + GAP) + 1;     /* and a row more */
    if (rf < 0)
        rf = 0;
    for (int i = rf * S.cols; i < (rl + 1) * S.cols && i < S.list.n; i++) {
        const plex_item *it = &S.list.v[i];
        if (!it->thumb || S.disp[i].poster)
            continue;
        if (!(S.disp[i].poster = cache_find(it->thumb))) {
            S.disp[i].poster = poster_fetch(it->thumb, it->thumb, TILE_W >> S.xeig, POSTER_H >> S.yeig, 0);
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

/* ---- the browser: lists --------------------------------------------------------- */

static void set_where(void)
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

static void browser_open(void)
{
    int st[9], w = 4 * (TILE_W + GAP) + GAP, h = S.scr_h - 320;
    if (h > 1100)
        h = 1100;
    if (S.browser_open) {
        window_state(S.browser_w, st);
        open_front(S.browser_w, st[1], st[2], st[3], st[4], st[5], st[6]);
    } else {
        int x0 = (S.scr_w - w) / 2, y0 = (S.scr_h - h) / 2 + 40;
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
static int show_list(const char *path, const char *back_title, int push, int sel)
{
    plex_list l;
    int e;
    hourglass(1);
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
    S.list = l;
    S.have_list = 1;
    S.det_i = -1;                   /* the details (if kept) are of a video in the old list */
    S.hover = -1;
    S.page = PG_GRID;
    snprintf(S.path, sizeof(S.path), "%s", path);
    if (!strncmp(path, "search:", 7))       /* the field shows what was searched for */
        latin1(path + 7, S.query, sizeof(S.query));
    S.sel = sel >= 0 && sel < l.n ? sel : l.n ? 0 : -1;
    make_disp();
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
        set_status("The first %d of %d.", l.n, l.total);
    else
        set_status("%d item%s.", l.n, l.n == 1 ? "" : "s");
    return 0;
}

static void det_leave(void);

static void go_back(void)
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

static void search_now(void)
{
    char path[256];
    size_t n = snprintf(path, sizeof(path), "search:");
    to_utf8(S.query, path + n, sizeof(path) - n);
    S.search_due = 0;
    show_list(path, "", 0, 0);
}

static void search_open(void)
{
    if (searching())
        return;
    S.query[0] = 0;
    S.search_due = 0;
    show_list("search:", S.list.title[0] ? S.list.title : "Home", S.have_list, 0);
}

/* A key on the search page: 1 if it was the field's */
static int search_key(int k)
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

static void refresh_list(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s", S.path);
    show_list(path, "", 0, S.sel);
}

static void browser_top(void)
{
    S.nhist = 0;
    if (S.have_list) {
        plex_list_free(&S.list);
        S.have_list = 0;
    }
    show_list("", "", 0, 0);
}

static int tile_at(int sx, int sy)
{
    int st[9], wx, wy;
    if (!S.have_list)
        return -1;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    for (int i = 0; i < S.list.n; i++) {
        int x0, y0, x1, y1;
        tile_box(i, &x0, &y0, &x1, &y1);
        if (y1 < wy - TILE_H - GAP)
            break;
        if (wx >= x0 - GAP / 2 && wx < x1 + GAP / 2 && wy >= y0 - GAP / 2 && wy < y1 + GAP / 2)
            return i;
    }
    return -1;
}

/* The header button at a screen point: B_BACK, B_REFRESH or 0 */
static int header_hit(int sx, int sy)
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

static void set_hover(int i)
{
    int old = S.hover;
    if (i == old)
        return;
    S.hover = i;
    redraw_tile(old);
    redraw_tile(i);
}

static void det_show(int i);

static void select_tile(int i)
{
    int old = S.sel;
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

/* ---- the details page -------------------------------------------------------- */

/* A video's details in the browser window, in place of the grid: the
   backdrop across the top (under the bar), faded into the window, the
   title over its foot, then the buttons, subtitles, how it will play and
   the summary. Back (the button, Backspace or Escape) returns to the grid
   where it was. */

static int det_subs_y, det_how_y, det_sum_y;    /* baselines, work area */
static int det_cred_y, det_cast_y;              /* the credits' first baseline; the cast heading's */
#define CAST_W  200                             /* a cast tile: the photo, the name and the part */
#define CAST_PH 144                             /* the photo's diameter */
#define CAST_H  (CAST_PH + 100)
#define CRED_X  300                             /* the credits' values, after their labels */
static int det_wd = 1000, art_w, art_h;         /* the width laid out for; the backdrop's box */

static const plex_item *det_item(void)
{
    return S.have_det && S.det.n ? &S.det.v[0] : NULL;
}

/* Is it the video the details are of? */
static int det_is(const plex_item *it)
{
    const plex_item *d = det_item();
    return d && it && it->rating_key && d->rating_key && !strcmp(d->rating_key, it->rating_key);
}

/* The width the window shows */
static int vis_width(void)
{
    int st[9];
    if (!S.browser_open)
        return 4 * (TILE_W + GAP) + GAP;
    window_state(S.browser_w, st);
    return st[3] - st[1];
}

/* The backdrop, for the width laid out for */
static void det_fetch_art(void)
{
    const plex_item *it = det_item();
    char key[320];
    S.det_art = NULL;
    art_w = det_wd;
    art_h = art_w * 9 / 16;
    if (art_h > 720)
        art_h = 720;
    art_w &= ~((1 << S.xeig) - 1);
    art_h &= ~((1 << S.yeig) - 1);
    if (!it || !it->art)
        return;
    snprintf(key, sizeof(key), "art:%s@%dx%d", it->art, art_w, art_h);
    if (!(S.det_art = cache_find(key))) {
        S.det_art = poster_fetch(it->art, key, art_w >> S.xeig, art_h >> S.yeig, 1);
        cache_trim();
    }
}

/* One video's details, from the server, into S.det (and its backdrop,
   when it's to be shown). 0 = ok. */
static int det_fetch(const plex_item *it, int with_art)
{
    plex_list d;
    hourglass(1);
    if (plex_details(&S.px, it, &d) != 0) {
        hourglass(0);
        report("Can't get the details: %s", S.px.err);
        return -1;
    }
    if (S.have_det)
        plex_list_free(&S.det);
    S.det = d;
    S.have_det = 1;
    S.det_art = NULL;
    for (int i = 0; i < (int)(sizeof(S.cast) / sizeof(S.cast[0])); i++)
        S.cast[i].photo = NULL;
    S.ncast = 0;
    S.cast_wanted = 1;              /* laid out by det_layout, fetched from null events */
    if (with_art)
        det_fetch_art();
    hourglass(0);
    return 0;
}

static void det_button(int id, const char *label, int *x, int *y)
{
    int w = draw_width(D_BOLD, label) + 64 + (id == D_PLAY ? 36 : id == D_SUBS ? 40 : 0);
    if (S.nbtn >= DET_BTN_MAX)
        return;
    if (*x + w > det_wd - 40 && *x > 40) {  /* a new row */
        *x = 40;
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
static void det_layout_more(const plex_item *it, int y)
{
    char t[400], v[400];
    int n = 0, cols, w = det_wd - 80;
#define CRED(label, ...) do { if (n < 8) { \
        snprintf(S.det_cred[n][0], sizeof(S.det_cred[0][0]), "%s", label); \
        snprintf(t, sizeof(t), __VA_ARGS__); latin1(t, v, sizeof(v)); \
        draw_fit(D_BODY, v, w - CRED_X); \
        snprintf(S.det_cred[n][1], sizeof(S.det_cred[0][1]), "%s", v); n++; } } while (0)
    if (it->genres) CRED("Genre", "%s", it->genres);
    if (it->directors) CRED("Directed by", "%s", it->directors);
    if (it->writers) CRED("Written by", "%s", it->writers);
    if (it->studio) CRED("Studio", "%s", it->studio);
    if (it->released) {
        nice_date(it->released, v, sizeof(v));
        CRED(it->type && !strcmp(it->type, "episode") ? "First shown" : "Released", "%s", v);
    }
    if (it->country) CRED("Country", "%s", it->country);
    if (it->rating > 0 && it->audience_rating > 0)
        CRED("Ratings", "Critics %.1f   \xc2\xb7   Audience %.1f", it->rating, it->audience_rating);
    else if (it->audience_rating > 0)
        CRED("Ratings", "Audience %.1f", it->audience_rating);
    tech_line(it, v, sizeof(v));
    if (*v) {
        char l1[400];
        snprintf(l1, sizeof(l1), "%s", v);
        CRED("File", "%s", l1);
    }
#undef CRED
    S.det_ncred = n;
    det_cred_y = y - 12;
    y -= n * 40 + (n ? 24 : 0);
    /* the cast: rows of photos with the name and the part under them */
    S.ncast = it->ncast < (int)(sizeof(S.cast) / sizeof(S.cast[0])) ? it->ncast : (int)(sizeof(S.cast) / sizeof(S.cast[0]));
    det_cast_y = 0;
    if (S.ncast) {
        det_cast_y = y - 36;
        y -= 60;
        cols = (w + 24) / (CAST_W + 24);
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
    S.det_h = -y + 40;
}

/* A cast photo not fetched yet, fetched (one each null event): 1 if one was */
static int det_cast_step(void)
{
    const plex_item *it = det_item();
    if (S.page != PG_DETAILS || !S.browser_open || !it) {
        S.cast_wanted = 0;
        return 0;
    }
    for (int i = 0; i < S.ncast && i < it->ncast; i++) {
        const char *th = it->cast[i].thumb;
        char key[320];
        int px = CAST_PH >> S.xeig;
        if (!th || S.cast[i].photo)
            continue;
        snprintf(key, sizeof(key), "cast:%s@%d", th, px);
        if (!(S.cast[i].photo = cache_find(key))) {
            S.cast[i].photo = poster_fetch(th, key, px, CAST_PH >> S.yeig, 2);
            cache_trim();
        }
        force_redraw(S.browser_w, S.cast[i].x0, S.cast[i].y0, S.cast[i].x0 + CAST_W, S.cast[i].y0 + CAST_H);
        return 1;
    }
    S.cast_wanted = 0;
    return 0;
}

/* The texts and buttons, where they go, and the page's height */
static void det_layout(void)
{
    const plex_item *it = det_item();
    static play_t p;
    caps_t k;
    char t[400], when[32];
    int x = 40, y, n = 0;
    if (!it)
        return;
    det_wd = vis_width();
    if (!art_w)
        art_h = 562;
    y = -HEADER_H - art_h - 28;
    latin1(it->title, S.det_title, sizeof(S.det_title));
    draw_fit(D_TITLE, S.det_title, det_wd - 80);
    /* year, how long, the rating: "2008  .  1h 30m  .  PG  .  7.5" */
    t[0] = 0;
#define META(...) do { if (n++) strcat(t, "   \xb7   "); snprintf(t + strlen(t), sizeof(t) - strlen(t), __VA_ARGS__); } while (0)
    if (it->type && !strcmp(it->type, "episode") && it->subtitle)
        META("%s", it->subtitle);
    if (it->year > 0)
        META("%d", it->year);
    if (it->duration_ms > 0) {
        long m = (long)(it->duration_ms / 60000);
        if (m >= 60)
            META("%ldh %02ldm", m / 60, m % 60);
        else
            META("%ldm", m);
    }
    if (it->content_rating)
        META("%s", it->content_rating);
    if (it->rating > 0)
        META("%.1f", it->rating);
#undef META
    snprintf(S.det_meta, sizeof(S.det_meta), "%s", t);
    /* the buttons */
    S.nbtn = 0;
    det_button(D_PLAY, "Play", &x, &y);
    if (it->view_offset_ms > 0) {
        hms(it->view_offset_ms, when, sizeof(when));
        snprintf(t, sizeof(t), "Resume from %s", when);
        det_button(D_RESUME, t, &x, &y);
    }
    det_button(D_START, "From the start", &x, &y);
    if (it->part_key && it->part_size <= FILE_MAX)
        det_button(D_SAVE, "Save file", &x, &y);
    det_button(D_WATCHED, it->watched ? "Mark unwatched" : "Mark watched", &x, &y);
    y -= BTN + 24;
    /* subtitles */
    if (it->nsubs) {
        int sel = plex_sub_selected(it);
        char name[120];
        latin1(sel >= 0 ? it->subs[sel].title : "None", name, sizeof(name));
        snprintf(t, sizeof(t), "Subtitles: %s", name);
        x = 40;
        det_button(D_SUBS, t, &x, &y);
        y -= BTN + 24;
        det_subs_y = 0;
    } else {
        det_subs_y = y - 36;
        y -= 60;
    }
    /* how it will play */
    caps_for(S.quality, &k);
    caps_play(&S.px, it, &k, S.direct, 1, &p);
    latin1(p.why, S.det_how, sizeof(S.det_how));
    S.det_how_n = draw_wrap(D_BODY, S.det_how, det_wd - 80, S.det_how_l, 2);
    det_how_y = y - 12;
    y -= 20 + 40 * S.det_how_n;
    /* the summary */
    {
        static char sum[2400];
        latin1(it->summary ? it->summary : "", sum, sizeof(sum));
        S.det_nlines = draw_wrap(D_BODY, sum, det_wd - 80, S.det_lines, 10);
    }
    det_sum_y = y - 12;
    y = det_sum_y - S.det_nlines * 40 - 24;
    det_layout_more(it, y);
}

static void det_redraw(int ox, int oy, int vis_w, int cy0, int cy1)
{
    const plex_item *it = det_item();
    int top = -HEADER_H;
    (void)cy0;
    (void)vis_w;
    if (!it)
        return;
    if (cy1 > top - art_h) {
        if (S.det_art && S.det_art->area)
            plot_sprite(S.det_art, 0, top - art_h, art_w, top);
        else
            draw_rect(ox, oy + top - art_h, ox + det_wd, oy + top, RGB(34, 37, 44));
        draw_text(D_TITLE, ox + 40, oy + top - art_h + 84, S.det_title, C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - art_h + 34, S.det_meta, C_SUB, C_BG);
    }
    for (int i = 0; i < S.nbtn; i++) {
        int x0 = ox + S.btn[i].x0, y0 = oy + S.btn[i].y0, x1 = ox + S.btn[i].x1, y1 = oy + S.btn[i].y1;
        unsigned bg = S.btn[i].id == D_PLAY ? C_ACCENT : C_CARD;
        int tx = x0 + 32;
        draw_round(x0, y0, x1, y1, BTN / 2, bg, C_BG);
        if (S.btn[i].id == D_PLAY) {            /* a play triangle */
            draw_glyph(G_PLAY, tx - 4, y0 + 16, tx + 28, y1 - 16, C_TEXT, bg);
            tx += 36;
        }
        draw_text(D_BOLD, tx, y0 + 22, S.btn[i].label, C_TEXT, bg);
        if (S.btn[i].id == D_SUBS)              /* a menu arrow */
            draw_glyph(G_DOWN, x1 - 52, y0 + 16, x1 - 20, y1 - 16, C_TEXT, bg);
    }
    if (det_subs_y)
        draw_text(D_BODY, ox + 40, oy + det_subs_y, "No subtitles", C_SUB, C_BG);
    for (int l = 0; l < S.det_how_n; l++)
        draw_text(D_BODY, ox + 40, oy + det_how_y - l * 40, S.det_how_l[l], C_SUB, C_BG);
    for (int l = 0; l < S.det_nlines; l++)
        draw_text(D_BODY, ox + 40, oy + det_sum_y - l * 40, S.det_lines[l], C_TEXT, C_BG);
    for (int l = 0; l < S.det_ncred; l++) {
        draw_text(D_BODY, ox + 40, oy + det_cred_y - l * 40, S.det_cred[l][0], C_SUB, C_BG);
        draw_text(D_BODY, ox + CRED_X, oy + det_cred_y - l * 40, S.det_cred[l][1], C_TEXT, C_BG);
    }
    if (det_cast_y) {
        draw_text(D_BOLD, ox + 40, oy + det_cast_y, "Cast", C_TEXT, C_BG);
        for (int i = 0; i < S.ncast; i++) {
            int x0 = S.cast[i].x0, y0 = S.cast[i].y0, px = x0 + (CAST_W - CAST_PH) / 2;
            int py0 = y0 + CAST_H - CAST_PH;
            if (y0 > cy1 || y0 + CAST_H < cy0)
                continue;
            if (S.cast[i].photo && S.cast[i].photo->area)
                plot_sprite(S.cast[i].photo, px, py0, px + CAST_PH, py0 + CAST_PH);
            else
                draw_glyph(G_CIRCLE, ox + px, oy + py0, ox + px + CAST_PH, oy + py0 + CAST_PH, C_CARD, C_BG);
            draw_text(D_BODY, ox + x0 + (CAST_W - draw_width(D_BODY, S.cast[i].name)) / 2, oy + py0 - 36,
                      S.cast[i].name, C_TEXT, C_BG);
            draw_text(D_BODY, ox + x0 + (CAST_W - draw_width(D_BODY, S.cast[i].role)) / 2, oy + py0 - 76,
                      S.cast[i].role, C_SUB, C_BG);
        }
    }
}

static void set_where(void);

/* The page laid out again, its extent set, and redrawn */
static void det_repaint(void)
{
    if (S.page != PG_DETAILS || !S.browser_open)
        return;
    det_layout();
    set_extent();
    set_where();
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
}

static void browser_open(void);

/* List item i's details, in the window */
static void det_show(int i)
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
    } else if (!S.det_art || art_w != (det_wd & ~((1 << S.xeig) - 1))) {
        hourglass(1);
        det_fetch_art();
        hourglass(0);
    }
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
static void det_leave(void)
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
static void det_refresh(void)
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
static int det_button_at(int sx, int sy)
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

/* ---- sign-in: a page of the window ------------------------------------------------- */

/* Before a server is chosen the window shows the sign-in page: the code
   to type at plex.tv/link (checked every 2 seconds), New code, and a
   server's address and token for signing in without plex.tv. The two
   fields are drawn here and typed into here (the window has the caret):
   Tab or the arrow keys move between them, Return goes on. */

static void choose_server(void);

static struct { int id, x0, y0, x1, y1; } si_box[4];

static void si_layout(void)
{
    int top = -HEADER_H;
    const int ids[4] = { S_NEWCODE, S_ADDR, S_TOK, S_USE };
    const int y1s[4] = { top - 372, top - 572, top - 652, top - 736 };
    const int x0s[4] = { 40, 220, 220, 220 };
    const int ws[4] = { 0, 620, 620, 0 };
    for (int i = 0; i < 4; i++) {
        int w = ws[i] ? ws[i] : draw_width(D_BOLD, i == 0 ? "New code" : "Use these") + 64;
        si_box[i].id = ids[i];
        si_box[i].x0 = x0s[i];
        si_box[i].x1 = x0s[i] + w;
        si_box[i].y1 = y1s[i];
        si_box[i].y0 = y1s[i] - BTN;
    }
}

static int si_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    si_layout();
    for (int i = 0; i < 4; i++)
        if (wx >= si_box[i].x0 && wx < si_box[i].x1 && wy >= si_box[i].y0 && wy < si_box[i].y1)
            return si_box[i].id;
    return 0;
}

static void signin_redraw(int ox, int oy, int vis_w)
{
    int top = -HEADER_H;
    char t[300];
    (void)vis_w;
    si_layout();
    draw_text(D_BOLD, ox + 40, oy + top - 76, "Sign in with a code", C_TEXT, C_BG);
    draw_text(D_BODY, ox + 40, oy + top - 124, "On a phone or computer, go to plex.tv/link and type this code:",
              C_SUB, C_BG);
    {   /* the code, big and spaced out, on a card */
        size_t n = 0;
        for (const char *c = S.code; *c && n + 3 < sizeof(t); c++) {
            t[n++] = *c;
            if (c[1]) { t[n++] = ' '; t[n++] = ' '; }
        }
        t[n] = 0;
        draw_round(ox + 40, oy + top - 292, ox + 460, oy + top - 160, 20, C_CARD, C_BG);
        draw_text(D_TITLE, ox + 250 - draw_width(D_TITLE, t) / 2, oy + top - 244, t, C_TEXT, C_CARD);
    }
    draw_text(D_BODY, ox + 490, oy + top - 244, S.si_status, C_SUB, C_BG);
    draw_rect(ox + 40, oy + top - 468, ox + 1000, oy + top - 466, C_CARD);
    draw_text(D_BOLD, ox + 40, oy + top - 520, "Or a server on your network", C_TEXT, C_BG);
    draw_text(D_BODY, ox + 40, oy + top - 616, "Address", C_SUB, C_BG);
    draw_text(D_BODY, ox + 40, oy + top - 696, "Token", C_SUB, C_BG);
    for (int i = 0; i < 4; i++) {
        int x0 = ox + si_box[i].x0, y0 = oy + si_box[i].y0, x1 = ox + si_box[i].x1, y1 = oy + si_box[i].y1;
        if (si_box[i].id == S_ADDR || si_box[i].id == S_TOK) {
            int f = si_box[i].id == S_TOK, tx = x0 + 20;
            if (f) {                /* the token shown as dots */
                size_t n = strlen(S.si_tok);
                if (n > 60) n = 60;
                memset(t, 0xB7, n);
                t[n] = 0;
            } else {
                snprintf(t, sizeof(t), "%s", S.si_addr);
            }
            if (S.field == f)       /* the caret's field: a blue edge */
                draw_round(x0 - 4, y0 - 4, x1 + 4, y1 + 4, 16, C_ACCENT, C_BG);
            draw_round(x0, y0, x1, y1, 12, C_CARD, S.field == f ? C_ACCENT : C_BG);
            draw_text(D_BODY, tx, y0 + 22, t, C_TEXT, C_CARD);
            if (S.field == f) {     /* the caret */
                int cx = tx + draw_width(D_BODY, t) + 2;
                draw_rect(cx, y0 + 14, cx + 4, y1 - 14, C_ACCENT);
            }
        } else {
            unsigned bg = si_box[i].id == S_USE ? C_ACCENT : C_CARD;
            draw_round(x0, y0, x1, y1, BTN / 2, bg, C_BG);
            draw_text(D_BOLD, x0 + 32, y0 + 22, si_box[i].id == S_USE ? "Use these" : "New code", C_TEXT, bg);
        }
    }
}

static void si_redraw_fields(void)
{
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -HEADER_H - 740, S.scr_w, -HEADER_H - 540);
}

static void pin_new(void)
{
    char code[16];
    long id;
    hourglass(1);
    if (plex_pin_create(&S.px, &id, code, sizeof(code)) != 0) {
        hourglass(0);
        S.pin_id = 0;
        snprintf(S.code, sizeof(S.code), "-");
        si_set_status("Can't reach plex.tv: %s", S.px.err);
        return;
    }
    hourglass(0);
    S.pin_id = id;
    S.pin_next = now_cs() + PIN_EVERY;
    snprintf(S.code, sizeof(S.code), "%s", code);
    si_set_status("Waiting for the code...");
}

/* The window, showing the sign-in page */
static void signin_open(void)
{
    int st[9];
    S.page = PG_SIGNIN;
    S.hover = -1;
    S.posters_wanted = 0;
    if (!S.browser_open)
        browser_open();
    set_where();
    set_extent();
    set_status("Sign in to see your films and programmes.");
    window_state(S.browser_w, st);
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    set_caret(S.browser_w, -1, NULL);
    if (!S.pin_id)
        pin_new();
}

static void signin_close(void)
{
    S.pin_id = 0;                   /* no more checks */
}

/* A key on the sign-in page: 1 if it was used */
static int signin_key(int k)
{
    char *f = S.field ? S.si_tok : S.si_addr;
    size_t size = S.field ? sizeof(S.si_tok) : sizeof(S.si_addr), n = strlen(f);
    if ((k >= 32 && k < 127) || (k >= 160 && k < 256)) {
        if (n + 1 < size) {
            f[n] = (char)k;
            f[n + 1] = 0;
        }
    } else if (k == 8 || k == 0x7F) {
        if (n)
            f[n - 1] = 0;
    } else if (k == 21) {           /* Ctrl-U: empty it */
        f[0] = 0;
    } else if (k == 9 || k == 0x18E || k == 0x18F) {
        S.field = !S.field;
    } else if (k == 13) {
        if (!S.field)
            S.field = 1;
        else
            return 2;               /* Use these */
    } else {
        return 0;
    }
    si_redraw_fields();
    return 1;
}

static void pin_check(void)
{
    int e = plex_pin_check(&S.px, S.pin_id);
    S.pin_next = now_cs() + PIN_EVERY;
    if (e == 0)
        return;
    if (e < 0) {
        S.pin_id = 0;
        si_set_status("The code has run out: click New code for another.");
        return;
    }
    S.pin_id = 0;
    S.nservers = 0;
    choices_save();
    si_set_status("Signed in.");
    signin_close();
    choose_server();
}

static void use_manual(void)
{
    plex_ctx c = S.px;
    char *a = S.si_addr;
    a[strcspn(a, "\r\n")] = 0;
    S.si_tok[strcspn(S.si_tok, "\r\n")] = 0;
    if (!*a) {
        si_set_status("Type the server's address first (such as 192.168.1.10).");
        S.field = 0;
        si_redraw_fields();
        return;
    }
    si_set_status("Trying %s...", a);
    hourglass(1);
    if (plex_use_address(&c, a, S.si_tok) != 0) {
        hourglass(0);
        si_set_status("%s", c.err);
        return;
    }
    hourglass(0);
    S.px = c;
    choices_save();
    signin_close();
    browser_top();
}

/* ---- servers -------------------------------------------------------------------- */

static int servers_get(void)
{
    int n;
    hourglass(1);
    n = plex_servers(&S.px, S.servers, SERVERS_MAX);
    hourglass(0);
    if (n < 0) {
        report("Can't get your servers: %s", S.px.err);
        S.nservers = 0;
        return -1;
    }
    S.nservers = n;
    return n;
}

static int use_server(int i)
{
    plex_ctx c = S.px;
    set_status("Trying %s...", S.servers[i].name);
    hourglass(1);
    if (plex_use_server(&c, &S.servers[i]) != 0) {
        hourglass(0);
        report("Can't reach %s: %s", S.servers[i].name, c.err);
        return -1;
    }
    hourglass(0);
    S.px = c;
    choices_save();
    return 0;
}

/* After signing in: the server (the one you own first), then the browser */
static void choose_server(void)
{
    int best = 0;
    if (servers_get() <= 0) {
        if (S.nservers == 0 && !*S.px.err)
            report("Your Plex account has no servers.");
        return;
    }
    for (int i = 0; i < S.nservers; i++)
        if (S.servers[i].owned) {
            best = i;
            break;
        }
    if (use_server(best) == 0)
        browser_top();
}

static void builtin_stop(int leave);

static void sign_out(void)
{
    plex_ctx *c = &S.px;
    builtin_stop(0);
    c->account_token[0] = c->base[0] = c->token[0] = c->server_name[0] = c->server_id[0] = 0;
    S.nservers = 0;
    if (S.browser_open)
        close_window(S.browser_w);
    S.browser_open = 0;
    if (S.have_list)
        plex_list_free(&S.list);
    S.have_list = 0;
    S.nhist = 0;
    S.page = PG_GRID;
    if (S.have_det)
        plex_list_free(&S.det);
    S.have_det = 0;
    cache_free_all();
    choices_save();
}

/* Select on the icon bar icon */
static void bar_select(void)
{
    if (*S.px.base) {
        if (S.have_list)
            browser_open();
        else
            browser_top();
    } else if (*S.px.account_token) {
        choose_server();
    } else {
        signin_open();
    }
}

/* ---- handing a video to Reel ------------------------------------------------------ */

/* Wimp_StartTask with our VFP context switched off meanwhile (UnixLib keeps
   it on our stack; the new task's UnixLib makes its own and VFPSupport
   would otherwise save ours lazily into the wrong place), as !FFmpeg's
   front end does. One asm block, so no VFP instruction comes between. */
static _kernel_oserror *start_task(const char *cmd)
{
#if defined(__arm__) && defined(__riscos__)
    _kernel_oserror *err;
    __asm__ volatile(
        "mov    r0, #0\n\t"
        "mov    r1, #0\n\t"
        "swi    0x78EC3\n\t"          /* XVFPSupport_ChangeContext 0: off */
        "movvs  r0, #0\n\t"
        "mov    r4, r0\n\t"
        "mov    r0, %1\n\t"
        "swi    0x600DE\n\t"          /* XWimp_StartTask */
        "movvs  %0, r0\n\t"
        "movvc  %0, #0\n\t"
        "movs   r0, r4\n\t"
        "mov    r1, #0\n\t"
        "swine  0x78EC3\n\t"          /* back on */
        : "=&r"(err) : "r"(cmd) : "r0", "r1", "r2", "r3", "r4", "r14", "cc", "memory");
    return err;
#else
    _kernel_swi_regs r;
    r.r[0] = (intptr_t)cmd;
    return swi(Wimp_StartTask, &r);
#endif
}

/* The task handle of a running task called name, or 0 */
static int find_task(const char *name)
{
    int buf[16 * 4];
    _kernel_swi_regs r;
    size_t n = strlen(name);
    r.r[0] = 0;
    do {
        r.r[1] = (intptr_t)buf;
        r.r[2] = sizeof(buf);
        if (swi(TaskManager_EnumerateTasks, &r))
            return 0;
        for (int *p = buf; (char *)p < (char *)(intptr_t)r.r[1]; p += 4) {
            const char *t = (const char *)(intptr_t)p[1];
            /* the name exactly: "Reel" mustn't find "ReelEGL" */
            if (p[0] != S.task && !strncmp(t, name, n) && (unsigned char)t[n] < 32)
                return p[0];
        }
    } while (r.r[0] >= 0);
    return 0;
}

static void start_player(int player, const char *file)
{
    char var[32], cmd[400];
    const char *dir;
    _kernel_oserror *e;
    snprintf(var, sizeof(var), "%s$Dir", player_names[player]);
    dir = getenv(var);
    if (!dir || !*dir) {
        report("%s hasn't been seen by the Filer yet: open the directory that holds !%s, "
               "then try again (or choose the other player on the Player menu).",
               player_names[player], player_names[player]);
        remove(file);
        return;
    }
    snprintf(cmd, sizeof(cmd), "Run <%s>." "!Run %s", var, file);
    if ((e = start_task(cmd)) != NULL)
        report("Can't start %s: %s", player_names[player], e->errmess);
    else
        set_status("%s (started %s)", S.play_why, player_names[player]);
    /* the player reads the file before its first Wimp_Poll, and StartTask
       returns at that poll */
    remove(file);
}

/* The hand-off file's name: a few in turn, so one still waiting for its
   DataLoadAck isn't overwritten */
static int handoff_path(char *out, size_t size)
{
    const char *scrap = getenv("Wimp$ScrapDir");
    char dir[256];
    if (!scrap || !*scrap)
        return -1;
    snprintf(dir, sizeof(dir), "%s" SEP APP, scrap);
    make_dir(dir);
    snprintf(out, size, "%s" SEP "Play%d", dir, S.play_n);
    S.play_n = (S.play_n + 1) % 8;
    return 0;
}

static void send_to_player(const char *file, int type)
{
    int task = find_task(player_names[S.player]);
    int b[64];
    _kernel_swi_regs r;
    if (task && strlen(file) < 212) {
        memset(b, 0, sizeof(b));
        b[3] = 0;
        b[4] = MSG_DATAOPEN;
        b[5] = 0;                   /* no directory window */
        b[10] = type;
        snprintf((char *)&b[11], 256 - 44, "%s", file);
        b[0] = (44 + (int)strlen(file) + 1 + 3) & ~3;
        r.r[0] = 18;                /* recorded: it comes back if nobody claims it */
        r.r[1] = (intptr_t)b;
        r.r[2] = task;              /* to that task, not to everyone (KinoAmp would take it) */
        if (!swi(Wimp_SendMessage, &r)) {
            for (int i = 0; i < PEND_MAX; i++)
                if (!S.pend[i].ref || i == PEND_MAX - 1) {
                    if (S.pend[i].ref)
                        remove(S.pend[i].path);
                    S.pend[i].ref = b[2];
                    S.pend[i].player = S.player;
                    snprintf(S.pend[i].path, sizeof(S.pend[i].path), "%s", file);
                    break;
                }
            return;
        }
    }
    start_player(S.player, file);
}

enum { PLAY_DEFAULT, PLAY_RESUME, PLAY_START };

/* it: the video as the list shows it (its title); what's played and how
   comes from its full details (the subtitles chosen, where it was left) */
static void builtin_play(const plex_item *it, int how);

static void play_item(const plex_item *it, int how)
{
    static play_t p;
    caps_t k;
    char file[300], title[300];
    int allow = S.direct, resume = 1, type, e;
    plex_list tmp;
    const plex_item *full = it;
    if (S.player == PLAYER_BUILTIN) {
        builtin_play(it, how);
        return;
    }
    tmp.n = 0;
    if (det_is(it)) {
        full = det_item();
    } else {
        hourglass(1);
        if (plex_details(&S.px, it, &tmp) == 0)
            full = &tmp.v[0];
        hourglass(0);
    }
    /* Resume from: the server starts its stream there (copying the video
       when it can); Reel can't be told where to start a file of its own */
    if (how == PLAY_RESUME)
        allow = 0;
    if (how == PLAY_START)
        resume = 0;
    caps_for(S.quality, &k);
    e = caps_play(&S.px, full, &k, allow, resume, &p);
    if (tmp.n)
        plex_list_free(&tmp);
    if (e != 0) {
        set_status("%s", p.why);
        return;
    }
    if (how == PLAY_START)
        p.key[0] = 0;               /* not where Reel last stopped, either */
    if (it->subtitle && *it->subtitle && it->type && !strcmp(it->type, "episode"))
        snprintf(title, sizeof(title), "%s (%s)", it->title, it->subtitle);
    else
        snprintf(title, sizeof(title), "%s", it->title);
    if (handoff_path(file, sizeof(file)) != 0) {
        report("<Wimp$ScrapDir> isn't set: is !Scrap missing?");
        return;
    }
    if (handoff_write(file, &p, title, S.agent) != 0) {
        report("Can't write %s", file);
        return;
    }
    /* typed as a video, so Reel claims the DataOpen; Reel looks inside and
       finds JSON */
    type = mime_type(2, "video/mp4", 0xBF4);
    set_type(file, type);
    snprintf(S.play_why, sizeof(S.play_why), "%s", p.why);
    set_status("%s", p.why);
    send_to_player(file, type);
}


/* ---- the built-in player ----------------------------------------------------------

   The video plays in the window itself (the player page), in player.c.
   Here is what Plex needs from it: the server told where playing has got
   to (Continue watching, Resume) every 10 s and when it pauses or stops; a
   converted stream started again at the new place to seek in it; the
   sound track chosen on the server for a converted stream; and the next
   episode offered at the end of one. */

static void player_menu_open(void);

#define TIMELINE_CS 1000
#define UPNEXT_CS   1000

static void builtin_timeline(const char *state)
{
    const plex_item *it;
    if (!S.pl.on || !S.pl.det.n)
        return;
    it = &S.pl.det.v[0];
    plex_timeline(&S.px, it, state, (int64_t)(player_position() * 1000), (int64_t)(player_duration() * 1000),
                  &S.pl.pq);
    S.pl.tl_cs = now_cs() + TIMELINE_CS;
}

/* The X-Plex headers, without Accept: JSON (the video isn't) */
static void media_headers(const char *in, char *out, size_t size)
{
    size_t o = 0;
    while (*in && o + 1 < size) {
        const char *e = strstr(in, "\r\n");
        size_t n = e ? (size_t)(e - in) + 2 : strlen(in);
        if (strncmp(in, "Accept:", 7) && o + n < size) {
            memcpy(out + o, in, n);
            o += n;
        }
        in += n;
    }
    out[o] = 0;
}

/* The title shown in the bar: "Show · S1 E3 · Episode" or the film's */
static void builtin_title(const plex_item *it, char *out, size_t size)
{
    char t[400];
    if (it->type && !strcmp(it->type, "episode") && it->grandparent_title)
        snprintf(t, sizeof(t), "%s \xc2\xb7 S%d E%d \xc2\xb7 %s", it->grandparent_title, it->parent_index, it->index,
                 it->title);
    else
        snprintf(t, sizeof(t), "%s", it->title);
    latin1(t, out, size);
}

/* Plays S.pl.det's video from t seconds (a converted stream starts there) */
static int builtin_open_at(double t)
{
    const plex_item *it = &S.pl.det.v[0];
    caps_t k;
    player_src src;
    char title[200], headers[1400], old[32];
    snprintf(old, sizeof(old), "%s", S.pl.p.direct ? "" : S.pl.p.session);
    caps_for(S.quality, &k);
    if (caps_play_at(&S.px, it, &k, S.direct, (long)t, &S.pl.p) != 0) {
        set_status("%s", S.pl.p.why);
        return -1;
    }
    if (*old)
        plex_transcode_stop(&S.px, old);    /* the stream before (a seek) */
    media_headers(S.pl.p.headers, headers, sizeof(headers));
    /* the stream and the timeline under one session id: how the server
       ties them together (the dashboard's Now Playing) */
    snprintf(S.pl.pq.session, sizeof(S.pl.pq.session), "%s", S.pl.p.session);
    if (strlen(headers) + strlen(S.pl.p.session) + 40 < sizeof(headers))
        snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers), "X-Plex-Session-Identifier: %s\r\n",
                 S.pl.p.session);
    builtin_title(it, title, sizeof(title));
    memset(&src, 0, sizeof(src));
    src.url = S.pl.p.url;
    src.headers = headers;
    src.user_agent = S.agent;
    src.title = title;
    src.convert = !S.pl.p.direct;
    src.base = S.pl.p.direct ? 0 : S.pl.p.offset_s;
    src.start = S.pl.p.direct ? t : 0;
    src.duration = it->duration_ms / 1000.0;
    snprintf(S.play_why, sizeof(S.play_why), "%s", S.pl.p.why);
    set_status("%s", S.pl.p.why);
    if (player_open(&src, S.browser_w) != 0) {
        report("Can't play %s: %s", title, player_error());
        return -1;
    }
    S.pl.tl_cs = now_cs() + TIMELINE_CS;
    return 0;
}

/* Back to the page it was played from */
static void builtin_leave(void)
{
    int st[9];
    if (S.page != PG_PLAYER)
        return;
    S.page = S.pl.prev_page;
    set_extent();
    set_where();
    if (S.browser_open) {
        window_state(S.browser_w, st);
        if (S.pl.prev_st[0])        /* the size it was before the video */
            memcpy(st, S.pl.prev_st, sizeof(st));
        open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, S.page == PG_GRID ? S.grid_sy : 0);
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    }
    if (S.page == PG_DETAILS)
        det_refresh();              /* where it stopped: Resume from */
    S.posters_wanted = S.page == PG_GRID;
}

/* Stops playing: the server is told, a converted stream stopped */
static void builtin_stop(int leave)
{
    if (!S.pl.on)
        return;
    if (player_ready() && !player_ended())
        builtin_timeline("stopped");
    if (!S.pl.p.direct)
        plex_transcode_stop(&S.px, S.pl.p.session);
    player_close();
    S.pl.on = 0;
    S.pl.upnext_cs = 0;
    if (S.pl.det.n)
        plex_list_free(&S.pl.det);
    if (S.pl.next.n)
        plex_list_free(&S.pl.next);
    if (leave)
        builtin_leave();
}

/* it: the video (a list's item); what's played comes from its details */
static void builtin_play(const plex_item *it, int how)
{
    plex_list d;
    int st[9], prev = S.page == PG_PLAYER ? S.pl.prev_page : S.page, was_player = S.page == PG_PLAYER;
    double t;
    hourglass(1);
    if (plex_details(&S.px, it, &d) != 0) {
        hourglass(0);
        report("Can't get %s from the server: %s", it->title, S.px.err);
        return;
    }
    builtin_stop(0);
    S.pl.det = d;
    S.pl.on = 1;
    /* a play queue of one, as the Plex apps play from (without one it still plays) */
    plex_play_queue(&S.px, &d.v[0], &S.pl.pq);
    hourglass(0);
    S.pl.prev_page = prev == PG_PLAYER ? PG_GRID : prev;
    if (!S.browser_open)
        browser_open();
    window_state(S.browser_w, st);
    if (S.page == PG_GRID)
        S.grid_sy = st[6];
    S.page = PG_PLAYER;
    S.hover = -1;
    S.posters_wanted = 0;
    set_extent();
    set_where();
    if (!was_player)
        S.pl.prev_st[0] = 0;
    if (st[3] - st[1] < 1280 && S.scr_w >= 1400 && !was_player) {
        /* a narrow window (two columns of posters): wider for the video,
           16:9 and the bar, the size it was kept for later */
        int w = S.scr_w * 3 / 4 < 1600 ? S.scr_w * 3 / 4 : 1600, h = w * 9 / 16 + 168;
        memcpy(S.pl.prev_st, st, sizeof(st));
        if (h > S.scr_h - 160)
            h = S.scr_h - 160;
        st[1] = st[1] + w > S.scr_w ? S.scr_w - w : st[1];
        st[2] = st[4] - h < 64 ? 64 : st[4] - h;
        st[3] = st[1] + w;
        st[4] = st[2] + h;
    }
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
    set_caret(S.browser_w, -1, NULL);
    t = how != PLAY_START && d.v[0].view_offset_ms > 0 ? d.v[0].view_offset_ms / 1000.0 : 0;
    if (builtin_open_at(t) != 0)
        builtin_stop(1);
}

/* The card at the end of an episode: the next one, in so many seconds */
static void upnext_card(void)
{
    const plex_item *n = &S.pl.next.v[0];
    char head[200], line[300], line2[60], t[400];
    int s = (S.pl.upnext_cs - now_cs() + 99) / 100;
    snprintf(t, sizeof(t), "Up next \xc2\xb7 %s", n->grandparent_title ? n->grandparent_title : "");
    latin1(t, head, sizeof(head));
    snprintf(t, sizeof(t), "S%d E%d \xc2\xb7 %s", n->parent_index, n->index, n->title);
    latin1(t, line, sizeof(line));
    snprintf(line2, sizeof(line2), "Playing in %d s", s < 0 ? 0 : s);
    player_card(head, line, line2, "Play now", "Back");
}

static void upnext_play(void)
{
    plex_item *n;
    int prev = S.pl.prev_page;
    if (!S.pl.next.n)
        return;
    n = &S.pl.next.v[0];
    player_card(NULL, NULL, NULL, NULL, NULL);
    S.pl.upnext_cs = 0;
    {
        plex_list keep = S.pl.next;         /* builtin_play frees it (builtin_stop) */
        memset(&S.pl.next, 0, sizeof(S.pl.next));
        S.page = PG_PLAYER;
        builtin_play(n, PLAY_START);
        S.pl.prev_page = prev;
        plex_list_free(&keep);
    }
}

/* The end: watched; the next episode offered, or back */
static void builtin_ended(void)
{
    const plex_item *it = &S.pl.det.v[0];
    builtin_timeline("stopped");
    plex_mark(&S.px, it, 1);
    if (S.pl.next.n)
        plex_list_free(&S.pl.next);
    if (plex_next_episode(&S.px, it, &S.pl.next) == 0 && S.pl.next.n) {
        S.pl.upnext_cs = now_cs() + UPNEXT_CS;
        upnext_card();
        return;
    }
    builtin_stop(1);
}

/* What the player said */
static void builtin_event(int e)
{
    switch (e) {
    case PE_READY:
        builtin_timeline("playing");
        break;
    case PE_PAUSED:
        builtin_timeline("paused");
        break;
    case PE_PLAYING:
        builtin_timeline("playing");
        break;
    case PE_SEEK:                   /* a converted stream: another from there */
        if (builtin_open_at(player_seek_to()) != 0)
            builtin_stop(1);
        break;
    case PE_END:
        builtin_ended();
        break;
    case PE_FAILED:
        report("%s", player_error());
        builtin_stop(1);
        break;
    case PE_BACK:
        builtin_stop(1);
        break;
    case PE_CARD_1:
        if (S.pl.upnext_cs)
            upnext_play();
        break;
    case PE_CARD_2:
        builtin_stop(1);
        break;
    case PE_MENU:
        player_menu_open();
        break;
    }
}

static void builtin_nulls(void)
{
    int e = player_null();
    if (e != PE_NONE) {
        builtin_event(e);
        return;
    }
    if (!S.pl.on)
        return;
    if (S.pl.upnext_cs) {
        if (now_cs() - S.pl.upnext_cs >= 0)
            upnext_play();
        else {
            static int last;
            if (now_cs() - last >= 50) {
                last = now_cs();
                upnext_card();
            }
        }
        return;
    }
    if (player_ready() && !player_paused() && !player_ended() && now_cs() - S.pl.tl_cs >= 0)
        builtin_timeline("playing");
}

/* ---- saving the original file ---------------------------------------------------- */

static void save_prepare(const plex_item *it)
{
    const char *leaf, *dot;
    char ext[16] = "";
    size_t n = 0;
    S.sv_ok = it && it->kind == PI_VIDEO && it->part_key && *it->part_key;
    if (!S.sv_ok)
        return;
    leaf = it->part_file ? strrchr(it->part_file, '/') : NULL;
    leaf = leaf ? leaf + 1 : it->part_file ? it->part_file : "Video";
    if ((dot = strrchr(leaf, '.')) != NULL)
        snprintf(ext, sizeof(ext), "%s", dot + 1);
    {
        char l1[300];
        latin1(leaf, l1, sizeof(l1));
        for (const char *s = l1; *s && n < sizeof(S.sv_leaf) - 1; s++) {
            char c = *s;
            if (c == '.')
                c = '/';            /* "film.mkv" is "film/mkv" on RISC OS */
            else if (c == ' ')
                c = (char)0xA0;     /* hard space */
            else if (strchr(":*#$&@^%\\|\"<>", c))
                c = '_';
            S.sv_leaf[n++] = c;
        }
        S.sv_leaf[n] = 0;
    }
    snprintf(S.sv_name, sizeof(S.sv_name), "%s", S.sv_leaf);
    snprintf(S.sv_url_key, sizeof(S.sv_url_key), "%s", it->part_key);
    latin1(it->title, S.sv_title, sizeof(S.sv_title));
    S.sv_size = it->part_size;
    S.sv_type = *ext ? mime_type(3, ext, 0xFFD) : 0xFFD;
    snprintf(sv_sprite, sizeof(sv_sprite), "file_%03x", S.sv_type);
}

static int too_big(int64_t size, const char *title)
{
    if (size <= FILE_MAX)
        return 0;
    report("%s is %.1fGB. RISC OS's filing systems hold files of up to 4GB, so it can't be saved.",
           title, size / 1e9);
    return 1;
}

static void save_stop(const char *why)
{
    if (!S.save.active)
        return;
    net_close(S.save.ns);
    if (S.save.f)
        fclose(S.save.f);
    remove(S.save.path);            /* no half files left behind */
    S.save.active = 0;
    S.save.ns = NULL;
    S.save.f = NULL;
    if (why)
        set_status("%s", why);
}

static void save_start(const char *path)
{
    char url[600], headers[1024], err[256];
    int64_t size;
    if (S.save.active) {
        report("Already saving %s: stop that first (on the menu).", S.save.title);
        return;
    }
    if (S.speed.active) {
        report("Wait until the speed test has finished.");
        return;
    }
    if (too_big(S.sv_size, S.sv_title))
        return;
    snprintf(url, sizeof(url), "%s%s", S.px.base, S.sv_url_key);
    plex_headers(&S.px, S.px.token, headers, sizeof(headers));
    hourglass(1);
    S.save.ns = net_open(url, headers, 30000, err, sizeof(err));
    hourglass(0);
    if (!S.save.ns) {
        report("Can't get %s: %s", S.sv_title, err);
        return;
    }
    size = net_size(S.save.ns);
    if (too_big(size, S.sv_title)) {
        net_close(S.save.ns);
        S.save.ns = NULL;
        return;
    }
    if (!(S.save.f = fopen(path, "wb"))) {
        report("Can't write %s: %s", path, strerror(errno));
        net_close(S.save.ns);
        S.save.ns = NULL;
        return;
    }
    S.save.active = 1;
    S.save.size = size > 0 ? size : S.sv_size;
    S.save.done = 0;
    S.save.t0 = now_cs();
    S.save.type = S.sv_type;
    snprintf(S.save.path, sizeof(S.save.path), "%s", path);
    snprintf(S.save.title, sizeof(S.save.title), "%s", S.sv_title);
    set_status("Saving %s...", S.save.title);
}

static void save_fail(const char *fmt, const char *arg)
{
    char why[400];
    snprintf(why, sizeof(why), fmt, S.save.title, arg ? arg : "");
    save_stop(NULL);
    report("%s", why);
}

/* A piece of the file (SAVE_STEP bytes): one null event's work */
static void save_step(void)
{
    static char buf[64 * 1024];
    int total = 0, end = 0;
    while (total < SAVE_STEP) {
        int want = (int)sizeof(buf), got;
        if (S.save.size > 0 && S.save.size - S.save.done < want)
            want = (int)(S.save.size - S.save.done);
        if (want <= 0) {            /* all of it */
            end = 1;
            break;
        }
        got = net_read(S.save.ns, buf, want);
        if (got < 0) {
            save_fail("Saving %s stopped: the server stopped sending.%s", NULL);
            return;
        }
        if (got == 0) {
            end = 1;
            break;
        }
        if (fwrite(buf, 1, (size_t)got, S.save.f) != (size_t)got) {
            save_fail("Can't write %s: %s", strerror(errno));
            return;
        }
        S.save.done += got;
        total += got;
    }
    if (S.save.size > 0 && S.save.done >= S.save.size)
        end = 1;
    if (!end) {
        char rate[40] = "";
        int cs = now_cs() - S.save.t0;
        if (cs >= 100)              /* how fast, once there's a second to go on */
            snprintf(rate, sizeof(rate), ", %.1f MB/s", S.save.done / 1048576.0 * 100 / cs);
        if (S.save.size > 0)
            set_status("Saving %s: %.0f of %.0f MB (%d%%)%s", S.save.title, S.save.done / 1048576.0,
                       S.save.size / 1048576.0, (int)(S.save.done * 100 / S.save.size), rate);
        else
            set_status("Saving %s: %.0f MB%s", S.save.title, S.save.done / 1048576.0, rate);
        return;
    }
    if (S.save.size > 0 && S.save.done != S.save.size) {
        save_fail("Saving %s stopped: the file ended early.%s", NULL);
        return;
    }
    net_close(S.save.ns);
    S.save.ns = NULL;
    if (fclose(S.save.f) != 0) {
        S.save.f = NULL;
        save_fail("Couldn't finish writing %s: is the disc full?%s", NULL);
        return;
    }
    S.save.f = NULL;
    set_type(S.save.path, S.save.type);
    S.save.active = 0;
    set_status("Saved %s (%.0f MB).", S.save.title, S.save.done / 1048576.0);
}

/* ---- the speed test ------------------------------------------------------------ */

/* Reads the start of a video's file from the server for up to SPEED_TIME,
   a piece each null event (as saving does), and says how fast it came and
   whether that's enough to play the file directly. It tells a slow server
   or network from a player that doesn't read fast enough. */
#define SPEED_TIME 800              /* centiseconds */
#define SPEED_MAX (64 * 1024 * 1024)

static void speed_close(void)
{
    net_close(S.speed.ns);
    S.speed.ns = NULL;
    S.speed.active = 0;
}

static void speed_start(const plex_item *it)
{
    char url[600], headers[1024], err[256];
    int t;
    if (S.save.active || S.speed.active) {
        report("Wait until the file being saved, or the speed test, has finished.");
        return;
    }
    if (!it || it->kind != PI_VIDEO || !it->part_key || !*it->part_key)
        return;
    snprintf(url, sizeof(url), "%s%s", S.px.base, it->part_key);
    plex_headers(&S.px, S.px.token, headers, sizeof(headers));
    latin1(it->title, S.speed.title, sizeof(S.speed.title));
    hourglass(1);
    t = now_cs();
    S.speed.ns = net_open(url, headers, 30000, err, sizeof(err));
    S.speed.start_cs = now_cs() - t;
    hourglass(0);
    if (!S.speed.ns) {
        report("Speed test: can't get %s from the server: %s", S.speed.title, err);
        return;
    }
    S.speed.active = 1;
    S.speed.done = 0;
    S.speed.kbps = it->bitrate_kbps;
    S.speed.t0 = now_cs();
    set_status("Testing the speed: reading %s from the server...", S.speed.title);
}

static void speed_done(const char *broke)
{
    int cs = now_cs() - S.speed.t0;
    double mbit = S.speed.done * 8.0 / (cs > 0 ? cs : 1) * 100 / 1e6;
    int https = !strncmp(S.px.base, "https:", 6);
    char how[160], verdict[120] = "";
    speed_close();
    /* all of it must fit in an error block (252 characters) */
    snprintf(how, sizeof(how), "%.1f MB in %.1f s: %.1f Mbit/s, over %s (%s; answered in %.1f s).",
             S.speed.done / 1048576.0, cs / 100.0, mbit, https ? "https" : "http",
             S.px.local ? "local" : "not local", S.speed.start_cs / 100.0);
    if (S.speed.kbps > 0) {
        double need = S.speed.kbps / 1000.0, times = mbit / need;
        if (times >= 2)
            snprintf(verdict, sizeof(verdict), " It needs %.1f Mbit/s: plenty. If the player runs short, it isn't reading "
                     "fast enough.", need);
        else if (times >= 1.2)
            snprintf(verdict, sizeof(verdict), " It needs %.1f Mbit/s: just enough to play directly.", need);
        else
            snprintf(verdict, sizeof(verdict), " It needs %.1f Mbit/s: too slow to play directly. Untick Direct play, "
                     "or save the file first.", need);
    }
    set_status("Speed test: %.1f Mbit/s from the server (%s)", mbit, https ? "https" : "http");
    report("Speed test, %.40s: %s%s%s", S.speed.title, how, verdict, broke ? " It stopped part way." : "");
}

/* A piece of the file (SAVE_STEP bytes): one null event's work */
static void speed_step(void)
{
    static char buf[64 * 1024];
    int total = 0;
    while (total < SAVE_STEP) {
        int got = net_read(S.speed.ns, buf, sizeof(buf));
        if (got <= 0) {
            speed_done(got < 0 ? "broke" : NULL);
            return;
        }
        S.speed.done += got;
        total += got;
    }
    if (now_cs() - S.speed.t0 >= SPEED_TIME || S.speed.done >= SPEED_MAX) {
        speed_done(NULL);
        return;
    }
    set_status("Testing the speed: %.1f MB so far...", S.speed.done / 1048576.0);
}

static void close_menus(void)
{
    _kernel_swi_regs r;
    r.r[1] = -1;
    swi(Wimp_CreateMenu, &r);
    S.menu_kind = 0;
}

/* OK in the save box (or Return): a full path typed in saves there */
static void save_ok(void)
{
    S.sv_name[strcspn(S.sv_name, "\r\n")] = 0;
    if (!strchr(S.sv_name, '.') && !strchr(S.sv_name, ':')) {
        report("To save, drag the icon to a directory display.");
        return;
    }
    close_menus();
    save_start(S.sv_name);
}

static void drag_start(void)
{
    int st[9], box[10];
    _kernel_swi_regs r;
    window_state(S.save_w, st);
    /* the file icon's box on the screen */
    box[0] = st[1] - st[5] + 136; box[1] = st[4] - st[6] - 124;
    box[2] = st[1] - st[5] + 264; box[3] = st[4] - st[6] - 20;
    r.r[0] = 0x85;                  /* centred, bounded by the screen, a shadow */
    r.r[1] = 1;
    r.r[2] = (intptr_t)sv_sprite;
    r.r[3] = (intptr_t)box;
    r.r[4] = 0;
    S.dragging = 1;
    S.drag_sprite = !swi(DragASprite_Start, &r);
    if (!S.drag_sprite) {
        int b[10];
        b[0] = S.save_w; b[1] = 5;  /* a fixed-size box */
        b[2] = box[0]; b[3] = box[1]; b[4] = box[2]; b[5] = box[3];
        b[6] = -0x10000; b[7] = -0x10000; b[8] = 0x10000; b[9] = 0x10000;
        r.r[1] = (intptr_t)b;
        swi(Wimp_DragBox, &r);
    }
}

/* The icon was dropped: offer the file there (Message_DataSave) */
static void drag_end(void)
{
    int p[5], b[64];
    _kernel_swi_regs r;
    const char *leaf;
    if (!S.dragging)
        return;
    S.dragging = 0;
    if (S.drag_sprite)
        swi(DragASprite_Stop, &r);
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r) || p[3] == S.save_w || p[3] < 0)
        return;
    leaf = strrchr(S.sv_name, '.');
    leaf = leaf ? leaf + 1 : S.sv_name;
    memset(b, 0, sizeof(b));
    b[4] = MSG_DATASAVE;
    b[5] = p[3]; b[6] = p[4]; b[7] = p[0]; b[8] = p[1];
    b[9] = S.sv_size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)S.sv_size;
    b[10] = S.sv_type;
    snprintf((char *)&b[11], 212, "%s", *leaf ? leaf : "Video");
    b[0] = (44 + (int)strlen((char *)&b[11]) + 1 + 3) & ~3;
    r.r[0] = 17;
    r.r[1] = (intptr_t)b;
    r.r[2] = p[3];
    r.r[3] = p[4];
    if (!swi(Wimp_SendMessage, &r))
        S.datasave_ref = b[2];
}

/* ---- menus ------------------------------------------------------------------------ */

typedef struct { int flags, sub; unsigned iflags; int data[3]; } mitem_t;
typedef struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    mitem_t item[16];
} wmenu_t;
typedef struct { wmenu_t m; char text[16][80]; int n; } menu_t;

static menu_t m_bar, m_servers, m_player, m_quality, m_size, m_item, m_subs;
static menu_t m_play, m_audio, m_vol, m_pic;

static void menu_begin(menu_t *m, const char *title)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->m.title, sizeof(m->m.title), "%.11s", title);
    m->m.tfg = 7; m->m.tbg = 2; m->m.wfg = 7; m->m.wbg = 0;
    m->m.height = 44;
    m->m.width = (int)strlen(m->m.title) * 16 + 16;
}

static void menu_add(menu_t *m, const char *text, int tick, int shade, int sub, int dots)
{
    mitem_t *i = &m->m.item[m->n];
    int w;
    if (m->n >= 16)
        return;
    latin1(text, m->text[m->n], sizeof(m->text[0]));
    i->flags = (tick ? 1 : 0) | (dots ? 2 : 0);
    i->sub = sub;
    i->iflags = 0x07000121u | (shade ? 0x400000u : 0);  /* text, filled, indirected */
    i->data[0] = (int)(intptr_t)m->text[m->n];
    i->data[1] = -1;
    i->data[2] = (int)sizeof(m->text[0]);
    w = (int)strlen(m->text[m->n]) * 16 + 32;
    if (w > m->m.width)
        m->m.width = w;
    m->n++;
}

static void menu_end(menu_t *m)
{
    if (m->n)
        m->m.item[m->n - 1].flags |= 0x80;          /* the last */
}

static void open_menu(menu_t *m, int x, int y)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)&m->m;
    r.r[2] = x;
    r.r[3] = y;
    swi(Wimp_CreateMenu, &r);
}

static void bar_menu_build(void)
{
    int signed_in = *S.px.account_token != 0;
    menu_begin(&m_servers, "Servers");
    for (int i = 0; i < S.nservers; i++)
        menu_add(&m_servers, S.servers[i].name, !strcmp(S.servers[i].id, S.px.server_id), 0, -1, 0);
    if (!S.nservers)
        menu_add(&m_servers, "(none found)", 0, 1, -1, 0);
    menu_end(&m_servers);
    menu_begin(&m_player, "Player");
    for (int i = 0; i < PLAYER_COUNT; i++)
        menu_add(&m_player, player_names[i], S.player == i, 0, -1, i == PLAYER_BUILTIN);
    menu_end(&m_player);
    menu_begin(&m_quality, "Quality");
    for (int i = 0; i < Q_COUNT; i++)
        menu_add(&m_quality, caps_quality_name(i), S.quality == i, 0, -1, 0);
    menu_end(&m_quality);
    menu_begin(&m_size, "Poster size");
    for (int i = 0; i < 3; i++)
        menu_add(&m_size, sizes[i].name, S.psize == i, 0, -1, 0);
    menu_end(&m_size);

    menu_begin(&m_bar, APP);
    menu_add(&m_bar, "Info", 0, 0, S.proginfo, 0);
    menu_add(&m_bar, "Sign in...", 0, 0, -1, 0);
    menu_add(&m_bar, "Servers", 0, !signed_in, signed_in ? (int)(intptr_t)&m_servers.m : -1, 0);
    menu_add(&m_bar, "Player", 0, 0, (int)(intptr_t)&m_player.m, 0);
    menu_add(&m_bar, "Quality", 0, 0, (int)(intptr_t)&m_quality.m, 0);
    menu_add(&m_bar, "Poster size", 0, 0, (int)(intptr_t)&m_size.m, 0);
    menu_add(&m_bar, "Direct play when possible", S.direct, 0, -1, 1);
    menu_add(&m_bar, "Sign out", 0, !signed_in && !*S.px.base, -1, 0);
    menu_add(&m_bar, "Quit", 0, 0, -1, 0);
    menu_end(&m_bar);
}

static void bar_menu_open(int x)
{
    if (*S.px.account_token && !S.nservers)
        servers_get();              /* for the Servers submenu */
    bar_menu_build();
    S.menu_kind = 1;
    S.menu_x = x;
    open_menu(&m_bar, x - 64, 96 + MB_COUNT * 44);
}

static const plex_item *sel_item(void)
{
    return S.have_list && S.sel >= 0 && S.sel < S.list.n ? &S.list.v[S.sel] : NULL;
}

/* The subtitles of the video in S.det: None, then its tracks */
static void subs_menu_build(void)
{
    const plex_item *it = det_item();
    char t[160];
    menu_begin(&m_subs, "Subtitles");
    menu_add(&m_subs, "None", !it || plex_sub_selected(it) < 0, 0, -1, it && it->nsubs);
    for (int i = 0; it && i < it->nsubs && i < 15; i++) {
        const plex_sub *sb = &it->subs[i];
        snprintf(t, sizeof(t), "%s%s", sb->title,
                 sb->forced && !strstr(sb->title, "orced") ? " (forced)" : "");
        menu_add(&m_subs, t, sb->selected, 0, -1, 0);
    }
    menu_end(&m_subs);
}

static void item_menu_build(void)
{
    const plex_item *it = sel_item();
    int video = it && it->kind == PI_VIDEO, folder = it && it->kind == PI_FOLDER;
    char t[80], when[32];
    save_prepare(it);
    /* a video's subtitle tracks are only in its details */
    if (video && !det_is(it))
        det_fetch(it, 0);
    if (video && det_is(it))
        subs_menu_build();
    menu_begin(&m_item, it ? S.disp[S.sel].line[0] : APP);
    menu_add(&m_item, folder ? "Open" : "Play", 0, !video && !folder, -1, 0);
    menu_add(&m_item, "Details...", 0, !video, -1, 0);
    if (video && it->view_offset_ms > 0) {
        hms(it->view_offset_ms, when, sizeof(when));
        snprintf(t, sizeof(t), "Resume from %s", when);
        menu_add(&m_item, t, 0, 0, -1, 0);
    } else {
        menu_add(&m_item, "Resume", 0, 1, -1, 0);
    }
    menu_add(&m_item, "Play from start", 0, !video, -1, 0);
    menu_add(&m_item, "Subtitles", 0, !video || !det_is(it) || !det_item()->nsubs,
             video && det_is(it) && det_item()->nsubs ? (int)(intptr_t)&m_subs.m : -1, 0);
    if (S.save.active)
        menu_add(&m_item, "Stop saving", 0, 0, -1, 0);
    else
        menu_add(&m_item, "Save original file", 0, !S.sv_ok,
                 S.sv_ok && S.sv_size <= FILE_MAX ? S.save_w : -1, 0);
    menu_add(&m_item, "Test speed", 0, !S.sv_ok || S.save.active || S.speed.active, -1, 1);
    menu_add(&m_item, "Mark watched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 0);
    menu_add(&m_item, "Mark unwatched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 1);
    menu_add(&m_item, "Back", 0, S.nhist == 0, -1, 0);
    menu_add(&m_item, "Refresh", 0, 0, -1, 0);
    menu_end(&m_item);
}

/* Menu over the built-in player */
static void player_menu_build(void)
{
    const plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    char t[120];
    int n;
    menu_begin(&m_audio, "Sound track");
    /* a converted stream has the one the server was asked for: choose on the server */
    S.pl.menu_audio = !S.pl.p.direct;
    if (S.pl.menu_audio) {
        n = it ? it->nauds : 0;
        for (int i = 0; i < n && i < 15; i++)
            menu_add(&m_audio, it->auds[i].title, i == plex_audio_selected(it), 0, -1, 0);
    } else {
        n = player_tracks();
        for (int i = 0; i < n && i < 15; i++) {
            player_track_name(i, t, sizeof(t));
            menu_add(&m_audio, t, i == player_track(), 0, -1, 0);
        }
    }
    if (!n)
        menu_add(&m_audio, "(none)", 0, 1, -1, 0);
    menu_end(&m_audio);
    menu_begin(&m_vol, "Volume");
    for (int i = 0; i < 5; i++) {
        snprintf(t, sizeof(t), "%d%%", 100 - i * 25);
        menu_add(&m_vol, t, (int)(player_volume() * 100 + 0.5) == 100 - i * 25, 0, -1, 0);
    }
    menu_end(&m_vol);
    menu_begin(&m_pic, "Picture");
    for (int i = 0; i < PIC_COUNT; i++)
        menu_add(&m_pic, pic_names[i], player_pic_mode() == i, 0, -1, 0);
    menu_end(&m_pic);
    menu_begin(&m_play, "Player");
    menu_add(&m_play, "Sound track", 0, n < 2, n >= 2 ? (int)(intptr_t)&m_audio.m : -1, 0);
    menu_add(&m_play, "Volume", 0, 0, (int)(intptr_t)&m_vol.m, 0);
    menu_add(&m_play, "Picture", 0, 0, (int)(intptr_t)&m_pic.m, 1);
    menu_add(&m_play, "Stats", player_stats(), 0, -1, 0);
    menu_add(&m_play, "Full screen", player_fullscreen(), 0, -1, 0);
    menu_add(&m_play, "Hardware overlay", player_overlay(), 0, -1, 1);
    menu_add(&m_play, "Stop", 0, 0, -1, 0);
    menu_end(&m_play);
}

static void player_menu_open(void)
{
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r))
        p[0] = S.scr_w / 2, p[1] = S.scr_h / 2;
    player_menu_build();
    S.menu_kind = 4;
    S.menu_x = p[0];
    S.menu_y = p[1];
    open_menu(&m_play, p[0] - 64, p[1]);
}

/* A sound track: reelcore's for the file itself; the server's for a converted stream */
static void choose_audio(int k)
{
    plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    if (k < 0)
        return;
    if (!S.pl.menu_audio) {
        player_set_track(k);
        return;
    }
    if (!it || k >= it->nauds || k == plex_audio_selected(it))
        return;
    hourglass(1);
    if (plex_set_audio(&S.px, it, it->auds[k].id) != 0) {
        hourglass(0);
        report("Can't choose that sound track: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; i < it->nauds; i++)
        it->auds[i].selected = i == k;
    player_note("Changing the sound track...");
    if (builtin_open_at(player_position()) != 0)
        builtin_stop(1);
}

static void item_menu_open(int x, int y)
{
    item_menu_build();
    S.menu_kind = 2;
    S.menu_x = x;
    S.menu_y = y;
    open_menu(&m_item, x - 64, y);
}

static void open_item(int i, int how);

static void quit(void)
{
    _kernel_swi_regs r;
    save_stop(NULL);
    if (S.speed.active)
        speed_close();
    builtin_stop(0);
    draw_done();
    for (int i = 0; i < PEND_MAX; i++)
        if (S.pend[i].ref)
            remove(S.pend[i].path);
    r.r[0] = S.task;
    swi(Wimp_CloseDown, &r);
}

static void set_psize(int k)
{
    if (k < 0 || k > 2 || k == S.psize)
        return;
    S.psize = k;
    choices_save();
    cache_free_all();               /* posters are made at the tile's size */
    if (S.have_list)
        make_disp();
    if (S.browser_open) {
        int st[9];
        window_state(S.browser_w, st);
        S.cols = layout_cols(st[3] - st[1]);
        set_extent();
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    }
    if (S.page == PG_DETAILS)
        det_refresh();              /* its backdrop went with the cache */
    S.posters_wanted = 1;
}

/* Subtitles menu choice k: 0 None, else track k-1 of the video in S.det */
static void choose_sub(int k)
{
    const plex_item *it = det_item();
    char name[120];
    if (!it || k < 0 || k > it->nsubs)
        return;
    latin1(k ? it->subs[k - 1].title : "", name, sizeof(name));
    hourglass(1);
    if (plex_set_subtitle(&S.px, it, k ? it->subs[k - 1].id : 0) != 0) {
        hourglass(0);
        report("Can't choose the subtitles: %s", S.px.err);
        return;
    }
    hourglass(0);
    det_refresh();
    if (k)
        set_status("Subtitles: %s. The server burns them into the picture.", name);
    else
        set_status("Subtitles off.");
}

/* Watched (1) or not (0), on the server and here. it: the list's item or
   the details' */
static void mark(const plex_item *it, int watched)
{
    hourglass(1);
    if (plex_mark(&S.px, it, watched) != 0) {
        hourglass(0);
        report("Can't mark it: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; S.have_list && i < S.list.n; i++)
        if (S.list.v[i].rating_key && it->rating_key && !strcmp(S.list.v[i].rating_key, it->rating_key)) {
            S.list.v[i].watched = watched;
            S.list.v[i].view_offset_ms = 0;
            redraw_tile(i);
        }
    if (det_item() && det_item()->rating_key && it->rating_key &&
        !strcmp(det_item()->rating_key, it->rating_key)) {
        S.det.v[0].watched = watched;
        S.det.v[0].view_offset_ms = 0;
        det_repaint();
    }
    set_status(watched ? "Marked as watched." : "Marked as not watched.");
}

/* 1 if it's time to stop */
static int menu_select(const int *sel)
{
    int b[5], kind = S.menu_kind;
    _kernel_swi_regs r;
    if (kind == 1) {
        switch (sel[0]) {
        case MB_SIGNIN:
            signin_open();
            break;
        case MB_SERVERS:
            if (sel[1] >= 0 && sel[1] < S.nservers && use_server(sel[1]) == 0)
                browser_top();
            break;
        case MB_PLAYER:
            if (sel[1] >= 0 && sel[1] < PLAYER_COUNT) {
                S.player = sel[1];
                choices_save();
            }
            break;
        case MB_QUALITY:
            if (sel[1] >= 0 && sel[1] < Q_COUNT) {
                S.quality = sel[1];
                choices_save();
                det_repaint();
            }
            break;
        case MB_SIZE:
            set_psize(sel[1]);
            break;
        case MB_DIRECT:
            S.direct = !S.direct;
            choices_save();
            det_repaint();          /* how it will play */
            break;
        case MB_SIGNOUT:
            sign_out();
            break;
        case MB_QUIT:
            if (S.save.active && !ask("A file is being saved. Stop saving it and quit?"))
                break;
            quit();
            return 1;
        }
    } else if (kind == 2) {
        const plex_item *it = sel_item();
        switch (sel[0]) {
        case MI_PLAY:
            if (it)
                open_item(S.sel, PLAY_DEFAULT);
            break;
        case MI_RESUME:
            if (it)
                open_item(S.sel, PLAY_RESUME);
            break;
        case MI_START:
            if (it)
                open_item(S.sel, PLAY_START);
            break;
        case MI_DETAILS:
            if (it)
                det_show(S.sel);
            break;
        case MI_SUBS:
            choose_sub(sel[1]);
            break;
        case MI_SAVE:
            if (S.save.active)
                save_stop("Stopped saving.");
            else if (it && S.sv_ok && !too_big(S.sv_size, S.sv_title))
                report("To save it, move to the right of Save original file, then drag the icon "
                       "to a directory display.");
            break;
        case MI_SPEED:
            speed_start(it);
            break;
        case MI_WATCHED:
        case MI_UNWATCHED:
            if (it)
                mark(it, sel[0] == MI_WATCHED);
            break;
        case MI_BACK:
            go_back();
            break;
        case MI_REFRESH:
            refresh_list();
            break;
        }
    } else if (kind == 3) {
        choose_sub(sel[0]);
    } else if (kind == 4 && S.pl.on) {
        switch (sel[0]) {
        case MP_AUDIO:
            choose_audio(sel[1]);
            break;
        case MP_VOLUME:
            if (sel[1] >= 0 && sel[1] < 5) {
                player_set_volume((100 - sel[1] * 25) / 100.0);
                S.volume = player_volume();
                choices_save();
            }
            break;
        case MP_PICTURE:
            if (sel[1] >= 0 && sel[1] < PIC_COUNT) {
                player_set_pic_mode(sel[1]);
                S.pic_mode = sel[1];
                choices_save();
            }
            break;
        case MP_STATS:
            player_set_stats(!player_stats());
            break;
        case MP_FULL:
            player_set_fullscreen(!player_fullscreen());
            break;
        case MP_OVERLAY:
            player_set_overlay(!player_overlay());
            S.overlay = player_overlay();
            choices_save();
            break;
        case MP_STOP:
            builtin_stop(1);
            return 0;
        }
    }
    /* Adjust keeps the menu open */
    r.r[1] = (intptr_t)b;
    if (!swi(Wimp_GetPointerInfo, &r) && (b[2] & 1)) {
        if (kind == 1)
            bar_menu_open(S.menu_x);
        else if (kind == 2 && S.browser_open)
            item_menu_open(S.menu_x, S.menu_y);
        else if (kind == 4 && S.pl.on) {
            player_menu_build();
            S.menu_kind = 4;
            open_menu(&m_play, S.menu_x - 64, S.menu_y);
        }
    }
    return 0;
}

static void open_item(int i, int how)
{
    const plex_item *it;
    if (i < 0 || i >= S.list.n)
        return;
    it = &S.list.v[i];
    if (it->kind == PI_FOLDER) {
        char path[256], back[64];
        snprintf(path, sizeof(path), "%s", it->key ? it->key : "");
        snprintf(back, sizeof(back), "%s", S.list.title);
        show_list(path, back, 1, 0);
    } else if (it->kind == PI_VIDEO) {
        play_item(it, how);
    } else {
        set_status("%s libraries can't be opened yet: films and TV only.", it->subtitle ? it->subtitle : "These");
    }
}

/* ---- events --------------------------------------------------------------------- */

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
        if (S.page == PG_SIGNIN) {
            int id = si_hit(b[0], b[1]);
            if (!(buttons & 0x505))
                return;
            if (id == S_NEWCODE)
                pin_new();
            else if (id == S_USE)
                use_manual();
            else if (id == S_ADDR || id == S_TOK) {
                S.field = id == S_TOK;
                si_redraw_fields();
            }
            return;
        }
        if (S.page == PG_DETAILS) {
            int id = det_button_at(b[0], b[1]);
            if (buttons & 2)
                item_menu_open(b[0], b[1]);
            else if (id && (buttons & 0x505))
                det_action(id, b[0], b[1]);
            return;
        }
        t = tile_at(b[0], b[1]);
        if (buttons & 2) {
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
        if (cols != S.cols) {
            S.cols = cols;
            if (S.page == PG_GRID) {
                set_extent();
                force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
            }
        }
        if (S.page == PG_DETAILS && w != det_wd)
            det_repaint();          /* the text wrapped to the new width */
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
        if (S.page == PG_SIGNIN)
            signin_close();
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
    if (S.cast_wanted && S.page == PG_DETAILS && det_cast_step())
        return;
    if (S.posters_wanted && poster_step())
        return;
    if (S.in_browser && S.browser_open) {   /* the poster under the pointer */
        int p[5];
        _kernel_swi_regs r;
        r.r[1] = (intptr_t)p;
        if (!swi(Wimp_GetPointerInfo, &r))
            set_hover(p[3] == S.browser_w ? tile_at(p[0], p[1]) : -1);
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

int plexro_main(int argc, char **argv)
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
    snprintf(S.agent, sizeof(S.agent), APP "/" PLEXRO_VERSION " (RISC OS)");
    net_init(S.agent);
    choices_load();
    choices_save();                 /* the client id is kept from the start */
    read_screen();
    draw_init(S.xeig, S.yeig);
    snprintf(S.code, sizeof(S.code), "-");
    snprintf(S.title, sizeof(S.title), APP);
    make_windows();
    iconbar_icon();
    S.proginfo = proginfo_create(APP, PURPOSE, APP_AUTHOR, PLEXRO_VERSION " (" PLEXRO_DATE ")");
    player_init(S.task, S.overlay, S.pic_mode, S.volume);

    for (;;) {
        int mask = 1 << 11 | 1 << 12;  /* no caret events */
        int reason = Wimp_Poll;
        if (S.save.active || S.speed.active) {
            /* null events at once */
        } else if (S.pl.on) {           /* the built-in player says how often */
            int cs = player_poll_cs();
            if (S.pl.upnext_cs && (cs < 0 || cs > 25))
                cs = 25;                /* the card's countdown */
            if (cs > 0) {
                reason = Wimp_PollIdle;
                r.r[2] = now_cs() + cs;
            } else if (cs < 0)
                mask |= 1;
        } else if (S.search_due && searching()) {
            reason = Wimp_PollIdle;         /* the search, a moment after the last key */
            r.r[2] = S.search_due;
        } else if (S.posters_wanted || (S.cast_wanted && S.page == PG_DETAILS && S.browser_open)) {
            /* null events at once */
        } else if (S.pin_id && S.browser_open && S.page == PG_SIGNIN) {
            reason = Wimp_PollIdle;
            r.r[2] = S.pin_next;
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

#ifdef PLEXRO_TEST
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
const char *ui_test_item(int i, int line) { return S.disp && i >= 0 && i < S.list.n ? S.disp[i].line[line & 1] : ""; }
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
    return what == 0 ? S.code : what == 1 ? S.si_status : what == 2 ? S.si_addr : S.si_tok;
}
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
        for (int i = 0; i < 4; i++)
            if (si_box[i].id == id) {
                x0 = si_box[i].x0; y0 = si_box[i].y0; x1 = si_box[i].x1; y1 = si_box[i].y1;
                found = 1;
            }
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

#ifndef PLEXRO_NO_MAIN
int main(int argc, char **argv) { return plexro_main(argc, argv); }
#endif
