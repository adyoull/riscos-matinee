/*
 * ui.c - PlexRO's desktop front end.
 *
 * An icon on the icon bar. Select on it opens the browser (or, before
 * signing in, the sign-in window); Menu gives:
 *
 *   Info >, Sign in..., Servers >, Player > (ReelEGL / Reel),
 *   Quality > (1080p / 720p / 480p), Direct play when possible,
 *   Sign out, Quit
 *
 * Sign-in window: a code to type at plex.tv/link (checked every 2
 * seconds), or a server's address and token typed by hand.
 *
 * Browser window: Back, where you are, and a status line above a grid of
 * posters. Double-click opens a library, show or season, or plays a
 * video; Backspace or Escape goes back; Return opens the one selected;
 * the arrow keys move the selection. Menu on a poster: Play, Resume from
 * h:mm:ss, Play from start, Save original file >, Mark watched, Mark
 * unwatched, Back, Refresh.
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

/* The browser's layout, OS units */
#define TILE_W    232               /* a poster's width */
#define POSTER_H  348               /* and height (2:3) */
#define LINE_H    40                /* a line of text under it */
#define TILE_H    (POSTER_H + 8 + 2 * LINE_H)
#define GAP       24
#define HEADER_H  112               /* Back, the path, the status line */
#define BR_MIN_W  (2 * (TILE_W + GAP) + GAP)

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
int __dynamic_da_max_size = 256 << 20;
#endif

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

/* ---- state -------------------------------------------------------------- */

typedef struct poster {
    struct poster *next;
    char thumb[256];
    int *area;                      /* a sprite area with one sprite, "p" */
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
    int signin_w, browser_w, save_w;
    int signin_open, browser_open;
    int xeig, yeig, scr_w, scr_h;   /* the screen, OS units */

    plex_ctx px;
    char agent[64];
    int player, quality, direct;

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
    int posters_wanted;             /* a scan for missing posters is due */
    poster_t *cache;
    size_t cache_bytes;

    /* menus */
    int menu_kind;                  /* 1 icon bar, 2 item */
    int menu_x, menu_y;

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
    } save;
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

/* A text's width in the desktop font, OS units */
static int text_width(const char *s)
{
    _kernel_swi_regs r;
    r.r[0] = 1;
    r.r[1] = (intptr_t)s;
    r.r[2] = 0;
    return swi(Wimp_TextOp, &r) ? (int)strlen(s) * 16 : r.r[0];
}

/* Cut s (in place) so it fits in width, ending "..." if it was cut */
static void fit(char *s, int width)
{
    size_t n = strlen(s);
    if (text_width(s) <= width)
        return;
    while (n > 0) {
        s[--n] = 0;
        if (n + 4 > 80)
            continue;
        {
            char t[84];
            snprintf(t, sizeof(t), "%s...", s);
            if (text_width(t) <= width) {
                strcpy(s, t);
                return;
            }
        }
    }
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

static const char *player_names[2] = { "ReelEGL", "Reel" };

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
            "server_name %s\nserver_id %s\nserver_local %d\nplayer %s\nquality %d\ndirect_play %d\n",
            S.px.client_id, S.px.account_token, S.px.base, S.px.token,
            S.px.server_name, S.px.server_id, S.px.local, player_names[S.player], S.quality, S.direct);
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
    S.player = PLAYER_REELEGL;      /* ReelEGL by default, Reel the other */
    S.quality = Q_720;
    S.direct = 1;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (value(line, "client_id", v, sizeof(v)))
                snprintf(id, sizeof(id), "%s", v);
            else if (value(line, "player", v, sizeof(v)))
                S.player = !strcmp(v, "Reel") ? PLAYER_REEL : PLAYER_REELEGL;
            else if (value(line, "quality", v, sizeof(v)))
                S.quality = atoi(v) >= 0 && atoi(v) < Q_COUNT ? atoi(v) : Q_720;
            else if (value(line, "direct_play", v, sizeof(v)))
                S.direct = atoi(v) != 0;
        }
    }
    if (!*id) {                     /* made once: the server knows us by it */
        unsigned a = (unsigned)time(NULL), b = (unsigned)now_cs() * 2654435761u ^ (unsigned)clock();
        snprintf(id, sizeof(id), "plexro-%08x%08x", a, b);
    }
    plex_ctx_init(c, id, PLEXRO_VERSION);
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

#define F_LABEL   0x17000010u       /* v centred; black on grey */
#define F_DISPLAY 0x0700003Du       /* border, centred, filled; black on white */
#define F_BUTTON  0x1700303Du       /* click; border, centred, filled; black on grey */
#define F_WRITE   0x0700F035u       /* writable; border, v centred, filled; black on white */

static char si_title[] = "Sign in";
static char si_link[] = "Type the code at plex.tv/link";
static char si_newcode[] = "New code";
static char si_or[] = "Or a server on your network:";
static char si_addrl[] = "Address";
static char si_tokl[] = "Token";
static char si_use[] = "Use these";
static char valid_write[] = "Ktar";
static char br_back[] = "Back";
static char sv_title_text[] = "Save as";
static char sv_sprite[16];
static char sv_ok_text[] = "Save";

static void make_windows(void)
{
    window_t w;
    icon_t ic[SI_COUNT];

    /* sign-in */
    window_defaults(&w, 720, 520, si_title, sizeof(si_title), 0x84000012u | 0x02000000u);   /* + close */
    memset(ic, 0, sizeof(ic));
    icon_text(&ic[SI_CODE], 180, -100, 540, -24, F_DISPLAY, S.code, sizeof(S.code), NULL);
    icon_text(&ic[SI_LINK], 12, -148, 708, -104, F_LABEL | 0x08, si_link, sizeof(si_link), NULL);
    icon_text(&ic[SI_STATUS], 12, -196, 708, -152, F_LABEL | 0x08, S.si_status, sizeof(S.si_status), NULL);
    icon_text(&ic[SI_NEWCODE], 480, -256, 708, -204, F_BUTTON, si_newcode, sizeof(si_newcode), NULL);
    icon_text(&ic[SI_OR], 12, -320, 708, -276, F_LABEL, si_or, sizeof(si_or), NULL);
    icon_text(&ic[SI_ADDRL], 12, -376, 156, -324, F_LABEL, si_addrl, sizeof(si_addrl), NULL);
    icon_text(&ic[SI_ADDR], 160, -376, 708, -324, F_WRITE, S.si_addr, sizeof(S.si_addr), valid_write);
    icon_text(&ic[SI_TOKL], 12, -436, 156, -384, F_LABEL, si_tokl, sizeof(si_tokl), NULL);
    icon_text(&ic[SI_TOK], 160, -436, 708, -384, F_WRITE, S.si_tok, sizeof(S.si_tok), valid_write);
    icon_text(&ic[SI_USE], 480, -500, 708, -448, F_BUTTON, si_use, sizeof(si_use), NULL);
    S.signin_w = create_window(&w, ic, SI_COUNT);

    /* browser: its extent follows the list; the header row is icons */
    window_defaults(&w, 4 * (TILE_W + GAP) + GAP, 1000, S.title, sizeof(S.title), 0xBF000002u);
    w.ext[1] = -1000;
    w.ext[2] = S.scr_w;
    w.wbutton = 10 << 12;           /* click, drag, double-click */
    w.minw = BR_MIN_W;
    w.minh = HEADER_H + 200;
    memset(ic, 0, sizeof(ic));
    icon_text(&ic[BR_BACK], 12, -60, 148, -8, F_BUTTON, br_back, sizeof(br_back), NULL);
    icon_text(&ic[BR_PATH], 160, -60, S.scr_w - 12, -8, F_LABEL, S.where, sizeof(S.where), NULL);
    icon_text(&ic[BR_STATUS], 12, -108, S.scr_w - 12, -64, 0x18000010u, S.status, sizeof(S.status), NULL);
    S.browser_w = create_window(&w, ic, BR_COUNT);

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

static void refresh_icon(int w, int i)
{
    int b[4];
    _kernel_swi_regs r;
    b[0] = w; b[1] = i; b[2] = 0; b[3] = 0;     /* no change: redraws it */
    r.r[1] = (intptr_t)b;
    swi(Wimp_SetIconState, &r);
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
    if (S.signin_open)
        refresh_icon(S.signin_w, SI_STATUS);
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
        refresh_icon(S.browser_w, BR_STATUS);
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
        if (!used) {
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

static int poster_px_w(void) { return TILE_W >> S.xeig; }
static int poster_px_h(void) { return POSTER_H >> S.yeig; }

/* A sprite area with one 32bpp sprite "p", w x h pixels at the screen's
   resolution, filled with dark grey. Made by hand (the header's layout is
   fixed), so no SWI is needed. */
static int *sprite_make(int w, int h, size_t *bytes)
{
    size_t image = (size_t)w * h * 4;
    int *a = malloc(16 + 44 + image);
    int *s;
    unsigned *px;
    if (!a)
        return NULL;
    a[0] = (int)(16 + 44 + image);  /* area size */
    a[1] = 1;                       /* one sprite */
    a[2] = 16;                      /* the first at +16 */
    a[3] = (int)(16 + 44 + image);  /* free space after it */
    s = a + 4;
    s[0] = (int)(44 + image);       /* to the next sprite */
    memset(&s[1], 0, 12);
    ((char *)&s[1])[0] = 'p';
    s[4] = w - 1;                   /* width in words - 1 (32bpp: one word a pixel) */
    s[5] = h - 1;
    s[6] = 0;
    s[7] = 31;
    s[8] = 44;                      /* image */
    s[9] = 44;                      /* no mask */
    /* new-format mode word: 32bpp (type 6), the screen's dpi */
    s[10] = (int)(1u | ((unsigned)(180 >> S.xeig) << 1) | ((unsigned)(180 >> S.yeig) << 14) | (6u << 27));
    px = (unsigned *)(s + 11);
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i] = 0x00302C28;         /* 0xBBGGRR */
    *bytes = 16 + 44 + image;
    return a;
}

/* The JPEG drawn into the sprite, as big as fits, in the middle. 0 = ok. */
static int jpeg_into(int *area, int w, int h, const char *jpeg, size_t len)
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
    if ((int64_t)w * jh <= (int64_t)h * jw) {   /* width decides */
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

static poster_t *poster_fetch(const char *thumb)
{
    poster_t *p = calloc(1, sizeof(*p));
    char *jpeg = NULL;
    size_t len = 0;
    int w = poster_px_w(), h = poster_px_h();
    if (!p)
        return NULL;
    snprintf(p->thumb, sizeof(p->thumb), "%s", thumb);
    if (plex_poster(&S.px, thumb, w, h, &jpeg, &len) != 0 ||
        !(p->area = sprite_make(w, h, &p->bytes)) ||
        jpeg_into(p->area, w, h, jpeg, len) != 0) {
        free(p->area);
        p->area = NULL;
        p->bytes = 0;
        p->failed = 1;
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
    int h = list_height();
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
    force_redraw(S.browser_w, x0 - 12, y0 - 12, x1 + 12, y1 + 12);
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
        fit(S.disp[i].line[0], TILE_W - 8);
        fit(S.disp[i].line[1], TILE_W - 8);
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

static char kind_film[] = "Video", kind_folder[] = "Folder", kind_other[] = "Not yet";

static void draw_tile(int i)
{
    const plex_item *it = &S.list.v[i];
    disp_t *d = &S.disp[i];
    icon_t ic;
    int x0, y0, x1, y1, py0;
    tile_box(i, &x0, &y0, &x1, &y1);
    py0 = y1 - POSTER_H;
    if (i == S.sel) {               /* a light blue panel behind it */
        memset(&ic, 0, sizeof(ic));
        ic.box[0] = x0 - 10; ic.box[1] = y0 - 6; ic.box[2] = x1 + 10; ic.box[3] = y1 + 10;
        ic.flags = 0xF0000020u;
        plot(&ic);
    }
    memset(&ic, 0, sizeof(ic));
    ic.box[0] = x0; ic.box[1] = py0; ic.box[2] = x1; ic.box[3] = y1;
    if (d->poster && d->poster->area) {
        ic.flags = 0x0000011Au;     /* sprite, centred, indirected */
        ic.data[0] = (int)(intptr_t)"p";
        ic.data[1] = (int)(intptr_t)d->poster->area;
        ic.data[2] = 1;
    } else {                        /* no poster (yet): a grey card */
        char *k = it->kind == PI_VIDEO ? kind_film : it->kind == PI_FOLDER ? kind_folder : kind_other;
        ic.flags = 0x5000013Du;     /* text, border, centred, filled, indirected; white on grey */
        ic.data[0] = (int)(intptr_t)k;
        ic.data[1] = -1;
        ic.data[2] = (int)strlen(k) + 1;
    }
    plot(&ic);
    if (it->kind == PI_VIDEO && it->view_offset_ms > 0 && it->duration_ms > 0) {
        int64_t part = (int64_t)(TILE_W - 16) * it->view_offset_ms / it->duration_ms;
        memset(&ic, 0, sizeof(ic));             /* how far it's been watched */
        ic.box[0] = x0 + 8; ic.box[1] = py0 + 8; ic.box[2] = x1 - 8; ic.box[3] = py0 + 20;
        ic.flags = 0x70000020u;
        plot(&ic);
        ic.box[2] = x0 + 8 + (int)part;
        ic.flags = 0xE0000020u;                 /* orange */
        plot(&ic);
    } else if ((it->kind == PI_VIDEO || it->kind == PI_FOLDER) && !it->watched && it->rating_key) {
        memset(&ic, 0, sizeof(ic));             /* not watched yet: an orange corner */
        ic.box[0] = x1 - 28; ic.box[1] = y1 - 28; ic.box[2] = x1; ic.box[3] = y1;
        ic.flags = 0xE0000020u;
        plot(&ic);
    }
    for (int l = 0; l < 2; l++) {
        memset(&ic, 0, sizeof(ic));
        ic.box[0] = x0 - 8; ic.box[2] = x1 + 8;
        ic.box[3] = py0 - 8 - l * LINE_H;
        ic.box[1] = ic.box[3] - LINE_H;
        ic.flags = 0x00000119u | (unsigned)(l ? 4 : it->kind == PI_OTHER ? 3 : 7) << 24;
        ic.data[0] = (int)(intptr_t)d->line[l];
        ic.data[1] = -1;
        ic.data[2] = (int)sizeof(d->line[l]);
        plot(&ic);
    }
}

static void redraw(int *b)
{
    _kernel_swi_regs r;
    int more;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_RedrawWindow, &r))
        return;
    more = r.r[0];
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6];     /* the work area's origin on the screen */
        int cy0 = b[8] - oy, cy1 = b[10] - oy;      /* the clip, work area y */
        if (b[0] == S.browser_w && S.have_list && S.disp && S.cols > 0) {
            int top = -HEADER_H - GAP;
            int rf = (top - cy1) / (TILE_H + GAP) - 1, rl = (top - cy0) / (TILE_H + GAP) + 1;
            if (rf < 0)
                rf = 0;
            (void)ox;
            for (int row = rf; row <= rl; row++)
                for (int c = 0; c < S.cols; c++) {
                    int i = row * S.cols + c;
                    if (i < S.list.n)
                        draw_tile(i);
                }
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
    if (!S.browser_open || !S.have_list || !S.disp) {
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
            S.disp[i].poster = poster_fetch(it->thumb);
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
    for (int i = 0; i < S.nhist && n < sizeof(t); i++)
        n += snprintf(t + n, sizeof(t) - n, "%s > ", S.hist[i].title);
    if (n < sizeof(t))
        snprintf(t + n, sizeof(t) - n, "%s", S.list.title);
    latin1(t, S.where, sizeof(S.where));
    latin1(S.px.server_name, t, sizeof(t));
    snprintf(S.title, sizeof(S.title), "%s: %s", APP, t);
    if (S.browser_open) {
        _kernel_swi_regs r;         /* the title bar, redrawn (RISC OS 5) */
        r.r[0] = S.browser_w;
        r.r[1] = 0x4B534154;
        r.r[2] = 3;
        swi(Wimp_ForceRedraw, &r);
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
    S.posters_wanted = 1;
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
    snprintf(S.path, sizeof(S.path), "%s", path);
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
    if (!l.n)
        set_status("Nothing here.");
    else if (l.total > l.n)
        set_status("The first %d of %d.", l.n, l.total);
    else
        set_status("%d item%s.", l.n, l.n == 1 ? "" : "s");
    return 0;
}

static void go_back(void)
{
    hist_t h;
    if (!S.nhist)
        return;
    h = S.hist[--S.nhist];
    if (show_list(h.path, "", 0, h.sel) != 0)
        S.nhist++;                  /* still where it was */
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

/* ---- sign-in -------------------------------------------------------------------- */

static void choose_server(void);

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
        refresh_icon(S.signin_w, SI_CODE);
        return;
    }
    hourglass(0);
    S.pin_id = id;
    S.pin_next = now_cs() + PIN_EVERY;
    snprintf(S.code, sizeof(S.code), "%s", code);
    refresh_icon(S.signin_w, SI_CODE);
    si_set_status("Waiting for the code to be typed in...");
}

static void signin_open(void)
{
    int w = 720, h = 520, x0 = (S.scr_w - w) / 2, y0 = (S.scr_h - h) / 2;
    int st[9];
    if (S.signin_open) {
        window_state(S.signin_w, st);
        open_front(S.signin_w, st[1], st[2], st[3], st[4], 0, 0);
    } else {
        open_front(S.signin_w, x0, y0, x0 + w, y0 + h, 0, 0);
        S.signin_open = 1;
    }
    set_caret(S.signin_w, SI_ADDR, S.si_addr);
    if (!S.pin_id)
        pin_new();
}

static void signin_close(void)
{
    if (S.signin_open)
        close_window(S.signin_w);
    S.signin_open = 0;
    S.pin_id = 0;
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

static void sign_out(void)
{
    plex_ctx *c = &S.px;
    c->account_token[0] = c->base[0] = c->token[0] = c->server_name[0] = c->server_id[0] = 0;
    S.nservers = 0;
    if (S.browser_open)
        close_window(S.browser_w);
    S.browser_open = 0;
    if (S.have_list)
        plex_list_free(&S.list);
    S.have_list = 0;
    S.nhist = 0;
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

static void play_item(const plex_item *it, int how)
{
    static play_t p;
    caps_t k;
    char file[300], title[300];
    int allow = S.direct, resume = 1, type;
    /* Resume from: the server starts its stream there (copying the video
       when it can); Reel can't be told where to start a file of its own */
    if (how == PLAY_RESUME)
        allow = 0;
    if (how == PLAY_START)
        resume = 0;
    caps_for(S.quality, &k);
    if (caps_play(&S.px, it, &k, allow, resume, &p) != 0) {
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
        if (S.save.size > 0)
            set_status("Saving %s: %.0f of %.0f MB (%d%%)", S.save.title, S.save.done / 1048576.0,
                       S.save.size / 1048576.0, (int)(S.save.done * 100 / S.save.size));
        else
            set_status("Saving %s: %.0f MB", S.save.title, S.save.done / 1048576.0);
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
    mitem_t item[SERVERS_MAX + 2];
} wmenu_t;
typedef struct { wmenu_t m; char text[SERVERS_MAX + 2][80]; int n; } menu_t;

static menu_t m_bar, m_servers, m_player, m_quality, m_item;

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
    for (int i = 0; i < 2; i++)
        menu_add(&m_player, player_names[i], S.player == i, 0, -1, 0);
    menu_end(&m_player);
    menu_begin(&m_quality, "Quality");
    for (int i = 0; i < Q_COUNT; i++)
        menu_add(&m_quality, caps_quality_name(i), S.quality == i, 0, -1, 0);
    menu_end(&m_quality);

    menu_begin(&m_bar, APP);
    menu_add(&m_bar, "Info", 0, 0, S.proginfo, 0);
    menu_add(&m_bar, "Sign in...", 0, 0, -1, 0);
    menu_add(&m_bar, "Servers", 0, !signed_in, signed_in ? (int)(intptr_t)&m_servers.m : -1, 0);
    menu_add(&m_bar, "Player", 0, 0, (int)(intptr_t)&m_player.m, 0);
    menu_add(&m_bar, "Quality", 0, 0, (int)(intptr_t)&m_quality.m, 0);
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

static void item_menu_build(void)
{
    const plex_item *it = sel_item();
    int video = it && it->kind == PI_VIDEO, folder = it && it->kind == PI_FOLDER;
    char t[80], when[32];
    save_prepare(it);
    menu_begin(&m_item, it ? S.disp[S.sel].line[0] : APP);
    menu_add(&m_item, folder ? "Open" : "Play", 0, !video && !folder, -1, 0);
    if (video && it->view_offset_ms > 0) {
        hms(it->view_offset_ms, when, sizeof(when));
        snprintf(t, sizeof(t), "Resume from %s", when);
        menu_add(&m_item, t, 0, 0, -1, 0);
    } else {
        menu_add(&m_item, "Resume", 0, 1, -1, 0);
    }
    menu_add(&m_item, "Play from start", 0, !video, -1, 0);
    if (S.save.active)
        menu_add(&m_item, "Stop saving", 0, 0, -1, 1);
    else
        menu_add(&m_item, "Save original file", 0, !S.sv_ok,
                 S.sv_ok && S.sv_size <= FILE_MAX ? S.save_w : -1, 1);
    menu_add(&m_item, "Mark watched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 0);
    menu_add(&m_item, "Mark unwatched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 1);
    menu_add(&m_item, "Back", 0, S.nhist == 0, -1, 0);
    menu_add(&m_item, "Refresh", 0, 0, -1, 0);
    menu_end(&m_item);
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
    for (int i = 0; i < PEND_MAX; i++)
        if (S.pend[i].ref)
            remove(S.pend[i].path);
    r.r[0] = S.task;
    swi(Wimp_CloseDown, &r);
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
            if (sel[1] == 0 || sel[1] == 1) {
                S.player = sel[1];
                choices_save();
            }
            break;
        case MB_QUALITY:
            if (sel[1] >= 0 && sel[1] < Q_COUNT) {
                S.quality = sel[1];
                choices_save();
            }
            break;
        case MB_DIRECT:
            S.direct = !S.direct;
            choices_save();
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
        case MI_SAVE:
            if (S.save.active)
                save_stop("Stopped saving.");
            else if (it && S.sv_ok && !too_big(S.sv_size, S.sv_title))
                report("To save it, move to the right of Save original file, then drag the icon "
                       "to a directory display.");
            break;
        case MI_WATCHED:
        case MI_UNWATCHED:
            if (it) {
                int i = S.sel;
                hourglass(1);
                if (plex_mark(&S.px, it, sel[0] == MI_WATCHED) != 0) {
                    hourglass(0);
                    report("Can't mark it: %s", S.px.err);
                    break;
                }
                hourglass(0);
                S.list.v[i].watched = sel[0] == MI_WATCHED;
                S.list.v[i].view_offset_ms = 0;
                redraw_tile(i);
                set_status(sel[0] == MI_WATCHED ? "Marked as watched." : "Marked as not watched.");
            }
            break;
        case MI_BACK:
            go_back();
            break;
        case MI_REFRESH:
            refresh_list();
            break;
        }
    }
    /* Adjust keeps the menu open */
    r.r[1] = (intptr_t)b;
    if (!swi(Wimp_GetPointerInfo, &r) && (b[2] & 1)) {
        if (kind == 1)
            bar_menu_open(S.menu_x);
        else if (kind == 2 && S.browser_open)
            item_menu_open(S.menu_x, S.menu_y);
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
    if (w == S.signin_w) {
        if (i == SI_NEWCODE)
            pin_new();
        else if (i == SI_USE)
            use_manual();
        return;
    }
    if (w == S.save_w) {
        if (i == SV_FILE && (buttons & 0x50))
            drag_start();
        else if (i == SV_OK && (buttons & 5))
            save_ok();
        return;
    }
    if (w == S.browser_w) {
        int t;
        if (i == BR_BACK) {
            if (buttons & 5)
                go_back();
            return;
        }
        t = tile_at(b[0], b[1]);
        set_caret(S.browser_w, -1, NULL);
        if (buttons & 2) {
            if (t >= 0)
                select_tile(t);
            item_menu_open(b[0], b[1]);
        } else if (buttons & 0x500) {           /* one click */
            select_tile(t);
        } else if (buttons & 5) {               /* double-click */
            select_tile(t);
            if (t >= 0)
                open_item(t, PLAY_DEFAULT);
        }
    }
}

static void key(int *b)
{
    int w = b[0], i = b[1], k = b[6];
    _kernel_swi_regs r;
    if (w == S.signin_w && k == 13 && (i == SI_TOK || i == SI_ADDR)) {
        use_manual();
        return;
    }
    if (w == S.save_w && k == 13) {
        save_ok();
        return;
    }
    if (w == S.browser_w) {
        int n = S.have_list ? S.list.n : 0, s = S.sel;
        switch (k) {
        case 8: case 0x1B: case 0x7F:
            go_back();
            return;
        case 13:
            if (s >= 0)
                open_item(s, PLAY_DEFAULT);
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
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
    if (b[0] == S.browser_w) {
        int w = b[3] - b[1], cols = layout_cols(w);
        S.posters_wanted = 1;       /* scrolled or resized: more may be in view */
        if (cols != S.cols) {
            S.cols = cols;
            set_extent();
            force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
        }
    }
}

static void close_request(int *b)
{
    if (b[0] == S.signin_w)
        signin_close();
    else if (b[0] == S.browser_w) {
        close_window(S.browser_w);
        S.browser_open = 0;
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
        cache_free_all();
        if (S.have_list)
            make_disp();
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
    if (S.pin_id && S.signin_open && now_cs() - S.pin_next >= 0) {
        pin_check();
        return;
    }
    if (S.posters_wanted)
        poster_step();
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
    snprintf(S.code, sizeof(S.code), "-");
    snprintf(S.title, sizeof(S.title), APP);
    make_windows();
    iconbar_icon();
    S.proginfo = proginfo_create(APP, PURPOSE, APP_AUTHOR, PLEXRO_VERSION " (" PLEXRO_DATE ")");

    for (;;) {
        int mask = 1 << 4 | 1 << 5 | 1 << 11 | 1 << 12;   /* no pointer or caret events */
        int reason = Wimp_Poll;
        if (S.save.active || S.posters_wanted) {
            /* null events at once */
        } else if (S.pin_id && S.signin_open) {
            reason = Wimp_PollIdle;
            r.r[2] = S.pin_next;
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
int ui_test_windows(int *signin, int *browser, int *save)
{
    *signin = S.signin_w;
    *browser = S.browser_w;
    *save = S.save_w;
    return S.signin_open | S.browser_open << 1;
}
#endif

#ifndef PLEXRO_NO_MAIN
int main(int argc, char **argv) { return plexro_main(argc, argv); }
#endif
