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
 * Plain Wimp SWIs, as in Reel: no Toolbox, no Templates.
 * Part of riscos-matinee. GPL v2 or later.
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
#include "jellyfin.h"
#include "dlna.h"
#include "caps.h"
#include "handoff.h"
#include "net.h"
#include "ui.h"
#include "version.h"
#include "proginfo.h"
#include "draw.h"
#include "player.h"
#include "imgcache.h"

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

#define APP       "Matinee"
#define ICON      "!matinee"
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
#define TOPBAR_H  128               /* Back, where you are, the status line, Refresh */
#define STRIP_H   88                /* under it: Home and the libraries, as tabs */
static int strip_h(void);
#define HEADER_H  (TOPBAR_H + strip_h())
#define LIBBAR_H  168               /* a library's bar: Library / Collections / Playlists, sort, filter; A-Z
                                       (and a line more when the window's too narrow for one) */
static int libbar_h(void);
#define BR_MIN_W  (2 * (TILE_W + GAP) + GAP)
#define BTN       64                /* a button's height */

/* The details page */
#define DET_BTN_MAX 14

#define SAVE_STEP (256 * 1024)      /* bytes saved each null event */
#define FILE_MAX  4294967295LL      /* 4GB-1: the biggest file FileCore holds */
#define PIN_EVERY 200               /* cs between sign-in checks */
#define POSTER_CACHE_MAX (48 << 20) /* bytes of poster sprites kept */
#define HISTORY_MAX 16
#define SERVERS_MAX 8
#define PEND_MAX  4

#ifdef __riscos__
/* UnixLib: the heap in a dynamic area of its own (the posters live there) */
const char *const __dynamic_da_name = "Matinee Heap";
int __dynamic_da_max_size = 512 << 20;    /* the posters, and the player's pictures and packets */
#endif

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
static void force_redraw(int w, int x0, int y0, int x1, int y1);

/* ---- state -------------------------------------------------------------- */

typedef struct poster {
    struct poster *next;
    unsigned hash;                  /* thumb's (cache_find compares these first) */
    char thumb[256];
    int *area;                      /* a sprite area with one sprite, "p" (rounded: masked corners) */
    size_t bytes;
    int failed;
} poster_t;

typedef struct {
    char line[2][80];               /* Latin-1, cut to fit the tile */
    char sum[2][160];               /* the show page: an episode's summary, two lines */
    poster_t *poster;
    int fitted;                     /* the grid's lines are cut to fit when first drawn (fit_disp) */
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

/* The details page's rows of other videos, and the order they're shown in:
   the collection first, then More like this, then the extras */
enum { REL_SIMILAR, REL_EXTRAS, REL_COLLECTION, REL_ROWS };
static const int rel_order[REL_ROWS] = { REL_COLLECTION, REL_SIMILAR, REL_EXTRAS };

/* A Jellyfin server signed in to: kept in Choices, one of the Servers menu */
#define JF_SAVED 6
#define SRV_ADD (-100)
typedef struct {
    char name[64], id[64], base[256], token[128], uid[48], uname[64];
} jf_saved;
/* A DLNA server used: its name, UDN and description's address (Choices) */
#define DL_SAVED 6
#define SRV_DL(k) (-200 - (k))      /* the Servers menu's DLNA server k */
#define SRV_FORGET (-150)           /* the Servers menu's Forget a server (its submenu) */
/* Forget a server's items: the Plex sign-in, Jellyfin server k, DLNA server k */
#define FG_PLEX 0
#define FG_JF(k) (100 + (k))
#define FG_DL(k) (200 + (k))
typedef struct {
    char name[64], udn[96], loc[256];
} dl_saved;

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
    /* Plex Home: the people on the account (nusers -1: not asked yet), and
       who's watching (Choices) */
    plex_user users[PLEX_USERS];
    int nusers;
    char user_uuid[64], user_title[64];

    /* sign-in */
    long long pin_id;
    int pin_next;                   /* monotonic time of the next check */
    char code[16], si_status[160], si_addr[256], si_tok[256];
    /* Jellyfin: the servers signed in to (Choices); the sign-in page's
       Jellyfin half: the server being signed in to, its Quick Connect code */
    jf_saved jf[JF_SAVED];
    int njf;
    int si_mode;                    /* the sign-in page: SI_PICK, SI_PLEX, SI_JF or SI_DLNA */
    int si_prev;                    /* the page before it (Cancel goes back there) */
    plex_ctx jfc;
    char jf_addr[256], jf_user[64], jf_pw[128], jf_secret[96], jf_code[16];
    int qc_next;
    /* DLNA: the servers used (Choices); the sign-in page's DLNA half: the
       servers found on the network, an address typed */
    dl_saved dl[DL_SAVED];
    int ndl;
    dlna_server found[DLNA_FOUND];
    int nfound, searched;
    char dl_addr[256];
    int srv_map[16];                /* the Servers menu: >= 0 Plex server n, -1 - k Jellyfin k, SRV_DL(k)
                                       DLNA k, SRV_ADD, SRV_FORGET */
    int fg_map[16];                 /* Forget a server's submenu: FG_PLEX, FG_JF(k), FG_DL(k) */

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
    int more_wanted;                /* the list (a library) has more on the server: fetched a page at a time */
    int sel_want;                   /* the selection to come back to, in a page not fetched yet (-1 none) */
    int hover, in_browser;          /* the poster under the pointer (-1: none) */
    poster_t *cache;
    size_t cache_bytes;

    /* details */
    plex_list det;                  /* one item, from plex_details() */
    int have_det, det_i;            /* det_i: its place in the list shown (-1: gone) */
    poster_t *det_art, *det_poster;
    /* the labels under the title (year, running time...; then the genres) */
    struct { int x0, y0, x1, kind; char t[64]; } chip[16];
    int nchip;
    char det_title[120], det_meta[160], det_how[200], det_how_l[2][160], det_lines[10][160];
    int det_how_n;
    int det_nlines, det_h;
    /* the rest of the metadata: label and value lines, and the cast */
    char det_cred[8][2][160];
    int det_ncred;
    struct { poster_t *photo; char name[64], role[64]; int x0, y0; } cast[16];
    /* under the cast: More like this (posters) and extras (trailers, 16:9) */
    /* the details page's rows of other videos (REL_*): More like this, the
       trailers and extras, and the rest of the collection it's in */
    plex_list rel[REL_ROWS];
    struct { int x0, y0, x1, y1; poster_t *pic; char line[2][80]; } relc[REL_ROWS][16];
    int nrel[REL_ROWS], rel_y[REL_ROWS], rel_wanted;
    char rel_head[REL_ROWS][100];
    int ncast, cast_wanted;         /* cast_wanted: photos still to fetch (null events) */
    struct { int id, x0, y0, x1, y1; char label[48]; } btn[DET_BTN_MAX + 4];
    int nbtn;

    /* the show page: a show's backdrop, poster, title, labels, summary and
       buttons, its series as tabs, and the series shown as a list of
       episodes (the list shown: S.list is the series' episodes) */
    struct {
        int on;                     /* the list shown is one of the show's series */
        int have;                   /* det and seasons hold a show */
        plex_list det, seasons;     /* the show (one item), its series */
        int season;                 /* the one shown (in seasons) */
        poster_t *art, *poster;
        int w, head_h;              /* the width laid out for; the header's height */
        int art_w, art_h, pw, ph, fw, due;  /* the boxes; the width fetched at; when to fetch again */
        int tx, title_y, sum_y, count_x, count_y;
        char title[120], count[80], lines[3][160];
        int nlines;
        struct { int x0, y0, x1; char t[48]; } chip[8];
        int nchip;
        struct { int x0, y0, x1, y1; char t[48]; } tab[12];
        int ntab;
        struct { int id, x0, y0, x1, y1; char t[48]; } btn[2];
        int nbtn;
        int next;                   /* the episode Play plays (in S.list) */
    } show;

    plex_list libs;                 /* the libraries (for the tabs under the bar) */
    /* a library's list: which view (its films or shows, its collections, the
       playlists), how it's sorted, unwatched only; from the list's path */
    struct {
        int on, sec, view, sort, unwatched, h;
        struct { int id, x0, y0, x1, y1; char t[48]; } btn[6];
        int nbtn;
        int az_first[27];           /* each letter's first item (# then A-Z), -1 none */
        int az_y;
    } lib;

    /* the home page: a featured item (from Continue watching) across the
       top, then rows: Continue watching, Recently added in each library,
       the libraries. S.list holds every row's items one after another. */
    struct {
        int on;
        plex_row row[8];
        int nrows;
        struct { int title_y, top, cw, ch, vis, title_x1; } lay[8];
        int w, h, hero_h;           /* the width laid out for; the page's height; the featured part's */
        int pick, npick;            /* the featured item: which of Continue watching's first npick */
        poster_t *art;
        int art_w, fw, due;
        char label[40], title[120], lines[2][160], left[40];
        int nlines, title_y, sum_y;
        struct { int x0, y0, x1; char t[48]; } chip[6];
        int nchip;
        struct { int id, x0, y0, x1, y1; char t[48]; } btn[2];
        int nbtn, left_x;
        struct { int x0, y0, x1, y1; } dot[5];
    } home;

    /* menus */
    int menu_kind;                  /* 1 icon bar, 2 item, 3 subtitles, 4 player, 5 player's subtitles */
    int menu_x, menu_y;

    /* the built-in player: what it's playing, and for Plex */
    struct {
        int on;
        plex_list det;              /* the video's details (one item) */
        play_t p;                   /* how it's played (the stream's session) */
        plex_playing pq;            /* its play queue on the server, and the session */
        char sid[48];               /* the playback's session id ("" before the first part) */
        int ping_cs;                /* a converted stream, paused: when to tell the server it's wanted */
        int prev_page;              /* the page to go back to */
        int tl_cs;                  /* when the server is told where it's got to next */
        int upnext_cs;              /* the next episode starts then (0: no card) */
        plex_list next;             /* the next episode */
        int menu_audio;             /* the Sound track submenu is the server's tracks */
        /* subtitle files beside the video, fetched and given to the player */
        struct { long id; int track; char path[300]; } ext[8];
        int n_ext;
        int chap_first;             /* the Chapters submenu's first item's chapter */
    } pl;
    int overlay, pic_mode;          /* Choices for the built-in player */
    int cache_mb;                   /* Choices: the image cache's size */
    double volume;

    /* hand-offs waiting for DataLoadAck */
    pend_t pend[PEND_MAX];
    int play_n;
    char play_why[160];

    /* the save box, and a save in progress */
    char sv_name[256];
    char sv_leaf[200];
    char sv_url_key[1024], sv_title[80];
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

static void choices_save(void)
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

static void choices_load(void)
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
    memset(st, 0, 9 * sizeof(int));     /* all callers' blocks are 9 words; zero if the Wimp refuses */
    st[0] = w;
    r.r[1] = (intptr_t)st;
    if (swi(Wimp_GetWindowState, &r)) {
        memset(st, 0, 9 * sizeof(int));
        st[0] = w;
    }
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
    S.det_art = S.det_poster = NULL;
    S.show.art = S.show.poster = NULL;
    S.show.fw = 0;
    S.home.art = NULL;
    S.home.fw = 0;
    for (int i = 0; i < S.ncast; i++)
        S.cast[i].photo = NULL;
    for (int k = 0; k < REL_ROWS; k++)
        for (int i = 0; i < 16; i++)
            S.relc[k][i].pic = NULL;
    S.cast_wanted = S.ncast > 0 || S.nrel[0] || S.nrel[1] || S.nrel[2];
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
        for (int k = 0; k < REL_ROWS && !used; k++)
            for (int i = 0; i < S.nrel[k] && !used; i++)
                used = S.relc[k][i].pic == p;
        if (!used && p != S.det_art && p != S.det_poster && p != S.show.art && p != S.show.poster && p != S.home.art) {
            *pp = p->next;
            S.cache_bytes -= p->bytes;
            free(p->area);
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

static unsigned str_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* (A list of 2000 asks this for each item against a few hundred kept) */
static poster_t *cache_find(const char *thumb)
{
    unsigned h = str_hash(thumb);
    for (poster_t *p = S.cache; p; p = p->next)
        if (p->hash == h && !strcmp(p->thumb, thumb))
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
    /* taller than the box: most of what's cut comes off the foot, a fifth
       off the head (faces and titles are near the top of posters and
       backdrops; a cut through the middle took off heads) */
    r.r[2] = (th > h ? (h - th) * 4 / 5 : (h - th) / 2) << S.yeig;
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

/* The backdrop: darkened a little, faded into the window's grey over its
   lower part, and across from the left (where the title, the labels and
   the summary go, so they read as on the grey), as the picture fades into
   the page. Done once, when it's fetched: plotting it costs no more. */
static void sprite_fade(int *area, int w, int h)
{
    unsigned *px = (unsigned *)(area + 4 + 11);
    const int bg[3] = { 24, 26, 31 };
    int from = h * 40 / 100, solid = w * 48 / 100, clear = w * 86 / 100;
    static unsigned char across[4096];
    for (int x = 0; x < w && x < 4096; x++) {       /* how much grey from the left, in 256ths */
        int t = x <= solid ? 0 : x >= clear ? 256 : 256 * (x - solid) / (clear - solid);
        int smooth = t * t / 256 * (768 - 2 * t) / 256;         /* 0..256, easing in and out */
        across[x] = (unsigned char)(236 - 236 * smooth / 256);
    }
    for (int y = 0; y < h; y++) {
        /* down: how much of the grey, in 256ths */
        int kd = y < from ? 40 : 40 + (216 * (y - from) * (y - from)) / ((h - from) * (h - from));
        for (int x = 0; x < w; x++) {
            unsigned p = px[y * w + x];
            int ka = x < 4096 ? across[x] : 0, k = kd > ka ? kd : ka;
            int c[3] = { (int)(p & 255), (int)(p >> 8 & 255), (int)(p >> 16 & 255) };
            for (int i = 0; i < 3; i++)
                c[i] = (c[i] * (256 - k) + bg[i] * k) >> 8;
            px[y * w + x] = (unsigned)c[0] | (unsigned)c[1] << 8 | (unsigned)c[2] << 16;
        }
    }
}

/* A picture from the server (a poster, or a backdrop when art), into the
   cache under key */
/* A picture from the server at w x h pixels: from the disc if it's been
   fetched before, else fetched and kept there (imgcache) */
static int picture_get(const char *thumb, int w, int h, char **jpeg, size_t *len)
{
    char key[600];
    snprintf(key, sizeof(key), "%s|%s|%dx%d", S.px.server_id, thumb, w, h);
    if (imgcache_get(key, jpeg, len) == 0) {
        if (imgcache_jpeg_whole(*jpeg, *len))
            return 0;
        free(*jpeg);                /* cut short (kept by test28 or before): fetched again */
        *jpeg = NULL;
    }
    for (int tries = 0; tries < 2; tries++) {
        if (plex_poster(&S.px, thumb, w, h, jpeg, len) != 0)
            return -1;
        if (imgcache_jpeg_whole(*jpeg, *len)) {
            imgcache_put(key, *jpeg, *len);     /* (it replaces one cut short) */
            return 0;
        }
        if (tries == 0) {           /* cut short on the way: once more */
            free(*jpeg);
            *jpeg = NULL;
        }
    }
    return 0;                       /* the part there is, shown but not kept */
}

/* art: 0 a poster (rounded corners), 1 a backdrop (faded), 2 a person's
   photo (round, filling the circle), 3 an episode's picture (filling its
   box, rounded corners) */
static poster_t *poster_fetch(const char *thumb, const char *key, int w, int h, int art)
{
    int round = art == 2 ? w / 2 : art == 1 ? 0 : 12 >> S.xeig;
    poster_t *p = calloc(1, sizeof(*p));
    char *jpeg = NULL;
    size_t len = 0;
    if (!p)
        return NULL;
    snprintf(p->thumb, sizeof(p->thumb), "%s", key);
    p->hash = str_hash(p->thumb);
    if (picture_get(thumb, w, h, &jpeg, &len) != 0 ||
        !(p->area = sprite_make(w, h, round, &p->bytes)) ||
        jpeg_into(p->area, w, h, jpeg, len, art != 0) != 0) {
        free(p->area);
        p->area = NULL;
        p->bytes = sizeof(*p);      /* counted, so failed ones are trimmed too */
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

/* The show page's episode rows: a 16:9 picture, the title, the summary */
#define EP_TW   320                 /* the picture, OS units */
#define EP_TH   180
#define EP_ROW  (EP_TH + 40)
#define EP_GAP  16

static void home_box(int i, int *x0, int *y0, int *x1, int *y1);
static int home_row_of(int i, int *col);
static const char *home_pic(int i, char *key, size_t size, int *art);

static void tile_box(int i, int *x0, int *y0, int *x1, int *y1)
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

static int layout_cols(int width)
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

static void set_extent(void)
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

static void force_redraw(int w, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = x0; r.r[2] = y0; r.r[3] = x1; r.r[4] = y1;
    swi(Wimp_ForceRedraw, &r);
}

static void redraw_tile(int i)
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
static void make_disp(void)
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
static void fit_disp(int i)
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
static int searching(void)
{
    return S.page == PG_GRID && S.have_list && !strncmp(S.path, "search:", 7);
}

/* ox, oy: the work area's origin on the screen; vis_w: the width shown */
/* ---- Home and the libraries, as tabs under the bar ------------------------------------

   Always there on the browser's pages (not signing in), so a library is
   one click away from anywhere, as a TV app's top tabs. The one you're in
   (or came from) is light. */

#define LIB_TABS 16

static int vis_width(void);

static int strip_h(void)
{
    return S.page != PG_SIGNIN && S.page != PG_PLAYER && S.libs.n > 0 ? STRIP_H : 0;
}

/* Tab k (0 Home, then the libraries): where it is, work area */
static int tab_box(int k, int *x0, int *y0, int *x1, int *y1)
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
static int tab_current(void)
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
static int tab_hit(int sx, int sy)
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
static int badge_kind(const plex_item *it)
{
    if (it->watched && it->rating_key && it->kind != PI_OTHER)
        return 1;
    if (it->kind == PI_FOLDER && it->unwatched > 0)
        return 2;
    return 0;
}

static void badges(const plex_item *it, int x0, int y0, int x1, int y1)
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

static void det_redraw(int ox, int oy, int vis_w, int cy0, int cy1);
static void signin_redraw(int ox, int oy, int vis_w);
static void show_redraw(int ox, int oy, int cy0, int cy1);
static void draw_episode(int i, int ox, int oy);
static void home_redraw(int ox, int oy, int cy0, int cy1);
static void lib_redraw(int ox, int oy);
static void draw_home_card(int i, int ox, int oy);

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
                S.disp[i].poster = poster_fetch(pic, key, S.home.lay[r].cw >> S.xeig, S.home.lay[r].ch >> S.yeig, art);
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
                S.disp[i].poster = poster_fetch(it->thumb, key, EP_TW >> S.xeig, EP_TH >> S.yeig, 3);
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


/* ---- the show page ------------------------------------------------------------------

   A show opens as a page of its own (as Plex's apps show one): its
   backdrop across the top, its poster, title, labels and summary, Play
   (the next episode) and Mark watched, then its series as tabs and the
   series chosen as a list of episodes, each a 16:9 picture with its title,
   how long it is (and how much is left) and two lines of its summary. The
   list is S.list (the series' episodes), so selecting, opening, the menus
   and Back work as on the grid; S.show.on says how it's laid out. */


static int vis_width(void);

static void show_fetch_art(void)
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
static void show_layout(void)
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
static int show_hit(int sx, int sy)
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

static const char *const sort_names[] = { "Title", "Date added", "Year", "Rating" };
static const char *const sort_keys[] = { "", "addedAt:desc", "year:desc", "rating:desc" };
#define SORTS 4

static int libbar_h(void)
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

static void lib_path(int view, int sort, int unwatched, char *out, size_t size)
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
static int az_letter(const char *t)
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

static void lib_layout(void)
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
static void az_box(int k, int *x0, int *y0, int *x1, int *y1)
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
static int lib_hit(int sx, int sy)
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

static const plex_item *home_hero(void)
{
    int i = S.home.row[0].start + S.home.pick;
    if (!S.home.on || !S.home.npick || !S.have_list || S.home.nrows < 1 || i < 0 || i >= S.list.n)
        return NULL;
    return &S.list.v[i];
}

/* Which row item i is in, and its place in the row (-1: none) */
static int home_row_of(int i, int *col)
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
static void select_tile(int i);

static void home_box(int i, int *x0, int *y0, int *x1, int *y1)
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
static const char *home_pic(int i, char *key, size_t size, int *art)
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
static void home_layout(void)
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

static void home_fetch_art(void)
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

static void home_redraw(int ox, int oy, int cy0, int cy1)
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
static void draw_home_card(int i, int ox, int oy)
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
static int home_hit(int sx, int sy)
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
static void home_key(int k)
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
#define WIN_PERCENT 75
#define TITLE_H     40              /* the Wimp's title bar and scroll bar (about) */
#define SCROLL_W    40
static void browser_open(void)
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
static int show_list(const char *path, const char *back_title, int push, int sel)
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

static void browser_top(void);

/* A tab: Home, or a library (Back from it goes Home) */
static void tab_open(int k)
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

static int users_get(void);

/* The list shown let go, and the pages made from it (home, show, library bar) off */
static void builtin_stop(int leave);

static void list_forget(void)
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

static void browser_top(void)
{
    if (S.nusers < 0 && *S.px.account_token)
        users_get();                /* Plex Home: who's watching, for the title */
    S.nhist = 0;
    if (S.page == PG_PLAYER)
        builtin_stop(1);            /* not under the grid */
    list_forget();
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
static int det_wd = 1000, art_w, art_h;         /* the width laid out for; the backdrop's box */
static int art_fw, art_fh, art_due;             /* the size fetched; when to fetch it at the new size (0: not) */
static int det_pw, det_ph, det_pfw;             /* the poster's box (OS units), the width fetched */
static int det_tx, det_title_y, det_bx, det_stars_x, det_stars_y;       /* the text column's left; the title's baseline; buttons' left */
static int det_cred_x;                          /* the credits' labels' left */

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
    if (!S.browser_open)            /* the width it opens at (browser_open) */
        return (S.scr_w * WIN_PERCENT / 100 - SCROLL_W) & ~((1 << S.xeig) - 1);
    window_state(S.browser_w, st);
    return st[3] - st[1];
}

/* The backdrop's box: the window's width, 16:9, but at most 55% of the
   window's height (it grows and shrinks with the window) */
static void det_art_box(void)
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

/* The backdrop and the poster, for the boxes laid out for */
static void det_fetch_art(void)
{
    const plex_item *it = det_item();
    char key[320];
    S.det_art = S.det_poster = NULL;
    det_art_box();
    art_fw = art_w;
    art_fh = art_h;
    det_pfw = det_pw;
    art_due = 0;
    if (it && det_poster_thumb(it)) {
        snprintf(key, sizeof(key), "dposter:%s@%dx%d", det_poster_thumb(it), det_pw, det_ph);
        if (!(S.det_poster = cache_find(key)))
            S.det_poster = poster_fetch(det_poster_thumb(it), key, det_pw >> S.xeig, det_ph >> S.yeig, 0);
    }
    if (it && it->art) {
        snprintf(key, sizeof(key), "art:%s@%dx%d", it->art, art_w, art_h);
        if (!(S.det_art = cache_find(key)))
            S.det_art = poster_fetch(it->art, key, art_w >> S.xeig, art_h >> S.yeig, 1);
    }
    cache_trim();
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
    S.det_art = S.det_poster = NULL;
    for (int i = 0; i < (int)(sizeof(S.cast) / sizeof(S.cast[0])); i++)
        S.cast[i].photo = NULL;
    S.ncast = 0;
    S.cast_wanted = 1;              /* laid out by det_layout, fetched from null events */
    for (int k = 0; k < REL_ROWS; k++) {    /* More like this (not for an episode), its extras, its collection */
        const plex_item *v = &S.det.v[0];
        char path[260];
        if (S.rel[k].n || S.rel[k].v)
            plex_list_free(&S.rel[k]);
        S.nrel[k] = 0;
        S.rel_head[k][0] = 0;
        if (!v->rating_key || (k == REL_SIMILAR && v->type && !strcmp(v->type, "episode")))
            continue;
        if (k == REL_COLLECTION) {
            if (!v->collection || !v->collection_key)
                continue;
            snprintf(path, sizeof(path), "%s", v->collection_key);
            latin1(v->collection, S.rel_head[k], sizeof(S.rel_head[k]));
        } else {
            snprintf(path, sizeof(path), "/library/metadata/%s/%s", v->rating_key, k == REL_EXTRAS ? "extras" : "similar");
            snprintf(S.rel_head[k], sizeof(S.rel_head[k]), "%s", k == REL_EXTRAS ? "Trailers and extras" : "More like this");
        }
        if (plex_list_get(&S.px, path, &S.rel[k]) != 0)
            memset(&S.rel[k], 0, sizeof(S.rel[k]));
        if (k == REL_COLLECTION) {  /* the others in it: not this one */
            plex_list *c = &S.rel[k];
            for (int i = 0; i < c->n; i++)
                if (c->v[i].rating_key && !strcmp(c->v[i].rating_key, v->rating_key)) {
                    plex_item_free(&c->v[i]);
                    memmove(&c->v[i], &c->v[i + 1], (c->n - i - 1) * sizeof(plex_item));
                    c->n--;
                    i--;
                }
        }
    }
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
                S.relc[k][i].pic = poster_fetch(r->thumb, key, cw >> S.xeig, ch >> S.yeig, k == REL_EXTRAS ? 3 : 0);
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
    if ((S.det_art && (art_w != art_fw || art_h != art_fh)) || (S.det_poster && det_pw != det_pfw))
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

static void det_redraw(int ox, int oy, int vis_w, int cy0, int cy1)
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
    } else if (det_art_box(), !S.det_art || art_w != art_fw || art_h != art_fh) {
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
/* More like this or an extra at a screen point: 1000 * (k + 1) + i, or 0 */
static int det_rel_at(int sx, int sy)
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

/* Before a server is chosen the window shows the sign-in page. Plex's: the
   code to type at plex.tv/link (checked every 2 seconds), New code, and a
   server's address and token for signing in without plex.tv; and
   "Jellyfin server..." for the other half. Jellyfin's: the server's
   address, Quick Connect (a code to type into Jellyfin, checked every 2
   seconds), or a name and password; and "Plex instead". The fields are
   drawn here and typed into here (the window has the caret): Tab or the
   arrow keys move between them, Return goes on. */

static void choose_server(void);
static void jf_signed_in(void);
static void builtin_stop(int leave);
static void browser_top(void);

#define SI_MAX 24
static struct { int id, x0, y0, x1, y1; } si_box[SI_MAX];
static int si_n;

static void si_add_h(int id, int x0, int y1, int w, int h)
{
    if (si_n >= SI_MAX)
        return;
    si_box[si_n].id = id;
    si_box[si_n].x0 = x0;
    si_box[si_n].x1 = x0 + w;
    si_box[si_n].y1 = y1;
    si_box[si_n].y0 = y1 - h;
    si_n++;
}

static void si_add(int id, int x0, int y1, int w)
{
    if (si_n >= SI_MAX)
        return;
    si_box[si_n].id = id;
    si_box[si_n].x0 = x0;
    si_box[si_n].x1 = x0 + w;
    si_box[si_n].y1 = y1;
    si_box[si_n].y0 = y1 - BTN;
    si_n++;
}

static const char *si_label(int id)
{
    switch (id) {
    case S_NEWCODE: return "New code";
    case S_USE:     return "Use these";
    case S_PICK_PLEX: return "Plex";
    case S_PICK_JF: return "Jellyfin";
    case S_PICK_DLNA: return "DLNA";
    case S_DUSE:    return "Use";
    case S_DSEARCH: return "Search again";
    case S_BACK:    return "Back";
    case S_CANCEL:  return "Cancel";
    case S_QC:      return S.jf_secret[0] ? "New code" : "Quick Connect";
    case S_LOGIN:   return "Sign in";
    }
    return "";
}

static int si_bw(int id)
{
    return draw_width(D_BOLD, si_label(id)) + 64;
}

/* The DLNA page: where the part for an address starts, below the servers found */
static int dl_below(void)
{
    int n = S.nfound < SI_FOUND_SHOWN ? S.nfound : SI_FOUND_SHOWN;
    return -HEADER_H - 160 - (n ? n : 1) * 84;
}

static void si_layout(void)
{
    int top = -HEADER_H;
    si_n = 0;
    if (S.si_mode == SI_PICK) {         /* three cards, then Cancel */
        si_add_h(S_PICK_PLEX, 40, top - 176, 1160, 150);
        si_add_h(S_PICK_JF, 40, top - 360, 1160, 150);
        si_add_h(S_PICK_DLNA, 40, top - 544, 1160, 150);
        si_add(S_CANCEL, 40, top - 744, si_bw(S_CANCEL));
    } else if (S.si_mode == SI_DLNA) {  /* the servers found, Search again; an address, Use, Back, Cancel */
        int y = dl_below(), n = S.nfound < SI_FOUND_SHOWN ? S.nfound : SI_FOUND_SHOWN;
        for (int i = 0; i < n; i++)
            si_add_h(S_FOUND + i, 40, top - 160 - i * 84, 1160, 72);
        si_add(S_DSEARCH, 40, y - 16, si_bw(S_DSEARCH));
        si_add(S_DADDR, 220, y - 200, 620);
        si_add(S_DUSE, 220, y - 284, si_bw(S_DUSE));
        si_add(S_BACK, 220 + si_bw(S_DUSE) + 32, y - 284, si_bw(S_BACK));
        si_add(S_CANCEL, 220 + si_bw(S_DUSE) + si_bw(S_BACK) + 64, y - 284, si_bw(S_CANCEL));
    } else if (S.si_mode == SI_PLEX) {
        si_add(S_NEWCODE, 40, top - 372, si_bw(S_NEWCODE));
        si_add(S_ADDR, 220, top - 572, 620);
        si_add(S_TOK, 220, top - 652, 620);
        si_add(S_USE, 220, top - 736, si_bw(S_USE));
        si_add(S_BACK, 220 + si_bw(S_USE) + 32, top - 736, si_bw(S_BACK));
        si_add(S_CANCEL, 220 + si_bw(S_USE) + si_bw(S_BACK) + 64, top - 736, si_bw(S_CANCEL));
    } else {
        si_add(S_JADDR, 220, top - 132, 620);
        si_add(S_QC, 40, top - 372, si_bw(S_QC));
        si_add(S_JUSER, 220, top - 572, 620);
        si_add(S_JPW, 220, top - 652, 620);
        si_add(S_LOGIN, 220, top - 736, si_bw(S_LOGIN));
        si_add(S_BACK, 220 + si_bw(S_LOGIN) + 32, top - 736, si_bw(S_BACK));
        si_add(S_CANCEL, 220 + si_bw(S_LOGIN) + si_bw(S_BACK) + 64, top - 736, si_bw(S_CANCEL));
    }
}

static int si_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    si_layout();
    for (int i = 0; i < si_n; i++)
        if (wx >= si_box[i].x0 && wx < si_box[i].x1 && wy >= si_box[i].y0 && wy < si_box[i].y1)
            return si_box[i].id;
    return 0;
}

/* The fields: their text, and which is which (the caret's S.field) */
static int si_field_of(int id)
{
    switch (id) {
    case S_ADDR: case S_JADDR: case S_DADDR: return 0;
    case S_TOK: case S_JUSER:  return 1;
    case S_JPW:                return 2;
    }
    return -1;
}

static char *si_field(int f, size_t *size)
{
    char *p;
    if (S.si_mode == SI_DLNA) {
        *size = sizeof(S.dl_addr);
        return S.dl_addr;
    }
    if (S.si_mode != SI_JF) {
        p = f ? S.si_tok : S.si_addr;
        *size = f ? sizeof(S.si_tok) : sizeof(S.si_addr);
    } else {
        p = f == 0 ? S.jf_addr : f == 1 ? S.jf_user : S.jf_pw;
        *size = f == 0 ? sizeof(S.jf_addr) : f == 1 ? sizeof(S.jf_user) : sizeof(S.jf_pw);
    }
    return p;
}

static int si_fields(void)
{
    return S.si_mode == SI_JF ? 3 : S.si_mode == SI_PLEX ? 2 : S.si_mode == SI_DLNA ? 1 : 0;
}

/* The code, big and spaced out, on a card; the card's right edge (work area) */
static int si_code(int ox, int oy, const char *code)
{
    int top = -HEADER_H;
    char t[64];
    size_t n = 0;
    for (const char *c = code; *c && n + 3 < sizeof(t); c++) {
        t[n++] = *c;
        if (c[1]) { t[n++] = ' '; t[n++] = ' '; }
    }
    t[n] = 0;
    int w = draw_width(D_TITLE, t) + 120;
    if (w < 420)                    /* Plex's four letters; Jellyfin's six digits wider */
        w = 420;
    draw_round(ox + 40, oy + top - 292, ox + 40 + w, oy + top - 160, 20, C_CARD, C_BG);
    draw_text(D_TITLE, ox + 40 + (w - draw_width(D_TITLE, t)) / 2, oy + top - 244, t, C_TEXT, C_CARD);
    return 40 + w;
}

static void signin_redraw(int ox, int oy, int vis_w)
{
    int top = -HEADER_H;
    char t[300];
    (void)vis_w;
    si_layout();
    if (S.si_mode == SI_PICK) {
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Add a server", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 124, "Which kind is it?", C_SUB, C_BG);
    } else if (S.si_mode == SI_DLNA) {
        int y = dl_below();
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Add a DLNA server", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 124,
                  S.nfound ? "On your network (click one to use it):" :
                  S.searched ? "None found on your network." : "Looking on your network...", C_SUB, C_BG);
        draw_text(D_BODY, ox + 40 + si_bw(S_DSEARCH) + 32, oy + y - 58, S.si_status, C_SUB, C_BG);
        draw_rect(ox + 40, oy + y - 108, ox + 1000, oy + y - 106, C_CARD);
        draw_text(D_BOLD, ox + 40, oy + y - 160, "Or its address", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + y - 242, "Address", C_SUB, C_BG);
        draw_text(D_BODY, ox + 220, oy + y - 400, "As 192.168.1.10:8200 (MiniDLNA), :32469 (Plex), or its description's",
                  C_SUB, C_BG);
        draw_text(D_BODY, ox + 220, oy + y - 440, "address (http://.../rootDesc.xml).", C_SUB, C_BG);
    } else if (S.si_mode == SI_PLEX) {
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Sign in to Plex with a code", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 124, "On a phone or computer, go to plex.tv/link and type this code:",
                  C_SUB, C_BG);
        {
            int x1 = si_code(ox, oy, S.code);
            draw_text(D_BODY, ox + x1 + 40, oy + top - 244, S.si_status, C_SUB, C_BG);
        }
        draw_rect(ox + 40, oy + top - 468, ox + 1000, oy + top - 466, C_CARD);
        draw_text(D_BOLD, ox + 40, oy + top - 520, "Or a server on your network", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 616, "Address", C_SUB, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 696, "Token", C_SUB, C_BG);
    } else {
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Sign in to a Jellyfin server", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 176, "Address", C_SUB, C_BG);
        if (S.jf_secret[0]) {
            si_code(ox, oy - 60, S.jf_code);
            draw_text(D_BODY, ox + 40 + si_bw(S_QC) + 32, oy + top - 412, S.si_status, C_SUB, C_BG);
        } else {
            draw_text(D_BODY, ox + 40, oy + top - 236,
                      "Quick Connect gives a code to type into Jellyfin on a phone or computer", C_SUB, C_BG);
            draw_text(D_BODY, ox + 40, oy + top - 284, "(its user menu, then Quick Connect).", C_SUB, C_BG);
            draw_text(D_BODY, ox + 40 + si_bw(S_QC) + 32, oy + top - 412, S.si_status, C_SUB, C_BG);
        }
        draw_rect(ox + 40, oy + top - 468, ox + 1000, oy + top - 466, C_CARD);
        draw_text(D_BOLD, ox + 40, oy + top - 520, "Or your name and password", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 616, "Name", C_SUB, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 696, "Password", C_SUB, C_BG);
    }
    for (int i = 0; i < si_n; i++) {
        int x0 = ox + si_box[i].x0, y0 = oy + si_box[i].y0, x1 = ox + si_box[i].x1, y1 = oy + si_box[i].y1;
        int f = si_field_of(si_box[i].id);
        if (f >= 0) {
            size_t size;
            const char *v = si_field(f, &size);
            int tx = x0 + 20;
            if (si_box[i].id == S_TOK || si_box[i].id == S_JPW) {   /* shown as dots */
                size_t n = strlen(v);
                if (n > 60) n = 60;
                memset(t, 0xB7, n);
                t[n] = 0;
            } else {
                snprintf(t, sizeof(t), "%s", v);
            }
            if (S.field == f)       /* the caret's field: a blue edge */
                draw_round(x0 - 4, y0 - 4, x1 + 4, y1 + 4, 16, C_ACCENT, C_BG);
            draw_round(x0, y0, x1, y1, 12, C_CARD, S.field == f ? C_ACCENT : C_BG);
            draw_text(D_BODY, tx, y0 + 22, t, C_TEXT, C_CARD);
            if (S.field == f) {     /* the caret */
                int cx = tx + draw_width(D_BODY, t) + 2;
                draw_rect(cx, y0 + 14, cx + 4, y1 - 14, C_ACCENT);
            }
        } else if (si_box[i].id == S_PICK_PLEX || si_box[i].id == S_PICK_JF || si_box[i].id == S_PICK_DLNA) {
            int id = si_box[i].id;
            draw_round(x0, y0, x1, y1, 20, C_CARD, C_BG);
            draw_text(D_TITLE, x0 + 40, y1 - 70, si_label(id), C_TEXT, C_CARD);
            draw_text(D_BODY, x0 + 40, y0 + 28,
                      id == S_PICK_JF ? "Quick Connect, or your name and password" :
                      id == S_PICK_DLNA ? "On your network: MiniDLNA, a NAS, Plex's or Jellyfin's" :
                           "A code at plex.tv/link, or a server's address and token", C_SUB, C_CARD);
        } else if (si_box[i].id >= S_FOUND && si_box[i].id < S_FOUND + SI_FOUND_SHOWN) {
            const dlna_server *d = &S.found[si_box[i].id - S_FOUND];
            char nm[80];
            latin1(d->name, nm, sizeof(nm));
            draw_round(x0, y0, x1, y1, 16, C_CARD, C_BG);
            draw_text(D_BOLD, x0 + 32, y0 + 22, nm, C_TEXT, C_CARD);
            draw_text(D_BODY, x0 + 640, y0 + 22, d->host, C_SUB, C_CARD);
        } else {
            int id = si_box[i].id;
            unsigned bg = id == S_USE || id == S_LOGIN || id == S_DUSE ? C_ACCENT : C_CARD;
            draw_round(x0, y0, x1, y1, BTN / 2, bg, C_BG);
            draw_text(D_BOLD, x0 + 32, y0 + 22, si_label(id), C_TEXT, bg);
        }
    }
}

static void si_redraw_fields(void)
{
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, S.si_mode == SI_DLNA ? -HEADER_H - 1400 : -HEADER_H - 740, S.scr_w,
                     S.si_mode == SI_JF || S.si_mode == SI_DLNA ? -HEADER_H : -HEADER_H - 540);
}

static void pin_new(void)
{
    char code[16];
    long long id;
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

/* The window, showing the sign-in page: the choice of kind (SI_PICK),
   Plex's (SI_PLEX) or Jellyfin's (SI_JF). The page it was on is kept for
   Cancel (playing stops first). */
static void signin_show(int mode)
{
    int st[9];
    if (S.page == PG_PLAYER)
        builtin_stop(1);
    if (S.page != PG_SIGNIN) {
        S.si_prev = S.page;
        if (S.page == PG_GRID && S.browser_open) {
            window_state(S.browser_w, st);
            S.grid_sy = st[6];      /* where the grid was scrolled to */
        }
    }
    S.page = PG_SIGNIN;
    S.si_mode = mode;
    S.field = 0;
    S.hover = -1;
    S.posters_wanted = 0;
    if (!S.browser_open)
        browser_open();
    set_where();
    set_extent();
    set_status(mode == SI_JF ? "Sign in to a Jellyfin server." : mode == SI_PLEX ? "Sign in to Plex." :
               mode == SI_DLNA ? "Add a DLNA server." : "Add a Plex, a Jellyfin or a DLNA server.");
    if (mode == SI_DLNA)
        S.si_status[0] = 0;
    window_state(S.browser_w, st);
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    set_caret(S.browser_w, -1, NULL);
    if (mode != SI_PLEX)
        S.pin_id = 0;               /* plex.tv isn't asked meanwhile */
    if (mode != SI_JF)
        S.jf_secret[0] = 0;
    if (mode == SI_PLEX && !S.pin_id)
        pin_new();
    if (mode == SI_JF && !S.jf_secret[0])
        si_set_status("Type the address first.");
}

static void signin_open(void)
{
    signin_show(SI_PICK);
}

static void signin_close(void)
{
    S.pin_id = 0;                   /* no more checks */
    S.jf_secret[0] = 0;
    S.jf_pw[0] = 0;
}

/* Cancel: no server added; back to the page before (the server in use's),
   or the window closed if there's none */
static void signin_cancel(void)
{
    int st[9];
    signin_close();
    if (!*S.px.base) {
        if (S.browser_open)
            close_window(S.browser_w);
        S.browser_open = 0;
        S.page = PG_GRID;
        return;
    }
    if (!S.have_list || (S.si_prev == PG_DETAILS && !S.have_det)) {
        browser_top();
        return;
    }
    S.page = S.si_prev == PG_DETAILS ? PG_DETAILS : PG_GRID;
    set_extent();
    set_where();
    set_status("%s", S.px.server_name);
    window_state(S.browser_w, st);
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, S.page == PG_GRID ? S.grid_sy : 0);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    if (S.page == PG_DETAILS)
        det_refresh();
    S.posters_wanted = S.page == PG_GRID;
}

/* A key on the sign-in page: 1 if it was used; 2 Use these (Plex), 3 Sign in (Jellyfin), 4 Cancel,
   5 Use (DLNA) */
static int signin_key(int k)
{
    size_t size, n;
    char *f;
    if (k == 0x1B)
        return 4;                   /* Escape: Cancel */
    if (S.si_mode == SI_PICK)
        return 0;
    f = si_field(S.field, &size);
    n = strlen(f);
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
    } else if (k == 9 || k == 0x18E) {
        S.field = (S.field + 1) % si_fields();
    } else if (k == 0x18F) {
        S.field = (S.field + si_fields() - 1) % si_fields();
    } else if (k == 13) {
        if (S.field + 1 < si_fields())
            S.field++;
        else
            return S.si_mode == SI_JF ? 3 : S.si_mode == SI_DLNA ? 5 : 2;
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
    builtin_stop(0);
    S.px = c;
    choices_save();
    signin_close();
    browser_top();
}

/* ---- signing in to Jellyfin ------------------------------------------------------------ */

/* The Jellyfin server at the address typed (S.jfc), if it isn't already */
static int jf_reach(void)
{
    char *a = S.jf_addr;
    a[strcspn(a, "\r\n")] = 0;
    if (!*a) {
        si_set_status("Type the server's address first (such as 192.168.1.10).");
        S.field = 0;
        si_redraw_fields();
        return -1;
    }
    S.jfc = S.px;                   /* who this computer is */
    si_set_status("Trying %s...", a);
    hourglass(1);
    if (jf_connect(&S.jfc, a) != 0) {
        hourglass(0);
        si_set_status("%s", S.jfc.err);
        return -1;
    }
    hourglass(0);
    return 0;
}

static void qc_new(void)
{
    char secret[96], code[16];
    int e;
    S.jf_secret[0] = 0;
    if (jf_reach() != 0)
        return;
    hourglass(1);
    e = jf_qc_start(&S.jfc, secret, sizeof(secret), code, sizeof(code));
    hourglass(0);
    if (e != 0) {
        si_set_status(e == -2 ? "Quick Connect is off on %s: sign in with your name and password."
                              : "Can't start Quick Connect: %s", e == -2 ? S.jfc.server_name : S.jfc.err);
        return;
    }
    snprintf(S.jf_secret, sizeof(S.jf_secret), "%s", secret);
    snprintf(S.jf_code, sizeof(S.jf_code), "%s", code);
    S.qc_next = now_cs() + PIN_EVERY;
    si_set_status("Type the code in Jellyfin: Quick Connect.");
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
}

static void qc_check(void)
{
    int e = jf_qc_check(&S.jfc, S.jf_secret);
    S.qc_next = now_cs() + PIN_EVERY;
    if (e == 0)
        return;
    if (e < 0) {
        S.jf_secret[0] = 0;
        si_set_status("The code has run out: click Quick Connect for another.");
        return;
    }
    jf_signed_in();
}

static void jf_password(void)
{
    int e;
    if (jf_reach() != 0)
        return;
    if (!*S.jf_user) {
        si_set_status("Type your name on %s.", S.jfc.server_name);
        S.field = 1;
        si_redraw_fields();
        return;
    }
    hourglass(1);
    e = jf_login(&S.jfc, S.jf_user, S.jf_pw);
    hourglass(0);
    if (e != 0) {
        si_set_status(e == -2 ? "%s didn't take that name and password." : "Can't sign in: %s",
                      e == -2 ? S.jfc.server_name : S.jfc.err);
        S.jf_pw[0] = 0;
        S.field = 2;
        si_redraw_fields();
        return;
    }
    jf_signed_in();
}

/* ---- servers -------------------------------------------------------------------- */

static void server_changed(void);
static int use_jf(int k);
static void jf_add(void);

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
    if (S.px.kind != SRV_PLEX || strcmp(S.px.server_id, c.server_id))
        server_changed();
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

/* ---- Jellyfin servers -------------------------------------------------------------- */

/* Choices' line can't take a '|' in a name */
static void no_bars(char *s)
{
    for (; *s; s++)
        if (*s == '|' || *s == '\n' || *s == '\r')
            *s = '/';
}

/* The server in use (a Jellyfin one) kept, the same server and user replaced */
static void jf_remember(void)
{
    jf_saved *j = NULL;
    for (int i = 0; i < S.njf; i++)
        if (!strcmp(S.jf[i].id, S.px.server_id) && !strcmp(S.jf[i].uid, S.px.user_id))
            j = &S.jf[i];
    if (!j) {
        if (S.njf >= JF_SAVED) {        /* the oldest makes room */
            memmove(&S.jf[0], &S.jf[1], (JF_SAVED - 1) * sizeof(jf_saved));
            S.njf--;
        }
        j = &S.jf[S.njf++];
    }
    snprintf(j->name, sizeof(j->name), "%s", S.px.server_name);
    snprintf(j->id, sizeof(j->id), "%s", S.px.server_id);
    snprintf(j->base, sizeof(j->base), "%s", S.px.base);
    snprintf(j->token, sizeof(j->token), "%s", S.px.token);
    snprintf(j->uid, sizeof(j->uid), "%s", S.px.user_id);
    snprintf(j->uname, sizeof(j->uname), "%s", S.px.user_name);
    no_bars(j->name);
    no_bars(j->uname);
}

/* Moving to another server: what was shown of the last one goes */
static void server_changed(void)
{
    builtin_stop(1);                /* off the player page too */
    if (S.libs.n || S.libs.v)
        plex_list_free(&S.libs);
    S.nhist = 0;
}

/* The server (a copy of the context, kind and all) made the one in use;
   the Plex account's sign-in stays */
static void server_take(const plex_ctx *c)
{
    plex_ctx *d = &S.px;
    d->kind = c->kind;
    d->local = c->local;
    snprintf(d->base, sizeof(d->base), "%s", c->base);
    snprintf(d->token, sizeof(d->token), "%s", c->token);
    snprintf(d->server_name, sizeof(d->server_name), "%s", c->server_name);
    snprintf(d->server_id, sizeof(d->server_id), "%s", c->server_id);
    snprintf(d->user_id, sizeof(d->user_id), "%s", c->user_id);
    snprintf(d->user_name, sizeof(d->user_name), "%s", c->user_name);
    snprintf(d->ctl, sizeof(d->ctl), "%s", c->ctl);
    snprintf(d->svc, sizeof(d->svc), "%s", c->svc);
}

/* Signed in on the sign-in page's Jellyfin half: that server, then its home page */
static void jf_signed_in(void)
{
    server_changed();
    server_take(&S.jfc);
    jf_remember();
    choices_save();
    signin_close();
    browser_top();
    set_status("Signed in to %s as %s.", S.px.server_name, S.px.user_name);
}

/* Jellyfin server k of the saved ones */
static int use_jf(int k)
{
    plex_ctx c = S.px;
    if (k < 0 || k >= S.njf)
        return -1;
    c.kind = SRV_JELLYFIN;
    c.local = 1;
    snprintf(c.base, sizeof(c.base), "%s", S.jf[k].base);
    snprintf(c.token, sizeof(c.token), "%s", S.jf[k].token);
    snprintf(c.server_name, sizeof(c.server_name), "%s", S.jf[k].name);
    snprintf(c.server_id, sizeof(c.server_id), "%s", S.jf[k].id);
    snprintf(c.user_id, sizeof(c.user_id), "%s", S.jf[k].uid);
    snprintf(c.user_name, sizeof(c.user_name), "%s", S.jf[k].uname);
    server_changed();
    server_take(&c);
    choices_save();
    browser_top();
    return 0;
}

/* The Jellyfin page of the sign-in, with the address of the server in
   use (or the last one) to start from */
static void jf_add(void)
{
    if (!*S.jf_addr) {
        const char *b = S.px.kind == SRV_JELLYFIN ? S.px.base : S.njf ? S.jf[S.njf - 1].base : "";
        snprintf(S.jf_addr, sizeof(S.jf_addr), "%s", b);
    }
    S.jf_secret[0] = 0;
    signin_show(SI_JF);
}

/* ---- DLNA servers ------------------------------------------------------------------ */

/* The server in use (a DLNA one) kept, the same server replaced */
static void dl_remember(void)
{
    dl_saved *d = NULL;
    for (int i = 0; i < S.ndl; i++)
        if (!strcmp(S.dl[i].udn, S.px.server_id))
            d = &S.dl[i];
    if (!d) {
        if (S.ndl >= DL_SAVED) {        /* the oldest makes room */
            memmove(&S.dl[0], &S.dl[1], (DL_SAVED - 1) * sizeof(dl_saved));
            S.ndl--;
        }
        d = &S.dl[S.ndl++];
    }
    snprintf(d->name, sizeof(d->name), "%s", S.px.server_name);
    snprintf(d->udn, sizeof(d->udn), "%s", S.px.server_id);
    snprintf(d->loc, sizeof(d->loc), "%s", S.px.base);
    no_bars(d->name);
}

/* The servers on the network looked for (2 s), and the page drawn again */
static void dl_search(void)
{
    char err[256];
    S.si_status[0] = 0;
    hourglass(1);
    S.nfound = dlna_discover(S.found, DLNA_FOUND, 2000, err, sizeof(err));
    hourglass(0);
    S.searched = 1;
    if (S.nfound < 0) {
        S.nfound = 0;
        snprintf(S.si_status, sizeof(S.si_status), "Can't look: %s", err);
    } else if (!S.nfound) {
        snprintf(S.si_status, sizeof(S.si_status), "Type its address below.");
    } else {
        snprintf(S.si_status, sizeof(S.si_status), "Found %d.", S.nfound);
    }
    if (S.field >= si_fields())
        S.field = 0;
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
}

/* A DLNA server (a description's address, or a host and port) made the
   one in use, then its home page */
static void dl_use(const char *addr)
{
    plex_ctx c = S.px;
    int e;
    hourglass(1);
    e = dlna_connect(&c, addr);
    hourglass(0);
    if (e != 0) {
        si_set_status("%s", c.err);
        return;
    }
    server_changed();
    server_take(&c);
    dl_remember();
    choices_save();
    signin_close();
    browser_top();
    set_status("%s: a DLNA server.", S.px.server_name);
}

/* DLNA server k of the saved ones */
static int use_dl(int k)
{
    plex_ctx c = S.px;
    if (k < 0 || k >= S.ndl)
        return -1;
    c.kind = SRV_DLNA;
    c.local = 1;
    snprintf(c.base, sizeof(c.base), "%s", S.dl[k].loc);
    snprintf(c.server_name, sizeof(c.server_name), "%s", S.dl[k].name);
    snprintf(c.server_id, sizeof(c.server_id), "%s", S.dl[k].udn);
    c.token[0] = c.user_id[0] = c.user_name[0] = 0;
    c.ctl[0] = c.svc[0] = 0;        /* its description read again (the address may have changed) */
    server_changed();
    server_take(&c);
    choices_save();
    browser_top();
    return 0;
}

/* The DLNA page of the sign-in, and a look for servers */
static void dl_add(void)
{
    if (!*S.dl_addr && S.px.kind == SRV_DLNA) {
        const char *h = strstr(S.px.base, "://");
        h = h ? h + 3 : S.px.base;
        snprintf(S.dl_addr, sizeof(S.dl_addr), "%.*s", (int)strcspn(h, "/"), h);
    }
    S.nfound = 0;
    S.searched = 0;
    signin_show(SI_DLNA);
    dl_search();
}

static void sign_out(void)
{
    plex_ctx *c = &S.px;
    builtin_stop(0);
    if (S.libs.n || S.libs.v)
        plex_list_free(&S.libs);    /* no tabs until signed in again */
    if (c->kind == SRV_JELLYFIN) {
        /* that Jellyfin server forgotten (its token is no use now); the
           Plex account and the other servers kept */
        hourglass(1);
        jf_logout(c);
        hourglass(0);
        for (int i = 0; i < S.njf; i++)
            if (!strcmp(S.jf[i].id, c->server_id) && !strcmp(S.jf[i].uid, c->user_id)) {
                memmove(&S.jf[i], &S.jf[i + 1], (S.njf - i - 1) * sizeof(jf_saved));
                S.njf--;
                break;
            }
        c->base[0] = c->token[0] = c->server_name[0] = c->server_id[0] = 0;
        c->user_id[0] = c->user_name[0] = 0;
        c->kind = SRV_PLEX;
    } else if (c->kind == SRV_DLNA) {
        /* that DLNA server forgotten (where you got to in its videos is kept,
           for if it's added again) */
        for (int i = 0; i < S.ndl; i++)
            if (!strcmp(S.dl[i].udn, c->server_id)) {
                memmove(&S.dl[i], &S.dl[i + 1], (S.ndl - i - 1) * sizeof(dl_saved));
                S.ndl--;
                break;
            }
        c->base[0] = c->server_name[0] = c->server_id[0] = c->ctl[0] = c->svc[0] = 0;
        c->kind = SRV_PLEX;
    } else {
        c->account_token[0] = c->base[0] = c->token[0] = c->server_name[0] = c->server_id[0] = 0;
        S.nservers = 0;
        S.nusers = -1;
        S.user_uuid[0] = S.user_title[0] = 0;
    }
    if (S.browser_open)
        close_window(S.browser_w);
    S.browser_open = 0;
    list_forget();
    S.nhist = 0;
    S.page = PG_GRID;
    if (S.have_det)
        plex_list_free(&S.det);
    S.have_det = 0;
    cache_free_all();
    choices_save();
}

/* Forget a server (from Forget a server: FG_*), asked first. The one in
   use goes as Sign out does it; another is dropped from the list (a
   Jellyfin one signed out of on the server, if it answers). */
static void forget_server(int what)
{
    char q[300], name[80];
    int k = what >= FG_DL(0) ? what - FG_DL(0) : what >= FG_JF(0) ? what - FG_JF(0) : 0;
    int in_use;
    if (what == FG_PLEX) {
        in_use = S.px.kind == SRV_PLEX;
        snprintf(q, sizeof(q), "Forget Plex? Matinee signs out of your Plex account and forgets its servers; "
                 "you'll need a new code from plex.tv/link (or a server's token) to add it again.");
    } else if (what >= FG_DL(0)) {
        if (k >= S.ndl)
            return;
        in_use = S.px.kind == SRV_DLNA && !strcmp(S.dl[k].udn, S.px.server_id);
        latin1(S.dl[k].name, name, sizeof(name));
        snprintf(q, sizeof(q), "Forget %s? Matinee forgets this DLNA server; add it again with Add a server. "
                 "(Where you got to in its videos is kept.)", name);
    } else {
        if (k >= S.njf)
            return;
        in_use = S.px.kind == SRV_JELLYFIN && !strcmp(S.jf[k].id, S.px.server_id) && !strcmp(S.jf[k].uid, S.px.user_id);
        latin1(S.jf[k].name, name, sizeof(name));
        snprintf(q, sizeof(q), "Forget %s? Matinee signs out of this Jellyfin server and forgets it; you'll need "
                 "to sign in to it again (Add a server).", name);
    }
    if (!ask(q))
        return;                     /* not confirmed: nothing forgotten */
    if (in_use) {
        sign_out();
        return;
    }
    if (what == FG_PLEX) {          /* the server in use is another kind: it stays */
        S.px.account_token[0] = 0;
        S.nservers = 0;
        S.nusers = -1;
        S.user_uuid[0] = S.user_title[0] = 0;
        set_status("Signed out of Plex.");
    } else if (what >= FG_DL(0)) {
        snprintf(name, sizeof(name), "%s", S.dl[k].name);
        memmove(&S.dl[k], &S.dl[k + 1], (S.ndl - k - 1) * sizeof(dl_saved));
        S.ndl--;
        set_status("Forgot %s.", name);
    } else {
        plex_ctx c = S.px;          /* its token is no use after: signed out on the server too */
        c.kind = SRV_JELLYFIN;
        snprintf(c.base, sizeof(c.base), "%s", S.jf[k].base);
        snprintf(c.token, sizeof(c.token), "%s", S.jf[k].token);
        hourglass(1);
        jf_logout(&c);
        hourglass(0);
        snprintf(name, sizeof(name), "%s", S.jf[k].name);
        memmove(&S.jf[k], &S.jf[k + 1], (S.njf - k - 1) * sizeof(jf_saved));
        S.njf--;
        set_status("Forgot %s.", name);
    }
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
    } else if (S.njf) {
        use_jf(S.njf - 1);
    } else if (S.ndl) {
        use_dl(S.ndl - 1);
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
    if (how == PLAY_RESUME && S.px.kind != SRV_DLNA)
        allow = 0;                  /* (a DLNA server's converted stream can't start part way: Reel's
                                       own "carry on" for the file itself instead) */
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
static void psubs_menu_open(void);
static void builtin_subs_apply(void);
static void builtin_subs_forget(void);

#define TIMELINE_CS 1000
#define PING_CS     3000            /* paused: the server's converting kept alive */
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

/* Plays S.pl.det's video from t seconds. A converted stream always starts
   at the beginning and the player seeks in it (the server's playlist
   covers the whole video, and converting starts from the segment asked
   for), under one session id for the whole playback. new_session: a new
   conversion (another sound track), the one before stopped. */
static int builtin_open_at(double t, int new_session)
{
    const plex_item *it = &S.pl.det.v[0];
    caps_t k;
    player_src src;
    char title[200], headers[1400];
    int e;
    if (new_session || !*S.pl.sid) {
        if (*S.pl.sid && !S.pl.p.direct)
            plex_transcode_stop(&S.px, S.pl.sid);
        caps_session_id(&S.px, S.pl.sid, sizeof(S.pl.sid));
    }
    caps_for(S.quality, &k);
    k.own_subs = 1;                 /* reelcore draws them, playing the file itself */
    k.hevc = player_hevc_block();   /* and HEVC on the Pi 4's HEVC block */
    if (caps_play_at(&S.px, it, &k, S.direct, 0, S.pl.sid, &S.pl.p) != 0) {
        set_status("%s", S.pl.p.why);
        return -1;
    }
    media_headers(S.pl.p.headers, headers, sizeof(headers));
    /* the stream and the timeline under one session id: how the server
       ties them together (the dashboard's Now Playing); Jellyfin's is the
       one its PlaybackInfo gave */
    snprintf(S.pl.sid, sizeof(S.pl.sid), "%s", S.pl.p.session);
    snprintf(S.pl.pq.session, sizeof(S.pl.pq.session), "%s", S.pl.p.session);
    S.pl.pq.direct = S.pl.p.direct;
    if (S.px.kind == SRV_PLEX && strlen(headers) + strlen(S.pl.p.session) + 40 < sizeof(headers))
        snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers), "X-Plex-Session-Identifier: %s\r\n",
                 S.pl.p.session);
    builtin_title(it, title, sizeof(title));
    memset(&src, 0, sizeof(src));
    src.url = S.pl.p.url;
    src.headers = headers;
    src.user_agent = S.agent;
    src.title = title;
    src.convert = !S.pl.p.direct;
    src.base = 0;
    src.start = t;
    src.duration = it->duration_ms / 1000.0;
    src.subs = it->nsubs;
    {
        static double ch[100];
        int n = 0;
        for (int i = 0; i < it->nchapters && n < 100; i++)
            ch[n++] = it->chapters[i].start_ms / 1000.0;
        src.chapters = ch;
        src.nchapters = n;
    }
    snprintf(S.play_why, sizeof(S.play_why), "%s", S.pl.p.why);
    set_status("%s", S.pl.p.why);
    e = player_open(&src, S.browser_w);
    builtin_subs_forget();          /* the last reelcore's files: this one is given its own */
    if (e != 0) {
        report("Can't play %s: %s", title, player_error());
        return -1;
    }
    S.pl.tl_cs = now_cs() + TIMELINE_CS;
    S.pl.ping_cs = now_cs() + PING_CS;
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
    int was_mini = player_mini();
    if (!S.pl.on)
        return;
    if (player_ready() && !player_ended())
        builtin_timeline("stopped");
    if (!S.pl.p.direct && *S.pl.sid)
        plex_transcode_stop(&S.px, S.pl.sid);
    S.pl.sid[0] = 0;
    player_close();
    builtin_subs_forget();
    if (was_mini)
        choices_save();             /* where the mini player was left */
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
    int st[9], prev = S.page == PG_PLAYER ? S.pl.prev_page : S.page;
    int was_mini = player_mini();   /* the next episode stays in the mini player */
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
    /* the window keeps its size: the picture fits in it */
    if (!was_mini) {
        open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
        set_caret(S.browser_w, -1, NULL);
    }
    t = how != PLAY_START && d.v[0].view_offset_ms > 0 ? d.v[0].view_offset_ms / 1000.0 : 0;
    S.pl.sid[0] = 0;                /* a new playback */
    if (builtin_open_at(t, 1) != 0) {
        builtin_stop(1);
        if (was_mini)
            browser_open();
        return;
    }
    if (was_mini)
        player_set_mini(1);
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
    if (S.pl.upnext_cs)             /* already offered (Skip credits), then the end */
        return;
    player_skip(NULL);
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

/* Plex's markers: Skip intro or Skip credits while one is playing (not in
   its last second, so a skip to its end doesn't show it again) */
static void builtin_skip_update(void)
{
    const plex_item *it = &S.pl.det.v[0];
    int64_t t = (int64_t)(player_position() * 1000);
    int m = plex_marker_at(it, t);
    if (m >= 0 && t >= it->markers[m].end_ms - 1000)
        m = -1;
    player_skip(m < 0 ? NULL : it->markers[m].type == PM_INTRO ? "Skip intro" : "Skip credits");
}

/* Skip: to the intro's end; past credits that run to the end, the end
   (watched, and Up next for an episode) */
static void builtin_skip(void)
{
    const plex_item *it = &S.pl.det.v[0];
    const plex_marker *k;
    int m = plex_marker_at(it, (int64_t)(player_position() * 1000));
    if (m < 0)
        return;
    k = &it->markers[m];
    player_skip(NULL);
    if (k->type == PM_CREDITS && (k->final || (it->duration_ms > 0 && k->end_ms >= it->duration_ms - 5000))) {
        builtin_ended();
        return;
    }
    player_seek(k->end_ms / 1000.0);
}

/* Page Up and Page Down: the chapter before (or this one's start, if it's
   more than 3 s in) and the next */
static void builtin_chapter_step(int d)
{
    const plex_item *it = &S.pl.det.v[0];
    double t = player_position();
    int c = plex_chapter_at(it, (int64_t)(t * 1000));
    if (!it->nchapters)
        return;
    if (d > 0)
        c = c + 1;
    else if (c >= 0 && t - it->chapters[c].start_ms / 1000.0 < 3)
        c = c - 1;
    if (c < 0)
        c = 0;
    if (c >= it->nchapters)
        return;
    player_seek(it->chapters[c].start_ms / 1000.0);
}

/* What the player said */
static void builtin_event(int e)
{
    switch (e) {
    case PE_READY:
        builtin_subs_apply();
        builtin_timeline("playing");
        break;
    case PE_PAUSED:
        builtin_timeline("paused");
        break;
    case PE_PLAYING:
        builtin_timeline("playing");
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
    case PE_SUBS:
        psubs_menu_open();
        break;
    case PE_SKIP:
        builtin_skip();
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
    if (player_ready() && !player_ended())
        builtin_skip_update();
    if (player_ready() && !player_paused() && !player_ended() && now_cs() - S.pl.tl_cs >= 0)
        builtin_timeline("playing");
    /* paused in a converted stream: the server stops converting for a
       client it hasn't heard from, so it's told the session is wanted */
    if (player_paused() && !S.pl.p.direct && now_cs() - S.pl.ping_cs >= 0) {
        plex_transcode_ping(&S.px, S.pl.sid);
        S.pl.ping_cs = now_cs() + PING_CS;
    }
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
    {
        const char *bs = it->part_file ? strrchr(it->part_file, '\\') : NULL;
        if (bs && (!leaf || bs > leaf))         /* a Windows server's path (Jellyfin's) */
            leaf = bs;
    }
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

/* A file's address: a key (a path on the server), or an address of its
   own (a DLNA server's files) */
static void media_url(const char *key, char *url, size_t size)
{
    if (!strncmp(key, "http://", 7) || !strncmp(key, "https://", 8))
        snprintf(url, size, "%s", key);
    else
        snprintf(url, size, "%s%s", S.px.base, key);
}

static void save_start(const char *path)
{
    char url[1100], headers[1024], err[256];
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
    media_url(S.sv_url_key, url, sizeof(url));
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
    char url[1100], headers[1024], err[256];
    int t;
    if (S.save.active || S.speed.active) {
        report("Wait until the file being saved, or the speed test, has finished.");
        return;
    }
    if (!it || it->kind != PI_VIDEO || !it->part_key || !*it->part_key)
        return;
    media_url(it->part_key, url, sizeof(url));
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

static menu_t m_bar, m_servers, m_forget, m_player, m_quality, m_size, m_item, m_subs;
static menu_t m_play, m_audio, m_vol, m_pic, m_psubs, m_chap, m_users, m_pin, m_rate;

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
    /* the Plex account's servers, then the Jellyfin ones signed in to, then
       Add a server (S.srv_map says which an item is) */
    menu_begin(&m_servers, "Servers");
    for (int i = 0; i < S.nservers && m_servers.n < 9; i++) {
        S.srv_map[m_servers.n] = i;
        menu_add(&m_servers, S.servers[i].name, S.px.kind == SRV_PLEX && !strcmp(S.servers[i].id, S.px.server_id),
                 0, -1, 0);
    }
    if (signed_in && !S.nservers) {
        S.srv_map[m_servers.n] = SRV_ADD - 1;
        menu_add(&m_servers, "(no Plex servers found)", 0, 1, -1, 0);
    }
    if (m_servers.n)
        m_servers.m.item[m_servers.n - 1].flags |= 2;   /* a line under Plex's */
    for (int k = 0; k < S.njf && m_servers.n < 15; k++) {
        char t[80];
        snprintf(t, sizeof(t), "%s (Jellyfin)", S.jf[k].name);
        S.srv_map[m_servers.n] = -1 - k;
        menu_add(&m_servers, t, S.px.kind == SRV_JELLYFIN && !strcmp(S.jf[k].id, S.px.server_id) &&
                 !strcmp(S.jf[k].uid, S.px.user_id), 0, -1, 0);
    }
    if (S.njf && m_servers.n)
        m_servers.m.item[m_servers.n - 1].flags |= 2;
    for (int k = 0; k < S.ndl && m_servers.n < 15; k++) {
        char t[80];
        snprintf(t, sizeof(t), "%s (DLNA)", S.dl[k].name);
        S.srv_map[m_servers.n] = SRV_DL(k);
        menu_add(&m_servers, t, S.px.kind == SRV_DLNA && !strcmp(S.dl[k].udn, S.px.server_id), 0, -1, 0);
    }
    if (S.ndl && m_servers.n)
        m_servers.m.item[m_servers.n - 1].flags |= 2;
    S.srv_map[m_servers.n] = SRV_ADD;
    menu_add(&m_servers, "Add a server...", 0, 0, -1, 0);
    /* Forget a server: each that Matinee keeps (asked first when chosen) */
    menu_begin(&m_forget, "Forget");
    if (signed_in || (S.px.kind == SRV_PLEX && *S.px.base)) {
        S.fg_map[m_forget.n] = FG_PLEX;
        menu_add(&m_forget, signed_in ? "Plex (your sign-in)" : "Plex (the server given by hand)", 0, 0, -1, 0);
    }
    for (int k = 0; k < S.njf && m_forget.n < 15; k++) {
        char t[140];
        snprintf(t, sizeof(t), "%s (Jellyfin, %s)", S.jf[k].name, S.jf[k].uname);
        S.fg_map[m_forget.n] = FG_JF(k);
        menu_add(&m_forget, t, 0, 0, -1, 0);
    }
    for (int k = 0; k < S.ndl && m_forget.n < 16; k++) {
        char t[80];
        snprintf(t, sizeof(t), "%s (DLNA)", S.dl[k].name);
        S.fg_map[m_forget.n] = FG_DL(k);
        menu_add(&m_forget, t, 0, 0, -1, 0);
    }
    menu_end(&m_forget);
    S.srv_map[m_servers.n] = SRV_FORGET;
    menu_add(&m_servers, "Forget a server", 0, !m_forget.n, m_forget.n ? (int)(intptr_t)&m_forget.m : -1, 0);
    menu_end(&m_servers);
    /* Plex Home: its people; one with a PIN has a writable PIN item to its right */
    menu_begin(&m_pin, "PIN");
    menu_add(&m_pin, "", 0, 0, -1, 0);
    m_pin.m.item[0].flags |= 4;                 /* writable */
    m_pin.m.item[0].data[1] = (int)(intptr_t)"A0-9;D*";
    m_pin.m.item[0].iflags |= 0x07000000u;
    m_pin.m.width = 200;
    m_pin.text[0][0] = 0;
    menu_end(&m_pin);
    menu_begin(&m_users, "Who's watching");
    for (int i = 0; i < S.nusers && i < 15; i++) {
        char t[80];
        int cur = !strcmp(S.users[i].uuid, S.user_uuid);
        snprintf(t, sizeof(t), "%s%s", S.users[i].title, S.users[i].protected_ && !cur ? " (PIN)" : "");
        menu_add(&m_users, t, cur, 0, S.users[i].protected_ && !cur ? (int)(intptr_t)&m_pin.m : -1, 0);
    }
    if (S.nusers <= 0)
        menu_add(&m_users, "(no Plex Home)", 0, 1, -1, 0);
    menu_end(&m_users);
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
    menu_add(&m_bar, "Add a server...", 0, 0, -1, 0);
    menu_add(&m_bar, "Servers", 0, 0, (int)(intptr_t)&m_servers.m, 0);
    {   /* Plex Home's people: for a Plex server */
        int users = S.nusers >= 2 && S.px.kind == SRV_PLEX;
        menu_add(&m_bar, "Switch user", 0, !users, users ? (int)(intptr_t)&m_users.m : -1, 0);
    }
    menu_add(&m_bar, "Player", 0, 0, (int)(intptr_t)&m_player.m, 0);
    menu_add(&m_bar, "Quality", 0, 0, (int)(intptr_t)&m_quality.m, 0);
    menu_add(&m_bar, "Poster size", 0, 0, (int)(intptr_t)&m_size.m, 0);
    menu_add(&m_bar, "Direct play when possible", S.direct, 0, -1, 1);
    {
        char t[64];
        int n;
        long long b = imgcache_size(&n);
        snprintf(t, sizeof(t), "Clear image cache (%.1f MB)", b / 1048576.0);
        menu_add(&m_bar, t, 0, n == 0, -1, 0);
    }
    menu_add(&m_bar, S.px.kind == SRV_JELLYFIN ? "Sign out of this server" :
                     S.px.kind == SRV_DLNA ? "Forget this server" : "Sign out", 0,
             !signed_in && !*S.px.base, -1, 0);
    menu_add(&m_bar, "Quit", 0, 0, -1, 0);
    menu_end(&m_bar);
}

static void bar_menu_open(int x)
{
    if (*S.px.account_token && !S.nservers)
        servers_get();              /* for the Servers submenu */
    if (*S.px.account_token && S.nusers < 0)
        users_get();                /* and Switch user */
    bar_menu_build();
    S.menu_kind = 1;
    S.menu_x = x;
    open_menu(&m_bar, x - 64, 96 + MB_COUNT * 44);
}

static const plex_item *sel_item(void)
{
    return S.have_list && S.sel >= 0 && S.sel < S.list.n ? &S.list.v[S.sel] : NULL;
}

/* What the item menu is for: the selected poster, or on a details page
   opened from More like this (not one of the list's), that video */
static int menu_rel(void)
{
    return S.page == PG_DETAILS && S.det_i < 0 && det_item() != NULL;
}

static const plex_item *menu_item(void)
{
    return menu_rel() ? det_item() : sel_item();
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

static menu_t m_sort;

/* The library bar's buttons and letters */
static void lib_action(int id, int x, int y)
{
    char path[256];
    if (id >= LB_VIEW && id < LB_VIEW + 3 && id - LB_VIEW != S.lib.view) {
        lib_path(id - LB_VIEW, 0, 0, path, sizeof(path));
        show_list(path, "", 0, 0);
    } else if (id == LB_UNWATCHED) {
        lib_path(0, S.lib.sort, !S.lib.unwatched, path, sizeof(path));
        show_list(path, "", 0, 0);
    } else if (id == LB_SORT) {
        menu_begin(&m_sort, "Sort by");
        for (int k = 0; k < SORTS; k++)
            menu_add(&m_sort, sort_names[k], k == S.lib.sort, 0, -1, 0);
        menu_end(&m_sort);
        S.menu_kind = 6;
        S.menu_x = x;
        S.menu_y = y;
        open_menu(&m_sort, x - 64, y);
    } else if (id >= LB_AZ && id < LB_AZ + 27 && S.lib.az_first[id - LB_AZ] >= 0) {
        select_tile(S.lib.az_first[id - LB_AZ]);    /* (scrolled into view) */
    }
}

/* Is it in Continue watching (part watched, or the list is Continue watching)? */
static int in_continue(const plex_item *it)
{
    int c;
    if (it->view_offset_ms > 0 || !strncmp(S.path, "/library/onDeck", 15))
        return 1;
    return S.home.on && S.sel >= 0 && home_row_of(S.sel, &c) >= 0 &&
           S.home.row[home_row_of(S.sel, &c)].kind == PR_CONTINUE;
}

static void item_menu_build(void)
{
    const plex_item *it = menu_item();
    int video = it && it->kind == PI_VIDEO, folder = it && it->kind == PI_FOLDER, rel = menu_rel();
    char t[80], when[32], head[80];
    save_prepare(it);
    /* a video's subtitle tracks are only in its details */
    if (video && !det_is(it))
        det_fetch(it, 0);
    if (video && det_is(it))
        subs_menu_build();
    if (it && !rel && S.disp)
        fit_disp(S.sel);
    if (rel)
        latin1(it->title, head, sizeof(head));
    else
        snprintf(head, sizeof(head), "%s", it && S.disp ? S.disp[S.sel].line[0] : APP);
    menu_begin(&m_item, head);
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
    menu_add(&m_item, "Mark unwatched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 0);
    {
        /* yours: the details' if they're this one's (lists may leave it out) */
        double ur = it && det_is(it) ? det_item()->user_rating : it ? it->user_rating : 0;
        int can = it && it->rating_key && it->type && S.px.kind == SRV_PLEX &&
                  (!strcmp(it->type, "movie") || !strcmp(it->type, "episode") || !strcmp(it->type, "show") ||
                   !strcmp(it->type, "season"));
        menu_begin(&m_rate, "Rate");
        menu_add(&m_rate, "No rating", ur <= 0, 0, -1, 1);
        for (int k = 1; k <= 5; k++) {
            char t[16];
            snprintf(t, sizeof(t), "%d star%s", k, k == 1 ? "" : "s");
            menu_add(&m_rate, t, ur > 2 * k - 1 && ur < 2 * k + 1, 0, -1, 0);
        }
        menu_end(&m_rate);
        menu_add(&m_item, "Rate", 0, !can, can ? (int)(intptr_t)&m_rate.m : -1, 0);
    }
    menu_add(&m_item, "Remove from Continue watching", 0, !it || !it->rating_key || it->kind != PI_VIDEO || !in_continue(it),
             -1, 1);
    menu_add(&m_item, "Back", 0, S.nhist == 0, -1, 0);
    menu_add(&m_item, "Refresh", 0, 0, -1, 0);
    menu_end(&m_item);
}

/* The playing video's subtitles: None, then its tracks (the server's
   list: the names are Plex's) */
static void psubs_menu_build(void)
{
    const plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    char t[160];
    menu_begin(&m_psubs, "Subtitles");
    menu_add(&m_psubs, "None", !it || plex_sub_selected(it) < 0, 0, -1, it && it->nsubs);
    for (int i = 0; it && i < it->nsubs && i < 15; i++) {
        const plex_sub *sb = &it->subs[i];
        snprintf(t, sizeof(t), "%s%s", sb->title, sb->forced && !strstr(sb->title, "orced") ? " (forced)" : "");
        menu_add(&m_psubs, t, sb->selected, 0, -1, 0);
    }
    menu_end(&m_psubs);
}

/* Subtitles on the bar: the menu on its own, by the pointer */
static void psubs_menu_open(void)
{
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r))
        p[0] = S.scr_w / 2, p[1] = S.scr_h / 2;
    psubs_menu_build();
    S.menu_kind = 5;
    S.menu_x = p[0];
    S.menu_y = p[1];
    open_menu(&m_psubs, p[0] - 64, p[1] + 44 * (m_psubs.n + 1));
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
    psubs_menu_build();
    /* chapters: 16 at most, around the one playing */
    menu_begin(&m_chap, "Chapters");
    {
        int nc = it ? it->nchapters : 0, cur = it ? plex_chapter_at(it, (int64_t)(player_position() * 1000)) : -1;
        int first = cur - 7;
        if (first > nc - 16)
            first = nc - 16;
        if (first < 0)
            first = 0;
        S.pl.chap_first = first;
        for (int i = first; i < nc && i < first + 16; i++) {
            char tm[16], u[200];
            int s2 = (int)(it->chapters[i].start_ms / 1000);
            if (s2 >= 3600)
                snprintf(tm, sizeof(tm), "%d:%02d:%02d", s2 / 3600, s2 / 60 % 60, s2 % 60);
            else
                snprintf(tm, sizeof(tm), "%d:%02d", s2 / 60, s2 % 60);
            snprintf(u, sizeof(u), "%s  %s", tm, it->chapters[i].title);
            latin1(u, t, 60);
            menu_add(&m_chap, t, i == cur, 0, -1, 0);
        }
        if (!nc)
            menu_add(&m_chap, "(none)", 0, 1, -1, 0);
    }
    menu_end(&m_chap);
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
    menu_add(&m_play, "Subtitles", 0, !it || !it->nsubs, it && it->nsubs ? (int)(intptr_t)&m_psubs.m : -1, 0);
    menu_add(&m_play, "Chapters", 0, !it || !it->nchapters, it && it->nchapters ? (int)(intptr_t)&m_chap.m : -1, 0);
    menu_add(&m_play, "Volume", 0, 0, (int)(intptr_t)&m_vol.m, 0);
    menu_add(&m_play, "Picture", 0, 0, (int)(intptr_t)&m_pic.m, 1);
    menu_add(&m_play, "Stats", player_stats(), 0, -1, 0);
    menu_add(&m_play, "Full screen", player_fullscreen(), 0, -1, 0);
    menu_add(&m_play, "Mini player", player_mini(), 0, -1, 0);
    menu_add(&m_play, "Keep on top", player_ontop(), 0, -1, 1);
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
    if (builtin_open_at(player_position(), 1) != 0)
        builtin_stop(1);
}

/* A subtitle file beside the video (Plex's /library/streams/<id>): fetched
   into the scrap directory for the player. 0 = done, path set. */
static int sub_fetch(const plex_sub *sb, char *path, size_t size)
{
    const char *scrap = getenv("Wimp$ScrapDir");
    char dir[256], url[1024], hd[1400], h2[1400];
    net_buf b;
    FILE *f;
    int ok;
    if (!scrap || !*scrap || !sb->key)
        return -1;
    snprintf(dir, sizeof(dir), "%s" SEP APP, scrap);
    make_dir(dir);
    snprintf(path, size, "%s" SEP "Sub%ld", dir, sb->id);
    media_url(sb->key, url, sizeof(url));
    plex_headers(&S.px, S.px.token, hd, sizeof(hd));
    media_headers(hd, h2, sizeof(h2));
    hourglass(1);
    ok = net_fetch(url, h2, NULL, &b, 15000, S.px.err, sizeof(S.px.err)) == 0;
    hourglass(0);
    if (!ok)
        return -1;
    ok = (f = fopen(path, "wb")) != NULL;
    if (ok) {
        ok = fwrite(b.data, 1, b.len, f) == b.len;
        ok &= fclose(f) == 0;
    }
    net_buf_free(&b);
    if (!ok)
        snprintf(S.px.err, sizeof(S.px.err), "can't write %s", path);
    return ok ? 0 : -1;
}

/* The files given to the player, gone with it */
static void builtin_subs_forget(void)
{
    for (int i = 0; i < S.pl.n_ext; i++)
        remove(S.pl.ext[i].path);
    S.pl.n_ext = 0;
}

/* Playing the file itself: the player shows the subtitles chosen on the
   server (or none), as Plex's apps do. A track in the file is reelcore's
   track of the same place among the file's tracks (Plex lists them in the
   file's order, then the files beside it); a file beside it is fetched
   and added (once). */
static void builtin_subs_apply(void)
{
    const plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    const plex_sub *sb;
    int sel, j = 0, t = -1;
    char path[300];
    if (!it || !S.pl.p.direct || !player_ready())
        return;
    sel = plex_sub_selected(it);
    if (sel < 0) {
        player_set_sub(-1);
        return;
    }
    sb = &it->subs[sel];
    if (!sb->external) {
        for (int i = 0; i < sel; i++)
            j += !it->subs[i].external;
        if (j < player_sub_tracks())
            t = j;
    } else {
        for (int i = 0; i < S.pl.n_ext; i++)
            if (S.pl.ext[i].id == sb->id)
                t = S.pl.ext[i].track;
        if (t < 0 && sub_fetch(sb, path, sizeof(path)) == 0) {
            t = player_add_sub_file(path);
            if (t >= 0 && S.pl.n_ext < 8) {
                S.pl.ext[S.pl.n_ext].id = sb->id;
                S.pl.ext[S.pl.n_ext].track = t;
                snprintf(S.pl.ext[S.pl.n_ext].path, sizeof(S.pl.ext[0].path), "%s", path);
                S.pl.n_ext++;
            } else {
                remove(path);
            }
        }
    }
    if (t < 0 || player_set_sub(t) != 0)
        player_note("Can't show those subtitles");
}

/* Subtitles menu choice k (the player's): 0 None, else track k-1. Kept on
   the server; the file itself: the player shows them; a converted stream
   (or subtitles the player can't read): started again from here, the
   server burning them in */
static void choose_psub(int k)
{
    plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    char name[120], t[160];
    long id;
    if (!it || k < 0 || k > it->nsubs || k - 1 == plex_sub_selected(it))
        return;
    id = k ? it->subs[k - 1].id : 0;
    hourglass(1);
    if (plex_set_subtitle(&S.px, it, id) != 0) {
        hourglass(0);
        report("Can't choose the subtitles: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; i < it->nsubs; i++)
        it->subs[i].selected = i == k - 1;
    latin1(k ? it->subs[k - 1].title : "", name, sizeof(name));
    if (S.pl.p.direct && (!k || caps_sub_own(&it->subs[k - 1]))) {
        builtin_subs_apply();
        snprintf(t, sizeof(t), k ? "Subtitles: %s" : "Subtitles off", name);
        player_note(t);
        return;
    }
    player_note(k ? "Changing the subtitles..." : "Subtitles off...");
    if (builtin_open_at(player_position(), 1) != 0)
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
    {
        int own = k && caps_sub_own(&it->subs[k - 1]);      /* before det_refresh lets it go */
        det_refresh();
        it = NULL;
        if (own && S.player == PLAYER_BUILTIN)
            set_status("Subtitles: %s.", name);
        else if (k)
            set_status("Subtitles: %s. The server burns them into the picture.", name);
        else
            set_status("Subtitles off.");
    }
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

/* Your rating (0..10, 2 a star; -1 none), on the server, and wherever it's shown */
static void rate(const plex_item *it, int r)
{
    char key[64];
    if (!it->rating_key)
        return;
    snprintf(key, sizeof(key), "%s", it->rating_key);     /* it may be in a list redrawn below */
    hourglass(1);
    if (plex_rate(&S.px, it, r) != 0) {
        hourglass(0);
        report("Can't rate it: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; S.have_list && i < S.list.n; i++)
        if (S.list.v[i].rating_key && !strcmp(S.list.v[i].rating_key, key))
            S.list.v[i].user_rating = r > 0 ? r : 0;
    if (S.have_det && S.det.n && S.det.v[0].rating_key && !strcmp(S.det.v[0].rating_key, key)) {
        S.det.v[0].user_rating = r > 0 ? r : 0;
        det_repaint();
    }
    if (r > 0)
        set_status("Rated %d star%s.", r / 2, r == 2 ? "" : "s");
    else
        set_status("Rating taken away.");
}

/* ---- Plex Home: who's watching ------------------------------------------------- */

/* The account's people (once; again after a switch). -1: not a Plex Home, or plex.tv didn't answer */
static int users_get(void)
{
    int n;
    if (!*S.px.account_token)
        return S.nusers = 0;
    hourglass(1);
    n = plex_home_users(&S.px, S.users, PLEX_USERS);
    hourglass(0);
    S.nusers = n < 0 ? 0 : n;
    if (n > 0 && !*S.user_uuid)                 /* signed in as the account's owner */
        for (int i = 0; i < n; i++)
            if (S.users[i].admin) {
                snprintf(S.user_uuid, sizeof(S.user_uuid), "%s", S.users[i].uuid);
                snprintf(S.user_title, sizeof(S.user_title), "%s", S.users[i].title);
            }
    return n;
}

/* Switches to user i (pin: theirs, or ""): their account token, then the
   same server with their token for it (or another of theirs), and the
   home page again */
static void switch_user(int i, const char *pin)
{
    plex_ctx c = S.px;
    char srv[64];
    int e, k;
    if (i < 0 || i >= S.nusers)
        return;
    if (S.users[i].protected_ && !*pin) {
        report("%s has a PIN: move to the right of the name and type it, then press Return.", S.users[i].title);
        return;
    }
    builtin_stop(1);
    hourglass(1);
    e = plex_switch_user(&c, S.users[i].uuid, pin);
    hourglass(0);
    if (e == -2) {
        report("That isn't %s's PIN.", S.users[i].title);
        return;
    }
    if (e != 0) {
        report("Can't switch to %s: %s", S.users[i].title, c.err);
        return;
    }
    snprintf(S.px.account_token, sizeof(S.px.account_token), "%s", c.account_token);
    snprintf(S.user_uuid, sizeof(S.user_uuid), "%s", S.users[i].uuid);
    snprintf(S.user_title, sizeof(S.user_title), "%s", S.users[i].title);
    snprintf(srv, sizeof(srv), "%s", S.px.server_id);
    choices_save();
    /* the server's token is per user */
    if (servers_get() <= 0)
        return;
    for (k = 0; k < S.nservers && strcmp(S.servers[k].id, srv); k++)
        ;
    if (k == S.nservers) {
        report("%s can't use %s: choose one of their servers on the Servers menu.", S.user_title,
               S.px.server_name);
        S.px.base[0] = S.px.token[0] = 0;
        choose_server();
        return;
    }
    if (use_server(k) != 0)
        return;
    if (S.libs.n || S.libs.v)
        plex_list_free(&S.libs);    /* their libraries */
    cache_free_all();
    S.nhist = 0;
    browser_top();
    set_status("Now watching as %s.", S.user_title);
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
            if (sel[1] >= 0 && sel[1] < m_servers.n) {
                int w = S.srv_map[sel[1]];
                if (w == SRV_ADD)
                    signin_open();
                else if (w == SRV_FORGET) {
                    if (sel[2] >= 0 && sel[2] < m_forget.n)
                        forget_server(S.fg_map[sel[2]]);
                } else if (w <= -1 && w > -1 - S.njf)
                    use_jf(-1 - w);
                else if (w <= SRV_DL(0) && w > SRV_DL(S.ndl))
                    use_dl(SRV_DL(0) - w);
                else if (w >= 0 && w < S.nservers && use_server(w) == 0)
                    browser_top();
            }
            break;
        case MB_USERS:
            if (S.px.kind == SRV_PLEX && sel[1] >= 0 && sel[1] < S.nusers && strcmp(S.users[sel[1]].uuid, S.user_uuid)) {
                char pin[16];
                snprintf(pin, sizeof(pin), "%s", sel[2] == 0 ? m_pin.text[0] : "");
                pin[strcspn(pin, "\r\n")] = 0;
                m_pin.text[0][0] = 0;
                switch_user(sel[1], pin);
            }
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
        case MB_CACHE: {
            long long b = imgcache_size(NULL);
            int n = imgcache_clear();
            set_status("Image cache cleared: %d picture%s (%.1f MB).", n, n == 1 ? "" : "s", b / 1048576.0);
            break;
        }
        case MB_SIGNOUT: {          /* easily chosen by mistake: asked first */
            char q[300];
            if (S.px.kind == SRV_DLNA)
                snprintf(q, sizeof(q), "Forget %s? Matinee forgets this DLNA server; add it again with Servers, Add "
                         "a server. (Where you got to in its videos is kept.)", S.px.server_name);
            else if (S.px.kind == SRV_JELLYFIN)
                snprintf(q, sizeof(q), "Sign out of %s? Matinee forgets this Jellyfin server, and you'll need to "
                         "sign in to it again (Servers, Add a server).", S.px.server_name);
            else
                snprintf(q, sizeof(q), "Sign out? Matinee forgets the Plex server and your sign-in, and you'll need "
                         "a new code from plex.tv/link (or the server's token) to sign in again.");
            if (ask(q))
                sign_out();
            break;
        }
        case MB_QUIT:
            if (S.save.active && !ask("A file is being saved. Stop saving it and quit?"))
                break;
            quit();
            return 1;
        }
    } else if (kind == 2) {
        const plex_item *it = menu_item();
        int rel = menu_rel();
        switch (sel[0]) {
        case MI_PLAY:
        case MI_RESUME:
        case MI_START: {
            int how = sel[0] == MI_RESUME ? PLAY_RESUME : sel[0] == MI_START ? PLAY_START : PLAY_DEFAULT;
            if (it && rel)
                play_item(it, how);
            else if (it)
                open_item(S.sel, how);
            break;
        }
        case MI_DETAILS:
            if (it && !rel)
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
        case MI_REMOVE:
            if (it) {
                hourglass(1);
                if (plex_remove_continue(&S.px, it) != 0) {
                    hourglass(0);
                    report("Can't take it off Continue watching: %s", S.px.err);
                    break;
                }
                hourglass(0);
                if (S.home.on || !strncmp(S.path, "/library/onDeck", 15))
                    refresh_list(); /* it's gone from the row */
                set_status("Taken off Continue watching.");
            }
            break;
        case MI_WATCHED:
        case MI_UNWATCHED:
            if (it)
                mark(it, sel[0] == MI_WATCHED);
            break;
        case MI_RATE:
            if (it && sel[1] >= 0 && sel[1] <= 5)
                rate(det_is(it) ? det_item() : it, sel[1] ? 2 * sel[1] : -1);
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
    } else if (kind == 6 && S.lib.on) {
        if (sel[0] >= 0 && sel[0] < SORTS && sel[0] != S.lib.sort) {
            char path[256];
            lib_path(0, sel[0], S.lib.unwatched, path, sizeof(path));
            show_list(path, "", 0, 0);
        }
        return 0;
    } else if (kind == 5 && S.pl.on) {
        choose_psub(sel[0]);
    } else if (kind == 4 && S.pl.on) {
        switch (sel[0]) {
        case MP_AUDIO:
            choose_audio(sel[1]);
            break;
        case MP_SUBS:
            choose_psub(sel[1]);
            break;
        case MP_CHAPTERS:
            if (sel[1] >= 0 && S.pl.det.n && S.pl.chap_first + sel[1] < S.pl.det.v[0].nchapters)
                player_seek(S.pl.det.v[0].chapters[S.pl.chap_first + sel[1]].start_ms / 1000.0);
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
        case MP_MINI:
            player_set_mini(!player_mini());
            choices_save();
            break;
        case MP_ONTOP:
            player_set_ontop(!player_ontop());
            choices_save();
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
        } else if (kind == 5 && S.pl.on) {
            psubs_menu_build();
            S.menu_kind = 5;
            open_menu(&m_psubs, S.menu_x - 64, S.menu_y + 44 * (m_psubs.n + 1));
        }
    }
    return 0;
}

/* The home page's featured part and rows' titles */
static void home_action(int id)
{
    const plex_item *h = home_hero();
    if (id == HB_PLAY && h) {
        select_tile(S.home.row[0].start + S.home.pick);
        play_item(h, PLAY_DEFAULT);
    } else if (id == HB_DETAILS && h) {
        select_tile(S.home.row[0].start + S.home.pick);
        det_show(S.home.row[0].start + S.home.pick);
    } else if (id >= HB_DOT && id < HB_DOT + S.home.npick && id - HB_DOT != S.home.pick) {
        S.home.pick = id - HB_DOT;      /* another one featured */
        home_layout();
        hourglass(1);
        home_fetch_art();
        hourglass(0);
        force_redraw(S.browser_w, 0, -HEADER_H - S.home.hero_h, S.scr_w, -HEADER_H);
    } else if (id >= HB_ROW && id < HB_ROW + S.home.nrows && S.home.row[id - HB_ROW].path[0]) {
        char path[200], back[64];
        snprintf(path, sizeof(path), "%s", S.home.row[id - HB_ROW].path);
        snprintf(back, sizeof(back), "%s", S.list.title);
        show_list(path, back, 1, 0);
    }
}

/* A show: its page, with the series that has episodes not seen yet (or
   the first) */
static void show_open(const plex_item *it)
{
    plex_list d, se;
    char back[64], path[256];
    int k = 0, sel = 0;
    hourglass(1);
    if (plex_details(&S.px, it, &d) != 0 || !d.n) {
        hourglass(0);
        report("Can't get %s from the server: %s", it->title, S.px.err);
        return;
    }
    if (plex_list_get(&S.px, it->key, &se) != 0) {
        plex_list_free(&d);
        hourglass(0);
        report("Can't get its series: %s", S.px.err);
        return;
    }
    hourglass(0);
    plex_list_keep(&se, "season");  /* not Plex's "All episodes" */
    if (!se.n) {                    /* no series (Plex left them out) */
        plex_list_free(&d);
        plex_list_free(&se);
        snprintf(back, sizeof(back), "%s", S.list.title);
        snprintf(path, sizeof(path), "%s", it->key);
        show_list(path, back, 1, 0);
        return;
    }
    if (S.show.have) {
        plex_list_free(&S.show.det);
        plex_list_free(&S.show.seasons);
    }
    S.show.det = d;
    S.show.seasons = se;
    S.show.have = 1;
    S.show.art = S.show.poster = NULL;
    S.show.fw = 0;
    for (int i = 0; i < se.n; i++)
        if (se.v[i].unwatched > 0) {
            k = i;
            break;
        }
    snprintf(back, sizeof(back), "%s", S.list.title);
    snprintf(path, sizeof(path), "%s", se.v[k].key ? se.v[k].key : "");
    if (show_list(path, back, 1, 0) == 0 && S.show.on) {
        for (int i = 0; i < S.list.n; i++)  /* the episode to carry on with, selected */
            if (S.list.v[i].view_offset_ms > 0 || !S.list.v[i].watched) {
                sel = i;
                break;
            }
        S.sel = sel;
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    }
}

/* Another series of the show: its episodes, in place (Back still goes to
   where the show was opened from) */
static void show_season(int k)
{
    char path[256];
    if (!S.show.on || k < 0 || k >= S.show.seasons.n || k == S.show.season)
        return;
    snprintf(path, sizeof(path), "%s", S.show.seasons.v[k].key ? S.show.seasons.v[k].key : "");
    show_list(path, "", 0, 0);
}

/* Play and Mark watched on the show page */
static void show_action(int id)
{
    plex_item *it = &S.show.det.v[0];
    if (id == SH_PLAY && S.show.next < S.list.n) {
        select_tile(S.show.next);
        play_item(&S.list.v[S.show.next], PLAY_DEFAULT);
    } else if (id == SH_WATCHED) {
        int w = !it->watched;
        plex_list se;
        mark(it, w);
        it->watched = w;
        memset(&se, 0, sizeof(se));
        if (it->key && plex_list_get(&S.px, it->key, &se) == 0 &&
            (plex_list_keep(&se, "season"), se.n == S.show.seasons.n)) {
            plex_list_free(&S.show.seasons);    /* their unwatched counts */
            S.show.seasons = se;
        } else if (se.n || se.v)
            plex_list_free(&se);
        refresh_list();             /* the episodes' ticks */
    }
}

static void open_item(int i, int how)
{
    const plex_item *it;
    if (i < 0 || i >= S.list.n)
        return;
    it = &S.list.v[i];
    if (it->kind == PI_FOLDER && it->type && !strcmp(it->type, "show") && it->key && it->rating_key) {
        show_open(it);
    } else if (it->kind == PI_FOLDER && it->type && !strcmp(it->type, "season") && it->grandparent_key) {
        plex_item show;                 /* a series (recently added): its show's page */
        char key[160];
        memset(&show, 0, sizeof(show));
        snprintf(key, sizeof(key), "/library/metadata/%s/children", it->grandparent_key);
        show.kind = PI_FOLDER;
        show.type = (char *)"show";
        show.rating_key = it->grandparent_key;
        show.key = key;
        show.title = it->grandparent_title ? it->grandparent_title : it->title;
        show_open(&show);
    } else if (it->kind == PI_FOLDER) {
        char path[256], back[64];
        snprintf(path, sizeof(path), "%s", it->key ? it->key : "");
        snprintf(back, sizeof(back), "%s", S.list.title);
        show_list(path, back, 1, 0);
    } else if (it->kind == PI_VIDEO) {
        play_item(it, how);
    } else if (S.px.kind == SRV_DLNA) {     /* a song or a picture on a DLNA server */
        set_status("%s isn't played: films and TV only.", it->subtitle && *it->subtitle ? it->subtitle : "That");
    } else {
        set_status("%s libraries can't be opened yet: films and TV only.", it->subtitle ? it->subtitle : "These");
    }
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
    if (S.cast_wanted && S.page == PG_DETAILS && det_cast_step())
        return;
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
            /* null events at once */
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
