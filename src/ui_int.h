/*
 * ui_int.h - what the files of Matinee's desktop front end share (ui.c
 * describes it): the SWIs and messages it uses, the browser's layout, the
 * state (S), and each file's functions that the others call.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_UI_INT_H
#define MATINEE_UI_INT_H

#include <errno.h>
#include <pthread.h>
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
#include "draw.h"
#include "player.h"
#include "imgcache.h"
#include "bgreq.h"

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
typedef struct { int w, h; const char *name; } ui_size;
extern const ui_size sizes[3];
#define TILE_W    (sizes[S.psize].w)
#define POSTER_H  (sizes[S.psize].h)
#define TEXT_H    96                /* the title and a line under it */
#define TILE_H    (POSTER_H + TEXT_H)
#define GAP       36
#define TOPBAR_H  128               /* Back, where you are, the status line, Refresh */
#define STRIP_H   88                /* under it: Home and the libraries, as tabs */
#define HEADER_H  (TOPBAR_H + strip_h())
#define LIBBAR_H  168               /* a library's bar: Library / Collections / Playlists, sort, filter; A-Z
                                       (and a line more when the window's too narrow for one) */
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

static inline _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

#ifdef __riscos__
#define SEP "."
#else
#define SEP "/"                     /* the host test's files */
#endif

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
   the collection first, then More like this, then the extras (rel_order) */
enum { REL_SIMILAR, REL_EXTRAS, REL_COLLECTION, REL_ROWS };

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

typedef struct {
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
        int tl_async;               /* builtin_timeline: this "playing" from the thread */
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
} ui_state;
extern ui_state S;

/* ---- shared by more than one file ------------------------------------------ */

#define EP_TW   320                 /* the picture, OS units */
#define EP_TH   180
#define LIB_TABS 16
#define SORTS 4
#define WIN_PERCENT 75
#define SCROLL_W    40
#define SI_MAX 24

typedef struct { int box[4]; unsigned flags; int data[3]; } icon_t;
enum { PLAY_DEFAULT, PLAY_RESUME, PLAY_START };
typedef struct { int flags, sub; unsigned iflags; int data[3]; } mitem_t;
typedef struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    mitem_t item[16];
} wmenu_t;
typedef struct { wmenu_t m; char text[16][80]; int n; } menu_t;

/* ---- ui.c ---- */
int now_cs(void);
void hourglass(int on);
void report(const char *fmt, ...);
int ask(const char *text);
void latin1(const char *s, char *out, size_t size);
void hms(int64_t ms, char *out, size_t size);
void make_dir(const char *path);
void set_type(const char *path, int type);
int mime_type(int from, const char *what, int fallback);
extern char sv_sprite[16];
void window_state(int w, int *st);
void open_front(int w, int x0, int y0, int x1, int y1, int sx, int sy);
void close_window(int w);
void set_caret(int w, int icon, const char *text);
void si_set_status(const char *fmt, ...);
void set_status(const char *fmt, ...);

/* ---- choices.c ---- */
extern const char *player_names[PLAYER_COUNT];
extern const char *pic_names[PIC_COUNT];
void choices_save(void);
void choices_load(void);

/* ---- pictures.c ---- */
void cache_free_all(void);
void cache_trim(void);
poster_t *cache_find(const char *thumb);
poster_t *poster_fetch(const char *thumb, const char *key, int w, int h, int art);
int pf_waiting(void);
poster_t *poster_soon(const char *thumb, const char *key, int w, int h, int art);

/* ---- browse.c ---- */
int strip_h(void);
int libbar_h(void);
void force_redraw(int w, int x0, int y0, int x1, int y1);
void tile_box(int i, int *x0, int *y0, int *x1, int *y1);
int layout_cols(int width);
void set_extent(void);
void redraw_tile(int i);
void make_disp(void);
void fit_disp(int i);
void plot_sprite(const poster_t *p, int x0, int y0, int x1, int y1);
void header_button(int id, int vis_w, int *x0, int *y0, int *x1, int *y1);
int searching(void);
int tab_box(int k, int *x0, int *y0, int *x1, int *y1);
int tab_current(void);
int tab_hit(int sx, int sy);
int badge_kind(const plex_item *it);
void badges(const plex_item *it, int x0, int y0, int x1, int y1);
void redraw(int *b);
int poster_step(void);
void show_fetch_art(void);
void show_layout(void);
int show_hit(int sx, int sy);
extern const char *const sort_names[SORTS];
void lib_path(int view, int sort, int unwatched, char *out, size_t size);
int az_letter(const char *t);
void lib_layout(void);
void az_box(int k, int *x0, int *y0, int *x1, int *y1);
int lib_hit(int sx, int sy);
void select_tile(int i);
void set_where(void);
void browser_open(void);
int show_list(const char *path, const char *back_title, int push, int sel);
void go_back(void);
void search_now(void);
void search_open(void);
int search_key(int k);
void refresh_list(void);
void browser_top(void);
void tab_open(int k);
void list_forget(void);
int tile_at(int sx, int sy);
int header_hit(int sx, int sy);
void set_hover(int i);

/* ---- home.c ---- */
void home_box(int i, int *x0, int *y0, int *x1, int *y1);
int home_row_of(int i, int *col);
const char *home_pic(int i, char *key, size_t size, int *art);
void home_redraw(int ox, int oy, int cy0, int cy1);
void draw_home_card(int i, int ox, int oy);
const plex_item *home_hero(void);
void home_layout(void);
void home_fetch_art(void);
int home_hit(int sx, int sy);
void home_key(int k);

/* ---- details.c ---- */
int vis_width(void);
void det_redraw(int ox, int oy, int vis_w, int cy0, int cy1);
void det_leave(void);
void det_show(int i);
extern int det_wd, art_w, art_h;          /* the width laid out for; the backdrop's box */
extern int art_due;                       /* when to fetch the backdrop at the new size (0: not) */
const plex_item *det_item(void);
int det_is(const plex_item *it);
void det_art_box(void);
void det_fetch_art(void);
int det_fetch(const plex_item *it, int with_art);
int det_cast_step(void);
void det_repaint(void);
void det_refresh(void);
int det_rel_at(int sx, int sy);
int det_button_at(int sx, int sy);

/* ---- signin.c ---- */
void signin_redraw(int ox, int oy, int vis_w);
void choose_server(void);
typedef struct { int id, x0, y0, x1, y1; } si_box_t;
extern si_box_t si_box[SI_MAX];
extern int si_n;
void si_layout(void);
int si_hit(int sx, int sy);
int si_field_of(int id);
void si_redraw_fields(void);
void pin_new(void);
void signin_show(int mode);
void signin_open(void);
void signin_close(void);
void signin_cancel(void);
int signin_key(int k);
void pin_check(void);
void use_manual(void);
void qc_new(void);
void qc_check(void);
void jf_password(void);
int use_jf(int k);
void jf_add(void);
int servers_get(void);
int use_server(int i);
void dl_search(void);
void dl_use(const char *addr);
int use_dl(int k);
void dl_add(void);
void sign_out(void);
void forget_server(int what);
void bar_select(void);

/* ---- playback.c ---- */
void timeline_step(void);
int timeline_going(void);
void builtin_stop(int leave);
int find_task(const char *name);
void start_player(int player, const char *file);
void play_item(const plex_item *it, int how);
void media_headers(const char *in, char *out, size_t size);
int builtin_open_at(double t, int new_session);
void builtin_chapter_step(int d);
void builtin_event(int e);
void builtin_nulls(void);

/* ---- saving.c ---- */
void save_prepare(const plex_item *it);
int too_big(int64_t size, const char *title);
void save_stop(const char *why);
void media_url(const char *key, char *url, size_t size);
void save_start(const char *path);
void save_step(void);
void speed_close(void);
void speed_start(const plex_item *it);
void speed_step(void);
void close_menus(void);
void save_ok(void);
void drag_start(void);
void drag_end(void);

/* ---- menus.c ---- */
int users_get(void);
void player_menu_open(void);
void psubs_menu_open(void);
void builtin_subs_apply(void);
void builtin_subs_forget(void);
extern menu_t m_subs;
void open_menu(menu_t *m, int x, int y);
void bar_menu_open(int x);
void subs_menu_build(void);
void lib_action(int id, int x, int y);
void item_menu_open(int x, int y);
void open_item(int i, int how);
void quit(void);
void mark(const plex_item *it, int watched);
void rate(const plex_item *it, int r);
int menu_select(const int *sel);
void home_action(int id);
void show_season(int k);
void show_action(int id);

#endif
