/*
 * player.c - PlexRO's built-in player (see player.h).
 *
 * Built on riscos-ffmpeg's reelcore (libreelcore.a from its devkit), with
 * the window side done as !Reel does it (player/reel.c): the sprite path,
 * the VideoOverlay path and its rules, and full screen as a desktop window.
 * Those parts follow Reel's code closely, so fixes there apply here too.
 * Part of riscos-plex. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include "kernel.h"
#include "reelcore.h"
#include "draw.h"
#include "player.h"

#define OS_Byte                0x06
#define OS_WriteN              0x46
#define OS_ReadModeVariable    0x35
#define OS_SpriteOp            0x2E
#define OS_ReadMonotonicTime   0x42
#define OS_ScreenMode          0x65
#define OS_SWINumberFromString 0x39
#define Wimp_CreateWindow      0x400C1
#define Wimp_DeleteWindow      0x400C3
#define Wimp_OpenWindow        0x400C5
#define Wimp_CloseWindow       0x400C6
#define Wimp_RedrawWindow      0x400C8
#define Wimp_UpdateWindow      0x400C9
#define Wimp_GetRectangle      0x400CA
#define Wimp_GetWindowState    0x400CB
#define Wimp_GetPointerInfo    0x400CF
#define Wimp_ForceRedraw       0x400D1
#define Wimp_SetCaretPosition  0x400D2
#define Wimp_DragBox           0x400D0

#define BAR_H     168               /* the controls, OS units: the title and time over the buttons */
#define BTN_R     32                /* a round button's radius */
#define SHOW_BAR  300               /* full screen: cs the bar stays after the pointer moves */
#define HIDE_POINTER 200            /* full screen: cs before a still pointer is hidden */
#define NOTE_CS   300
#define MIN_SPRITE_BYTES (1024 * 1024)
#define YV12_FOURCC 0x32315659
#define OV_MAX_PIXELS (1920L * 1088 * 11 / 10)   /* the most an overlay is made with */
/* the mini player (as Reel's): a small window with no furniture above the icon bar */
#define MINI_BAR   72               /* its controls: Play, the position bar, Normal, the grip */
#define MINI_W     640              /* its width at first, OS units (320 pixels on most screens) */
#define MINI_MIN_W 480              /* narrowest when resized (the grip) */
#define MINI_EDGE  32               /* its gap from the screen's right edge at first */
#define MINI_LIFT  16               /* ... and above the icon bar */
#define GRIP       32
#define ONTOP_CS   100              /* Keep on top: how often it looks */

typedef struct { int x0, y0, x1, y1; } box_t;

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

static struct {
    int task;
    int win, full;                  /* the caller's window; the full screen one (0: not made) */
    int fullscreen;
    ReelCore *v;
    int ready, ended, failed;
    char error[240];
    /* what's playing */
    char url[4096], headers[1600], agent[128], title[160];
    double base, start, duration;
    int convert, based, subs;             /* based: base checked against the stream's own times */
    double sync0;                   /* position - clock at the first picture (see panel_update) */
    int opened_cs;
    /* the screen */
    int xeig, yeig, log2bpp, trgb, scr_w, scr_h;
    /* the window: its visible size, the picture and the bar (work area) */
    int vis_w, vis_h;
    box_t pic, bar;
    int bar_shown;                  /* full screen: the bar is showing (until bar_until) */
    int bar_until, ptr_x, ptr_y, ptr_cs;
    int ptr_hidden;                 /* full screen: the pointer hidden (it hasn't moved for a while) */
    /* the mini player: its window (0: not made), where the normal window was */
    int mini, mini_win, main_st[9];
    int mini_w, mini_right, mini_bottom;    /* its width, gap from the right, bottom (-1: above the icon bar) */
    int ontop, ontop_cs;
    /* the picture sprite */
    int *area;
    int spr_w, spr_h, spr_rows, have_frame;
    unsigned sprite_plots;
    /* choices */
    int hw, pic_mode, stats;
    double vol;
    /* the bar's text */
    char time_text[64], note[120];
    int note_until;
    int last_bar_cs;
    double last_pos;
    int fill_x;
    /* a card over the picture */
    int card;
    char skip[40];                  /* the skip button's label ("": none) */
    double chap[100];               /* where chapters start (seconds) */
    int nchap;
    char card_head[80], card_line[160], card_line2[120], card_b1[40], card_b2[40];
    box_t card_btn[2];
    /* the buttons, laid out (work area) */
    box_t btn[PB_COUNT];
    /* timing */
    int idle_cs;
    int ov_pending;
    unsigned draw_n, draw_cs;
    /* the log */
    FILE *log;
    int log_cs;
} P;

/* ---- small helpers ------------------------------------------------------ */

static int now_cs(void)
{
    _kernel_swi_regs r;
    return swi(OS_ReadMonotonicTime, &r) ? (int)(clock() * 100 / CLOCKS_PER_SEC) : r.r[0];
}

/* <Wimp$ScrapDir>.PlexROLog while playing (PlexRO$Log: another file, or "off") */
static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
    va_list a;
    if (!P.log)
        return;
    fprintf(P.log, "%6.2f ", (now_cs() - P.log_cs) / 100.0);
    va_start(a, fmt);
    vfprintf(P.log, fmt, a);
    va_end(a);
    fputc('\n', P.log);
    fflush(P.log);
}

static void log_open(void)
{
    const char *e = getenv("PlexRO$Log"), *scrap = getenv("Wimp$ScrapDir");
    char path[300];
    if (P.log)
        return;
    if (e && (!strcasecmp(e, "off") || !strcmp(e, "0")))
        return;
    if (e && *e)
        snprintf(path, sizeof(path), "%s", e);
    else if (scrap && *scrap)
#ifdef __riscos__
        snprintf(path, sizeof(path), "%s.PlexROLog", scrap);
#else
        snprintf(path, sizeof(path), "%s/PlexROLog", scrap);
#endif
    else
        return;
    P.log = fopen(path, "w");
    P.log_cs = now_cs();
}

static void ff_log(int level, const char *line)
{
    (void)level;
    lg("ffmpeg: %s", line);
}

static int mode_var(int var)
{
    _kernel_swi_regs r;
    r.r[0] = -1;
    r.r[1] = var;
    return swi(OS_ReadModeVariable, &r) ? 0 : r.r[2];
}

static void read_screen(void)
{
    P.xeig = mode_var(4);
    P.yeig = mode_var(5);
    P.log2bpp = mode_var(9);
    P.trgb = P.log2bpp == 5 && (mode_var(0) & 0x4000);
    P.scr_w = (mode_var(11) + 1) << P.xeig;
    P.scr_h = (mode_var(12) + 1) << P.yeig;
}

static void vdu_clip(int x0, int y0, int x1, int y1)    /* inclusive, OS units */
{
    unsigned char b[9] = { 24, x0 & 255, (x0 >> 8) & 255, y0 & 255, (y0 >> 8) & 255,
                           x1 & 255, (x1 >> 8) & 255, y1 & 255, (y1 >> 8) & 255 };
    _kernel_swi_regs r;
    r.r[0] = (intptr_t)b;
    r.r[1] = 9;
    swi(OS_WriteN, &r);
}

static void hms(double s, char *buf, size_t n)
{
    long t = s > 0 ? (long)s : 0;
    if (t >= 3600)
        snprintf(buf, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else
        snprintf(buf, n, "%ld:%02ld", t / 60, t % 60);
}

static void window_state(int w, int *b)
{
    _kernel_swi_regs r;
    b[0] = w;
    r.r[1] = (intptr_t)b;
    swi(Wimp_GetWindowState, &r);
}

static void force_redraw(int w, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = x0; r.r[2] = y0; r.r[3] = x1; r.r[4] = y1;
    swi(Wimp_ForceRedraw, &r);
}

static void set_caret(int w)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0;
    r.r[4] = 1 << 25;               /* invisible */
    r.r[5] = -1;
    swi(Wimp_SetCaretPosition, &r);
}

static int cur_win(void) { return P.fullscreen ? P.full : P.mini ? P.mini_win : P.win; }
static void pointer_show(int on);

/* ---- where things go ------------------------------------------------------ */

/* The picture: the whole window above the bar; full screen, the whole screen */
static void layout_boxes(void)
{
    int st[9], cy, x, r, mid0, mid1;
    if (P.fullscreen) {
        P.vis_w = P.scr_w;
        P.vis_h = P.scr_h;
    } else {
        window_state(cur_win(), st);
        P.vis_w = st[3] - st[1];
        P.vis_h = st[4] - st[2];
    }
    if (P.mini) {                   /* Play, the position bar, Normal, the grip; the rest out of sight */
        P.bar = (box_t){ 0, -P.vis_h, P.vis_w, -P.vis_h + MINI_BAR };
        P.pic = (box_t){ 0, -P.vis_h + MINI_BAR, P.vis_w, 0 };
        if (P.pic.y1 - P.pic.y0 < 2)
            P.pic.y0 = P.pic.y1 - 2;
        memset(P.btn, 0, sizeof(P.btn));
        cy = -P.vis_h + MINI_BAR / 2;
        P.btn[PB_PLAY] = (box_t){ 12, cy - 28, 12 + 56, cy + 28 };
        P.btn[PB_GRIP] = (box_t){ P.vis_w - GRIP, -P.vis_h, P.vis_w, -P.vis_h + GRIP };
        r = P.vis_w - GRIP - 8;
        P.btn[PB_NORMAL] = (box_t){ r - 128, cy - 24, r, cy + 24 };
        mid0 = 12 + 56 + 24;
        mid1 = r - 128 - 24;
        if (mid1 < mid0 + 32)
            mid1 = mid0 + 32;
        P.btn[PB_TRACK] = (box_t){ mid0, cy - 12, mid1, cy + 12 };
        return;
    }
    P.bar = (box_t){ 0, -P.vis_h, P.vis_w, -P.vis_h + BAR_H };
    P.pic = P.fullscreen ? (box_t){ 0, -P.vis_h, P.vis_w, 0 } : (box_t){ 0, -P.vis_h + BAR_H, P.vis_w, 0 };
    if (P.pic.y1 - P.pic.y0 < 2)
        P.pic.y0 = P.pic.y1 - 2;
    /* the buttons, under the title and the time */
    cy = -P.vis_h + 56;
    x = 24;
    P.btn[PB_BACK] = (box_t){ x, cy - BTN_R, x + 2 * BTN_R, cy + BTN_R };
    x += 2 * BTN_R + 20;
    P.btn[PB_PLAY] = (box_t){ x, cy - 40, x + 80, cy + 40 };
    x += 80 + 20;
    P.btn[PB_REW] = (box_t){ x, cy - 28, x + 96, cy + 28 };
    x += 96 + 12;
    P.btn[PB_FWD] = (box_t){ x, cy - 28, x + 96, cy + 28 };
    x += 96 + 32;
    mid0 = x;
    r = P.vis_w - 24;
    P.btn[PB_FULL] = (box_t){ r - 2 * BTN_R, cy - BTN_R, r, cy + BTN_R };
    r -= 2 * BTN_R + 16;
    P.btn[PB_STATS] = (box_t){ r - 112, cy - 28, r, cy + 28 };
    r -= 112 + 16;
    P.btn[PB_SUBS] = (box_t){ 0, 0, 0, 0 };
    if (P.subs) {
        P.btn[PB_SUBS] = (box_t){ r - 176, cy - 28, r, cy + 28 };
        r -= 176 + 16;
    }
    r -= 16;
    mid1 = r;
    /* a narrow window: Subtitles goes (the menu has it), then the skip
       buttons (the arrow keys do it), then Stats (S) */
    if (mid1 - mid0 < 160 && P.btn[PB_SUBS].x1 > P.btn[PB_SUBS].x0) {
        mid1 = P.btn[PB_STATS].x0 - 32;
        P.btn[PB_SUBS] = (box_t){ 0, 0, 0, 0 };
    }
    if (mid1 - mid0 < 160) {
        mid0 = P.btn[PB_REW].x0;
        P.btn[PB_REW] = P.btn[PB_FWD] = (box_t){ 0, 0, 0, 0 };
    }
    if (mid1 - mid0 < 160) {
        mid1 = P.btn[PB_FULL].x0 - 32;
        P.btn[PB_STATS] = (box_t){ 0, 0, 0, 0 };
    }
    if (mid1 < mid0 + 64)
        mid1 = mid0 + 64;
    P.btn[PB_TRACK] = (box_t){ mid0, cy - 16, mid1, cy + 16 };
    /* the skip button: the right of the title's line */
    P.btn[PB_SKIP] = (box_t){ 0, 0, 0, 0 };
    if (P.skip[0]) {
        int w = draw_width(D_BOLD, P.skip) + 56;
        P.btn[PB_SKIP] = (box_t){ P.vis_w - 24 - w, P.bar.y1 - 62, P.vis_w - 24, P.bar.y1 - 8 };
    }
    /* the card: in the middle of the picture */
    {
        int cw = 1000, ch = 360, px = (P.pic.x0 + P.pic.x1) / 2, py = (P.pic.y0 + P.pic.y1) / 2;
        if (cw > P.vis_w - 40)
            cw = P.vis_w - 40;
        P.btn[PB_CARD1] = (box_t){ px - cw / 2 + 40, py - ch / 2 + 40, px - 20, py - ch / 2 + 40 + 64 };
        P.btn[PB_CARD2] = (box_t){ px + 20, py - ch / 2 + 40, px + cw / 2 - 40, py - ch / 2 + 40 + 64 };
    }
}

/* The bar is there (the window: always; full screen: while the pointer's been moving) */
static int bar_visible(void) { return !P.fullscreen || P.bar_shown; }

/* ---- the picture sprite (as !Reel's) ---------------------------------------- */

static void sprite_free(void)
{
    free(P.area);
    P.area = NULL;
    P.spr_w = P.spr_h = 0;
    P.have_frame = 0;
}

static int *sprite_header(void) { return P.area + 4; }
static uint8_t *sprite_pixels(void) { return (uint8_t *)(P.area + 4 + 11); }

static int sprite_make(int w, int h)
{
    size_t rows = h, bytes;
    int *spr, mode;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (P.area && P.spr_w == w && P.spr_h == h)
        return 0;
    sprite_free();
    /* at least 1MB: small sprites plot slowly on the Pi (riscos-mesa's finding) */
    if ((size_t)w * 4 * rows < MIN_SPRITE_BYTES)
        rows = (MIN_SPRITE_BYTES + (size_t)w * 4 - 1) / ((size_t)w * 4);
    bytes = 16 + 44 + (size_t)w * 4 * rows;
    if (!(P.area = calloc(1, bytes)))
        return -1;
    P.area[0] = (int)bytes;
    P.area[1] = 1;
    P.area[2] = 16;
    P.area[3] = (int)bytes;
    spr = sprite_header();
    spr[0] = (int)bytes - 16;
    memcpy(&spr[1], "picture\0\0\0\0\0", 12);
    spr[4] = w - 1;
    spr[5] = (int)rows - 1;
    spr[6] = 0;
    spr[7] = 31;
    spr[8] = 44;
    spr[9] = 44;
    mode = 1 | ((180 >> P.xeig) << 1) | ((180 >> P.yeig) << 14) | (6 << 27);   /* 32bpp TBGR */
    if (P.trgb) {
        _kernel_swi_regs r;
        r.r[0] = 1;                 /* the current mode's specifier */
        if (!swi(OS_ScreenMode, &r))
            mode = r.r[1];
    }
    spr[10] = mode;
    P.spr_w = w;
    P.spr_h = h;
    P.spr_rows = (int)rows;
    return 0;
}

static int pic_flags(void)
{
    return P.pic_mode == PIC_FILL ? REELCORE_FILL : P.pic_mode == PIC_STRETCH ? REELCORE_STRETCH : 0;
}

static void pic_make(void)
{
    sprite_make((P.pic.x1 - P.pic.x0) >> P.xeig, (P.pic.y1 - P.pic.y0) >> P.yeig);
}

static void pic_refresh(void)
{
    if (!P.v || !P.area || !P.ready)
        return;
    if (reelcore_draw_pixels(P.v, sprite_pixels(), P.spr_w * 4, P.spr_w, P.spr_h, P.trgb, pic_flags()) == 0)
        P.have_frame = 1;
}

static void sprite_plot(int x, int y1)
{
    _kernel_swi_regs r;
    if (!P.area || !P.have_frame)
        return;
    r.r[0] = 52 + 512;              /* PutSpriteScaled, by pointer */
    r.r[1] = (intptr_t)P.area;
    r.r[2] = (intptr_t)sprite_header();
    r.r[3] = x;
    r.r[4] = y1 - (P.spr_rows << P.yeig);
    r.r[5] = 0;
    r.r[6] = 0;                     /* no scaling: the sprite's dpi is the screen's */
    r.r[7] = 0;                     /* no table: 16 and 32bpp screens */
    swi(OS_SpriteOp, &r);
    P.sprite_plots++;
}

/* ---- the hardware overlay (as !Reel 0.1.18+) ----------------------------------

   Each new picture is copied as YV12 into an overlay buffer, and the
   display scales it on its way to the monitor. The Pi 4's lessons
   (riscos-mesa's ovltest): probe with Create (Vet fails); overlay memory is
   uncached, so only write to it; a switch happens at the next vsync, so 3
   buffers and never write one until a vsync has passed; the overlay is
   "Basic", over everything, so it's hidden while anything overlaps the
   picture, while paused, at the end and under the card; a mode change
   doesn't free it. Anything that fails: the sprite, with no error. */

enum { OV_CREATE, OV_DESTROY, OV_DISPLAY, OV_MAP, OV_UNMAP, OV_SCALE, OV_WINDOW, OV_POSITION, OV_REDRAW, OV_N };
static const char *const ov_names[OV_N] = {
    "VideoOverlay_Create", "VideoOverlay_Destroy", "VideoOverlay_DisplayBuffer", "VideoOverlay_MapBuffer",
    "VideoOverlay_UnmapBuffer", "VideoOverlay_SetScale", "VideoOverlay_SetWindow", "VideoOverlay_SetPosition",
    "VideoOverlay_RedrawWindow"
};

static struct {
    int found;
    int swi[OV_N];
    int id, type, banks, next, shown, vsync;
    int minw, minh, maxw, maxh;
    int win;
    int fw, fh, colour, mode;
    int failed;
    int placed[9];
} ov;

static int ov_allowed(void)
{
    const char *e = getenv("EGL$Overlay");
    if (!P.hw || getenv("PlexRO$NoOverlay"))
        return 0;
    return !(e && (!strcasecmp(e, "off") || !strcasecmp(e, "no") || !strcmp(e, "0")));
}

static int ov_available(void)
{
    _kernel_swi_regs r;
    if (ov.found)
        return 1;
    for (int i = 0; i < OV_N; i++) {
        r.r[0] = 0;
        r.r[1] = (intptr_t)ov_names[i];
        if (swi(OS_SWINumberFromString, &r))
            return 0;
        ov.swi[i] = r.r[0] & ~0x20000;
    }
    ov.found = 1;
    return 1;
}

static _kernel_oserror *ov_call(int which, int a, int b)
{
    _kernel_swi_regs r;
    r.r[0] = a;
    r.r[1] = b;
    return swi(ov.swi[which], &r);
}

static int ov_vsyncs(void)
{
    _kernel_swi_regs r;
    r.r[0] = 176;                   /* the vsync counter */
    r.r[1] = 0;
    r.r[2] = 255;
    return swi(OS_Byte, &r) ? 0 : r.r[1] & 0xFF;
}

static int ov_mode_sig(void) { return P.scr_w ^ (P.scr_h << 12) ^ (P.log2bpp << 24) ^ (P.trgb << 28); }

static void ov_destroy(void)
{
    if (ov.id) {
        if (ov.shown)
            ov_call(OV_DISPLAY, ov.id, -1);
        ov_call(OV_DESTROY, ov.id, 0);
        lg("overlay: destroyed");
    }
    ov.id = ov.shown = ov.win = 0;
    P.ov_pending = 0;
}

static void ov_fail(const char *why)
{
    ov_destroy();
    ov.failed = 1;
    lg("overlay: %s: plotting instead", why);
}

static int ov_hide(void)
{
    if (!ov.shown)
        return 0;
    ov_call(OV_DISPLAY, ov.id, -1);
    ov.shown = 0;
    P.ov_pending = 0;
    return 1;
}

static int ov_create(int fw, int fh, int colour)
{
    int sel[12], b = 0, banks;
    _kernel_swi_regs r;
    for (banks = 3; banks >= 2; banks--) {
        sel[0] = 1; sel[1] = fw; sel[2] = fh; sel[3] = 7; sel[4] = -1;     /* Log2BPP 7: planar */
        sel[5] = 0;                                     /* ModeFlags: YCbCr, bit 14 video range, bit 15 BT.709 */
        sel[6] = 0x2000 | (colour & REELCORE_YUV_FULL ? 0 : 0x4000) | (colour & REELCORE_YUV_709 ? 0x8000 : 0);
        sel[7] = 3; sel[8] = YV12_FOURCC;
        sel[9] = 13; sel[10] = banks;                   /* MinScreenBanks */
        sel[11] = -1;
        r.r[0] = (intptr_t)sel;
        r.r[1] = (fw << 16) | fh;
        r.r[2] = 1;                                     /* must scale */
        r.r[3] = P.task;
        if (swi(ov.swi[OV_CREATE], &r)) {
            lg("overlay: Create refused YV12 %dx%d", fw, fh);
            return 0;
        }
        ov.id = r.r[0];
        ov.type = r.r[1] & 0xFF;
        ov.minw = r.r[2]; ov.minh = r.r[3]; ov.maxw = r.r[4]; ov.maxh = r.r[5];
        for (b = 0; b < banks; b++) {
            if (ov_call(OV_MAP, ov.id, b))
                break;
            ov_call(OV_UNMAP, ov.id, b);
        }
        if (b == banks)
            break;
        lg("overlay: only %d of %d buffers (GPU memory)", b, banks);
        ov_call(OV_DESTROY, ov.id, 0);
        ov.id = 0;
    }
    if (!ov.id)
        return 0;
    ov.banks = banks;
    ov.next = 0;
    ov.win = 0;
    ov.vsync = ov_vsyncs() - 1;
    memset(ov.placed, 0, sizeof(ov.placed));
    lg("overlay: YV12 %dx%d, %d buffers, %s, scales %dx%d to %dx%d", fw, fh, banks,
       ov.type == 0 ? "Z-Order" : "Basic", ov.minw, ov.minh, ov.maxw, ov.maxh);
    return 1;
}

/* The part of the picture the overlay may cover: not the bar when it shows */
static box_t ov_clip(void)
{
    box_t c = P.pic;
    if (P.fullscreen && P.bar_shown)
        c.y0 = P.bar.y1;
    return c;
}

static int ov_covered(int w)
{
    int st[9], o[9], n, h;
    box_t pic = ov_clip(), sc;
    window_state(w, st);
    if (!(st[8] & (1 << 16)))
        return 1;
    sc.x0 = st[1] - st[5] + pic.x0; sc.x1 = st[1] - st[5] + pic.x1;
    sc.y0 = st[4] - st[6] + pic.y0; sc.y1 = st[4] - st[6] + pic.y1;
    for (h = st[7], n = 0; h != -1 && n < 256; n++) {
        _kernel_swi_regs r;
        o[0] = h;
        r.r[1] = (intptr_t)o;
        if (swi(Wimp_GetWindowState, &r))
            return 1;
        if ((o[8] & (1 << 16)) && o[1] < sc.x1 && o[3] > sc.x0 && o[2] < sc.y1 && o[4] > sc.y0)
            return 1;
        h = o[7];
    }
    return 0;
}

/* Scales and places it over the picture (Fit, Fill, Stretch): 1 placed,
   0 outside its limits, -1 an error */
static int ov_place(int w)
{
    box_t pic = P.pic, clip = ov_clip();
    int bw = (pic.x1 - pic.x0) >> P.xeig, bh = (pic.y1 - pic.y0) >> P.yeig;
    int dw = reelcore_width(P.v), dh = reelcore_height(P.v), rw = bw, rh = bh, x, y;
    int want[9];
    _kernel_swi_regs r;
    if (bw < 1 || bh < 1 || dw < 1 || dh < 1)
        return 0;
    if (P.pic_mode != PIC_STRETCH) {
        double sx = (double)bw / dw, sy = (double)bh / dh;
        double sc = P.pic_mode == PIC_FIT ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
        rw = (int)(dw * sc + 0.5);
        rh = (int)(dh * sc + 0.5);
        if (P.pic_mode == PIC_FIT) {
            if (rw > bw) rw = bw;
            if (rh > bh) rh = bh;
        }
    }
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if ((ov.minw && rw < ov.minw) || (ov.minh && rh < ov.minh) || (ov.maxw && rw > ov.maxw) || (ov.maxh && rh > ov.maxh))
        return 0;
    x = pic.x0 + (((bw - rw) / 2) << P.xeig);
    y = pic.y1 - (((bh - rh) / 2) << P.yeig);
    want[0] = w; want[1] = rw; want[2] = rh; want[3] = x; want[4] = y;
    want[5] = clip.x0; want[6] = clip.y0; want[7] = clip.x1; want[8] = clip.y1;
    if (!memcmp(want, ov.placed, sizeof(want)))
        return 1;
    if (ov.win != w) {
        if (ov_call(OV_WINDOW, ov.id, w))
            return -1;
        ov.win = w;
    }
    r.r[0] = ov.id; r.r[1] = rw; r.r[2] = rh; r.r[3] = (rw << 16) | rh;
    if (swi(ov.swi[OV_SCALE], &r))
        return -1;
    r.r[0] = ov.id; r.r[1] = x; r.r[2] = y;
    r.r[3] = clip.x0; r.r[4] = clip.y0; r.r[5] = clip.x1; r.r[6] = clip.y1;
    if (swi(ov.swi[OV_POSITION], &r))
        return -1;
    memcpy(ov.placed, want, sizeof(want));
    if (ov.fw > 0)                  /* the stats panel: drawn into the frame this much bigger */
        reelcore_set_yuv_scale(P.v, (double)ov.fw / rw);
    lg("overlay: %dx%d pixels at %d,%d in window &%x", rw, rh, x, y, w);
    return 1;
}

/* A new picture through the overlay (1); the caller plots it (0); 2: not
   yet (no vsync since the last switch: try again on the next pass) */
static int ov_show_frame(void)
{
    int w = cur_win(), fw, fh, colour, mode, placed, b;
    _kernel_swi_regs r;
    if (!P.v || !w || reelcore_paused(P.v) || P.ended || P.card || !ov_allowed() || !ov_available() ||
        reelcore_frame_size(P.v, &fw, &fh) < 0) {
        if (ov.id && !ov_allowed())
            ov_destroy();
        ov_hide();
        return 0;
    }
    fw &= ~1;
    fh &= ~1;
    /* bigger than HD (4K): halved into the overlay (NEON), to at most about
       1920x1088's pixels, as Reel 0.1.21: a full-size 4K overlay (3 buffers
       of 12 MB) ran the Pi's GPU short and the screen kept going black */
    while ((long)fw * fh > OV_MAX_PIXELS || fw > 2048 || fh > 2048) {
        fw = fw / 2 & ~1;
        fh = fh / 2 & ~1;
    }
    reelcore_draw_yuv420(P.v, NULL, NULL, 0, 0, &colour);
    mode = ov_mode_sig();
    if ((ov.id || ov.failed) && (ov.fw != fw || ov.fh != fh || ov.colour != colour || ov.mode != mode)) {
        ov_destroy();
        ov.failed = 0;
    }
    ov.fw = fw; ov.fh = fh; ov.colour = colour; ov.mode = mode;
    if (ov.failed || fw < 2 || fh < 2)
        return 0;
    if (ov_covered(w)) {
        ov_hide();
        return 0;
    }
    if (!ov.id && !ov_create(fw, fh, colour)) {
        ov.failed = 1;
        return 0;
    }
    if ((placed = ov_place(w)) < 0) {
        ov_fail("placing it failed");
        return 0;
    }
    if (!placed) {
        ov_hide();
        return 0;
    }
    if (ov_vsyncs() == ov.vsync)
        return 2;
    b = ov.next;
    r.r[0] = ov.id;
    r.r[1] = b;
    if (swi(ov.swi[OV_MAP], &r)) {
        ov_fail("MapBuffer failed");
        return 0;
    }
    {
        const int *a = (const int *)(intptr_t)r.r[0];
        uint8_t *planes[3] = { (uint8_t *)(intptr_t)a[0], (uint8_t *)(intptr_t)a[2], (uint8_t *)(intptr_t)a[4] };
        int pitch[3] = { a[1], a[3], a[5] };
        int e = reelcore_draw_yuv420(P.v, planes, pitch, fw, fh, NULL);
        ov_call(OV_UNMAP, ov.id, b);
        if (e < 0) {
            ov_fail("copying the picture failed");
            return 0;
        }
    }
    if (ov_call(OV_DISPLAY, ov.id, b)) {
        ov_fail("DisplayBuffer failed");
        return 0;
    }
    ov.vsync = ov_vsyncs();
    ov.next = (b + 1) % ov.banks;
    if (!ov.shown) {
        ov.shown = 1;
        lg("overlay: showing");
        force_redraw(w, P.pic.x0, P.pic.y0, P.pic.x1, P.pic.y1);   /* RedrawWindow prepares the area */
    }
    return 1;
}

/* Paused, the end, the card, an error box: hide it and plot the picture */
static void ov_hide_and_draw(void)
{
    int w = cur_win();
    if (ov_hide() && w) {
        pic_refresh();
        force_redraw(w, P.pic.x0, P.pic.y0, P.pic.x1, P.pic.y1);
    }
}

/* ---- drawing ------------------------------------------------------------------ */

static void round_button(const box_t *b, int ox, int oy, unsigned c, unsigned bg)
{
    int r = (b->y1 - b->y0) / 2;
    draw_round(ox + b->x0, oy + b->y0, ox + b->x1, oy + b->y1, r, c, bg);
}

static void pill(const box_t *b, int ox, int oy, const char *label, int on, unsigned bg)
{
    int w = draw_width(D_BOLD, label);
    round_button(b, ox, oy, on ? C_ACCENT : C_CARD, bg);
    draw_text(D_BOLD, ox + (b->x0 + b->x1 - w) / 2, oy + (b->y0 + b->y1) / 2 - 10, label, C_TEXT, on ? C_ACCENT : C_CARD);
}

static void bar_text(void)
{
    char pos[16], dur[16];
    double p = player_position(), d = player_duration();
    ReelCoreNet ns;
    if (P.note[0] && now_cs() - P.note_until < 0) {
        snprintf(P.time_text, sizeof(P.time_text), "%s", P.note);
        return;
    }
    if (!P.ready) {
        int s = (now_cs() - P.opened_cs) / 100;
        snprintf(P.time_text, sizeof(P.time_text), s ? "Opening %d s" : "Opening", s);
        return;
    }
    hms(p, pos, sizeof(pos));
    hms(d, dur, sizeof(dur));
    if (P.v && reelcore_net(P.v, &ns) && ns.buffering && !ns.ended && !reelcore_paused(P.v) && !P.ended)
        snprintf(P.time_text, sizeof(P.time_text), "Buffering  %s / %s", pos, dur);
    else if (d > 0)
        snprintf(P.time_text, sizeof(P.time_text), "%s / %s", pos, dur);
    else
        snprintf(P.time_text, sizeof(P.time_text), "%s", pos);
}

static int track_fill(void)
{
    double p = player_position(), d = player_duration(), f = d > 0 ? p / d : 0;
    const box_t *t = &P.btn[PB_TRACK];
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return t->x0 + (int)((t->x1 - t->x0) * f);
}

/* The position bar: how far it's got, and the knob */
static void draw_track(int ox, int oy)
{
    const box_t *b;
    int cy, fx;
    b = &P.btn[PB_TRACK];
    cy = (b->y0 + b->y1) / 2;
    fx = track_fill();
    P.fill_x = fx;
    draw_round(ox + b->x0, oy + cy - 6, ox + b->x1, oy + cy + 6, 6, C_CARD, C_HEADER);
    if (fx > b->x0 + 6)
        draw_round(ox + b->x0, oy + cy - 6, ox + fx, oy + cy + 6, 6, C_ACCENT, C_CARD);
    if (player_duration() > 0)                  /* where chapters start: gaps in the bar */
        for (int i = 0; i < P.nchap; i++) {
            int x = b->x0 + (int)((b->x1 - b->x0) * (P.chap[i] / player_duration()));
            if (P.chap[i] > 0.5 && x > b->x0 + 8 && x < b->x1 - 8)
                draw_rect(ox + x - 2, oy + cy - 6, ox + x + 2, oy + cy + 6, C_HEADER);
        }
    draw_glyph(G_CIRCLE, ox + fx - 12, oy + cy - 12, ox + fx + 12, oy + cy + 12, C_TEXT, C_CARD);
}

/* The mini player's controls: Play, the position bar, Normal, the grip */
static void draw_mini_bar(int ox, int oy, int paused)
{
    const box_t *b;
    b = &P.btn[PB_PLAY];
    round_button(b, ox, oy, C_ACCENT, C_HEADER);
    if (paused)
        draw_glyph(G_PLAY, ox + b->x0 + 20, oy + b->y0 + 14, ox + b->x1 - 14, oy + b->y1 - 14, C_TEXT, C_ACCENT);
    else {
        draw_rect(ox + b->x0 + 18, oy + b->y0 + 16, ox + b->x0 + 25, oy + b->y1 - 16, C_TEXT);
        draw_rect(ox + b->x1 - 25, oy + b->y0 + 16, ox + b->x1 - 18, oy + b->y1 - 16, C_TEXT);
    }
    pill(&P.btn[PB_NORMAL], ox, oy, "Normal", 0, C_HEADER);
    b = &P.btn[PB_GRIP];                        /* three short diagonal steps */
    for (int i = 0; i < 3; i++)
        draw_rect(ox + b->x1 - 8 - i * 8, oy + b->y0 + 4, ox + b->x1 - 4 - i * 8, oy + b->y0 + 8 + i * 8, C_SUB);
    draw_track(ox, oy);
}

static void draw_bar(int ox, int oy)
{
    const box_t *b;
    char title[160];
    int paused = !P.v || !P.ready || reelcore_paused(P.v) || P.ended;
    draw_rect(ox + P.bar.x0, oy + P.bar.y0, ox + P.bar.x1, oy + P.bar.y1, C_HEADER);
    if (P.mini) {
        draw_mini_bar(ox, oy, paused);
        return;
    }
    b = &P.btn[PB_BACK];
    round_button(b, ox, oy, C_CARD, C_HEADER);
    draw_glyph(G_BACK, ox + b->x0 + 20, oy + b->y0 + 16, ox + b->x1 - 20, oy + b->y1 - 16, C_TEXT, C_CARD);
    b = &P.btn[PB_PLAY];
    round_button(b, ox, oy, C_ACCENT, C_HEADER);
    if (paused)
        draw_glyph(G_PLAY, ox + b->x0 + 28, oy + b->y0 + 22, ox + b->x1 - 20, oy + b->y1 - 22, C_TEXT, C_ACCENT);
    else {                                          /* two bars */
        draw_rect(ox + b->x0 + 26, oy + b->y0 + 22, ox + b->x0 + 36, oy + b->y1 - 22, C_TEXT);
        draw_rect(ox + b->x1 - 36, oy + b->y0 + 22, ox + b->x1 - 26, oy + b->y1 - 22, C_TEXT);
    }
    if (P.btn[PB_REW].x1 > P.btn[PB_REW].x0) {
        pill(&P.btn[PB_REW], ox, oy, "-10 s", 0, C_HEADER);
        pill(&P.btn[PB_FWD], ox, oy, "+10 s", 0, C_HEADER);
    }
    if (P.btn[PB_STATS].x1 > P.btn[PB_STATS].x0)
        pill(&P.btn[PB_STATS], ox, oy, "Stats", P.stats, C_HEADER);
    if (P.btn[PB_SUBS].x1 > P.btn[PB_SUBS].x0)
        pill(&P.btn[PB_SUBS], ox, oy, "Subtitles", 0, C_HEADER);
    b = &P.btn[PB_FULL];                            /* four corners */
    round_button(b, ox, oy, C_CARD, C_HEADER);
    {
        int x0 = ox + b->x0 + 18, y0 = oy + b->y0 + 18, x1 = ox + b->x1 - 18, y1 = oy + b->y1 - 18, t = 6, l = 10;
        draw_rect(x0, y0, x0 + l, y0 + t, C_TEXT); draw_rect(x0, y0, x0 + t, y0 + l, C_TEXT);
        draw_rect(x1 - l, y0, x1, y0 + t, C_TEXT); draw_rect(x1 - t, y0, x1, y0 + l, C_TEXT);
        draw_rect(x0, y1 - t, x0 + l, y1, C_TEXT); draw_rect(x0, y1 - l, x0 + t, y1, C_TEXT);
        draw_rect(x1 - l, y1 - t, x1, y1, C_TEXT); draw_rect(x1 - t, y1 - l, x1, y1, C_TEXT);
    }
    /* the title and the time along the top of the bar */
    bar_text();
    {
        int tw = draw_width(D_BODY, P.time_text), y = P.bar.y1 - 44, right = P.vis_w - 24;
        if (P.btn[PB_SKIP].x1 > P.btn[PB_SKIP].x0) {
            pill(&P.btn[PB_SKIP], ox, oy, P.skip, 1, C_HEADER);
            right = P.btn[PB_SKIP].x0 - 24;
        }
        snprintf(title, sizeof(title), "%s", P.title);
        draw_fit(D_BOLD, title, right - 24 - tw - 32);
        draw_text(D_BOLD, ox + 24, oy + y, title, C_TEXT, C_HEADER);
        draw_text(D_BODY, ox + right - tw, oy + y, P.time_text, C_SUB, C_HEADER);
    }
    draw_track(ox, oy);
}

static void draw_card(int ox, int oy)
{
    const box_t *b1 = &P.btn[PB_CARD1], *b2 = &P.btn[PB_CARD2];
    int x0 = b1->x0 - 40, x1 = b2->x1 + 40, y0 = b1->y0 - 40, y1 = y0 + 360;
    char t[160];
    draw_round(ox + x0, oy + y0, ox + x1, oy + y1, 24, C_BG, RGB(0, 0, 0));
    draw_text(D_BODY, ox + x0 + 40, oy + y1 - 60, P.card_head, C_SUB, C_BG);
    snprintf(t, sizeof(t), "%s", P.card_line);
    draw_fit(D_TITLE, t, x1 - x0 - 80);
    draw_text(D_TITLE, ox + x0 + 40, oy + y1 - 130, t, C_TEXT, C_BG);
    draw_text(D_BODY, ox + x0 + 40, oy + y1 - 190, P.card_line2, C_SUB, C_BG);
    pill(b1, ox, oy, P.card_b1, 1, C_BG);
    pill(b2, ox, oy, P.card_b2, 0, C_BG);
}

/* One rectangle of a redraw or an update of the current window */
static void draw_rect_of(int w, int *b, int update)
{
    _kernel_swi_regs r;
    int ox = b[1] - b[5], oy = b[4] - b[6];
    box_t c;
    draw_origin(ox, oy);
    c.x0 = ox + P.pic.x0; c.x1 = ox + P.pic.x1; c.y0 = oy + P.pic.y0; c.y1 = oy + P.pic.y1;
    if (c.x0 < b[7]) c.x0 = b[7];
    if (c.y0 < b[8]) c.y0 = b[8];
    if (c.x1 > b[9]) c.x1 = b[9];
    if (c.y1 > b[10]) c.y1 = b[10];
    if (c.x0 < c.x1 && c.y0 < c.y1) {
        if (P.have_frame && !ov.shown) {
            vdu_clip(c.x0, c.y0, c.x1 - 1, c.y1 - 1);
            sprite_plot(ox + P.pic.x0, oy + P.pic.y1);
            vdu_clip(b[7], b[8], b[9] - 1, b[10] - 1);
        } else if (!ov.shown) {
            draw_rect(c.x0, c.y0, c.x1, c.y1, RGB(0, 0, 0));
            if (!P.ready) {                         /* opening: say so */
                char t[200];
                snprintf(t, sizeof(t), "%s", P.title);
                draw_fit(D_TITLE, t, P.pic.x1 - P.pic.x0 - 80);
                draw_text(D_TITLE, ox + P.pic.x0 + 40, oy + (P.pic.y0 + P.pic.y1) / 2 + 20, t, C_TEXT, RGB(0, 0, 0));
                draw_text(D_BODY, ox + P.pic.x0 + 40, oy + (P.pic.y0 + P.pic.y1) / 2 - 40,
                          P.failed ? P.error : "Opening...", C_SUB, RGB(0, 0, 0));
            }
        }
    }
    if (!update && ov.shown && w == ov.win) {
        r.r[0] = ov.id;
        r.r[1] = (intptr_t)b;
        swi(ov.swi[OV_REDRAW], &r);
    }
    if (P.card && !P.mini)          /* too small for the card: Up next plays on its own */
        draw_card(ox, oy);
    if (bar_visible() && b[8] < oy + P.bar.y1)
        draw_bar(ox, oy);
}

static void draw_all(int w, int *b, int more, int update)
{
    _kernel_swi_regs r;
    while (more) {
        draw_rect_of(w, b, update);
        r.r[1] = (intptr_t)b;
        more = swi(Wimp_GetRectangle, &r) ? 0 : r.r[0];
    }
}

void player_redraw(int *b)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_RedrawWindow, &r))
        return;
    draw_all(b[0], b, r.r[0], 0);
}

/* Draws part of the current window now (Wimp_UpdateWindow) */
static void update_box(box_t bx)
{
    int b[11], w = cur_win();
    _kernel_swi_regs r;
    if (!w)
        return;
    b[0] = w;
    b[1] = bx.x0; b[2] = bx.y0; b[3] = bx.x1; b[4] = bx.y1;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_UpdateWindow, &r))
        return;
    draw_all(w, b, r.r[0], 1);
}

static void show_frame_now(void)
{
    pic_refresh();
    update_box(P.pic);
}

static void show_pending(void)
{
    int t0 = now_cs(), s = ov_show_frame();
    if (s == 2)
        return;
    P.ov_pending = 0;
    if (!s)
        show_frame_now();
    P.draw_cs += now_cs() - t0;
    P.draw_n++;
}

static void show_frame(void)
{
    P.ov_pending = 1;
    show_pending();
}

static void bar_refresh(void)
{
    if (bar_visible())
        update_box(P.bar);
}

/* ---- the stats panel (drawn into the picture by reelcore, as !Reel's) ---------- */

#define GRAPH_N 60
static struct {
    float speed[GRAPH_N], act[GRAPH_N], buf[GRAPH_N];
    int n;
    ReelCoreStats prev;
    int prev_cs;
    unsigned prev_draw_n, prev_draw_cs;
    const ReelCore *of;
    char media[4096];
    ReelCorePanel pp;
    char val[REELCORE_PANEL_ROWS][160];
} G;

static const char *media_value(const char *section, const char *label, char *out, int size)
{
    const char *l = G.media;
    int in = 0, n = (int)strlen(label);
    out[0] = 0;
    while (*l) {
        const char *e = strchr(l, '\n');
        int len = e ? (int)(e - l) : (int)strlen(l);
        if (l[0] == '#')
            in = len - 1 == (int)strlen(section) && !strncmp(l + 1, section, len - 1);
        else if (in && len > n && !strncmp(l, label, n) && l[n] == '\t') {
            snprintf(out, size, "%.*s", len - n - 1, l + n + 1);
            break;
        }
        l = e ? e + 1 : l + len;
    }
    return out;
}

static void graph_push(float *a, double v)
{
    if (G.n == GRAPH_N)
        memmove(a, a + 1, (GRAPH_N - 1) * sizeof(*a));
    a[G.n < GRAPH_N ? G.n : GRAPH_N - 1] = (float)v;
}

static void graph_scale(float *out, const float *a)
{
    double m = 0;
    for (int i = 0; i < G.n; i++)
        if (a[i] > m) m = a[i];
    m = m > 0 ? m * 1.1 : 1;
    for (int i = 0; i < GRAPH_N; i++)
        out[i] = i < GRAPH_N - G.n ? 0 : (float)(a[i - (GRAPH_N - G.n)] / m);
}

/* Media info's long names made short (as Reel's panel): a codec's short
   name is in its last brackets ("H.264 / AVC / ... (h264)" -> "h264"); a
   container's long name comes before them */
static char *panel_short(char *t, int codec)
{
    char *o = strrchr(t, '(');
    size_t n = strlen(t);
    if (!o || n < 2 || t[n - 1] != ')')
        return t;
    if (codec) {
        t[n - 1] = 0;
        return o + 1;
    }
    while (o > t && o[-1] == ' ')
        o--;
    *o = 0;
    return t;
}

/* The stats panel: ReelEGL's rows (Reel 0.1.21), drawn into the picture by
   reelcore at the size it's seen (through the overlay too) */
static void panel_update(int sample)
{
    static float g_speed[GRAPH_N], g_act[GRAPH_N], g_buf[GRAPH_N];
    ReelCoreStats st;
    ReelCoreNet ns;
    char a[96], b[96], c[96], d[96];
    int t = now_cs(), fw = 0, fh = 0, i = 0, net;
    double dt, rate, got, ahead;
    unsigned dec, draws;
    time_t now;
    if (!P.stats || !P.v || !P.ready)
        return;
    if (G.of != P.v) {
        reelcore_media_info(P.v, G.media, sizeof(G.media));
        G.of = P.v;
        G.n = 0;
        reelcore_stats(P.v, &G.prev);
        G.prev_cs = t;
    }
    dt = (t - G.prev_cs) / 100.0;
    reelcore_stats(P.v, &st);
    net = reelcore_net(P.v, &ns);
    if (st.bytes_read < G.prev.bytes_read || st.decoded < G.prev.decoded)
        G.prev = st;
    got = (double)(st.bytes_read - G.prev.bytes_read);
    rate = dt > 0 ? got * 8 / dt / 1000 : 0;       /* kbit/s */
    ahead = net ? ns.ahead : st.sound_queued;
    if (sample && dt > 0.5) {
        graph_push(G.speed, rate);
        graph_push(G.act, got / 1024);
        graph_push(G.buf, ahead);
        if (G.n < GRAPH_N) G.n++;
    }
    graph_scale(g_speed, G.speed);
    graph_scale(g_act, G.act);
    graph_scale(g_buf, G.buf);
    memset(&G.pp, 0, sizeof(G.pp));
    reelcore_frame_size(P.v, &fw, &fh);

    media_value("File", "Container", a, sizeof(a));
    snprintf(G.val[i], sizeof(G.val[i]), "%.60s / %s%s%.30s", P.title,
             P.convert ? "Transcoded" : "Direct Play", a[0] ? ", " : "", a[0] ? panel_short(a, 0) : "");
    G.pp.label[i] = "Video / Source"; G.pp.value[i] = G.val[i]; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%dx%d / %u dropped of %u", (P.pic.x1 - P.pic.x0) >> P.xeig,
             (P.pic.y1 - P.pic.y0) >> P.yeig, st.late, st.shown + st.late);
    G.pp.label[i] = "Viewport / Frames"; G.pp.value[i] = G.val[i]; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%dx%d@%.3g / %s", fw, fh, st.fps, ov.shown ? "hardware overlay" : "sprite");
    G.pp.label[i] = "Current Res / Drawn"; G.pp.value[i] = G.val[i]; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%d%%", (int)(P.vol * 100 + 0.5));
    G.pp.label[i] = "Volume"; G.pp.value[i] = G.val[i]; i++;

    media_value("Video", "Codec", a, sizeof(a));
    media_value("Video", "Profile", b, sizeof(b));
    media_value("Audio", "Codec", c, sizeof(c));
    media_value("Audio", "Profile", d, sizeof(d));
    snprintf(G.val[i], sizeof(G.val[i]), "%.30s%s%.20s%s / %.30s%s%.20s%s", a[0] ? panel_short(a, 1) : "none",
             b[0] ? " (" : "", b, b[0] ? ")" : "", c[0] ? panel_short(c, 1) : "none", d[0] ? " (" : "", d, d[0] ? ")" : "");
    G.pp.label[i] = "Codecs"; G.pp.value[i] = G.val[i]; i++;

    media_value("Video", "Colours", a, sizeof(a));
    snprintf(G.val[i], sizeof(G.val[i]), "%.100s", a[0] ? a : "?");
    G.pp.label[i] = "Color"; G.pp.value[i] = G.val[i]; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%.40s, %s", strncmp(P.url, "https:", 6) ? "http" : "https (AcornSSL)",
             net ? (ns.buffering ? "BUFFERING" : ns.ended ? "all read" : "reading ahead") : "a file");
    G.pp.label[i] = "Connection"; G.pp.value[i] = G.val[i]; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%.0f Kbps", rate);
    G.pp.label[i] = "Connection Speed"; G.pp.value[i] = G.val[i];
    G.pp.graph[i] = g_speed; G.pp.graph_rgb[i] = 0x1E88E5; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%.0f KB", got / 1024);
    G.pp.label[i] = "Network Activity"; G.pp.value[i] = G.val[i];
    G.pp.graph[i] = g_act; G.pp.graph_rgb[i] = 0x26A69A; i++;

    snprintf(G.val[i], sizeof(G.val[i]), "%.2f s", ahead);
    G.pp.label[i] = "Buffer Health"; G.pp.value[i] = G.val[i];
    G.pp.graph[i] = g_buf; G.pp.graph_rgb[i] = 0xFFB300; i++;

    dec = st.decoded - G.prev.decoded;
    draws = P.draw_n - G.prev_draw_n;
    /* sync: the picture against the sound. reelcore's position counts from
       the stream's start_time and its clock doesn't, so for a stream that
       doesn't start at 0 (HLS; a converted stream started part way) the
       difference is that start, not a real lag (test5 on the Pi: -86789 ms,
       with 4 of 333 pictures late). Measured from the first picture's. */
    snprintf(G.val[i], sizeof(G.val[i]), "decode %.1f ms, draw %.1f ms, %.0f%% busy, sync %+d ms",
             dec ? (st.decode_time - G.prev.decode_time) * 1000 / dec : 0.0,
             draws ? (P.draw_cs - G.prev_draw_cs) * 10.0 / draws : 0.0,
             dt > 0 ? (st.decode_time - G.prev.decode_time) * 100 / dt : 0.0,
             (int)((st.position - st.clock - P.sync0) * 1000));
    G.pp.label[i] = "Timing"; G.pp.value[i] = G.val[i]; i++;

    if (st.auto_fast || st.skip_level) {          /* what's being left out to keep up */
        snprintf(G.val[i], sizeof(G.val[i]), "%s%s%s", st.auto_fast ? "no deblocking" : "",
                 st.auto_fast && st.skip_level ? ", " : "",
                 st.skip_level == 2 ? "keyframes only" : st.skip_level ? "non-reference frames skipped" : "");
        G.pp.label[i] = "Keeping Up"; G.pp.value[i] = G.val[i]; i++;
    }

    now = time(NULL);
    strftime(G.val[i], sizeof(G.val[i]), "%a %b %d %Y %H:%M:%S", localtime(&now));
    G.pp.label[i] = "Date"; G.pp.value[i] = G.val[i]; i++;

    G.pp.rows = i;
    G.pp.graph_n = GRAPH_N;
    /* through the overlay the display scales the picture: the panel is drawn
       that much bigger in the frame, so it's the same size on the screen */
    G.pp.yuv_scale = ov.shown && ov.placed[1] > 0 && ov.fw > 0 ? (double)ov.fw / ov.placed[1] : 1;
    reelcore_set_panel(P.v, &G.pp);
    if (sample && dt > 0.5) {
        G.prev = st;
        G.prev_cs = t;
        G.prev_draw_n = P.draw_n;
        G.prev_draw_cs = P.draw_cs;
    }
}

/* ---- full screen ------------------------------------------------------------ */

static int create_full_window(void)
{
    struct {
        box_t vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        box_t ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        int title[3];
        int nicons;
    } w;
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    w.vis.x1 = P.scr_w; w.vis.y1 = P.scr_h;
    w.behind = -1;
    w.flags = (int)0x80000040u;     /* new format, no furniture, may cover the icon bar */
    w.tfg = 0xFF;
    w.wfg = 7; w.wbg = 0xFF;        /* the task draws it all */
    w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -P.scr_h; w.ext.x1 = P.scr_w; w.ext.y1 = 0;
    w.wbutton = 10 << 12;           /* click, and double-click */
    w.sprites = 1;
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    P.full = r.r[0];
    return 0;
}

static void full_delete(void)
{
    _kernel_swi_regs r;
    if (!P.full)
        return;
    r.r[1] = (intptr_t)&P.full;
    swi(Wimp_DeleteWindow, &r);
    P.full = 0;
}

void player_set_fullscreen(int on)
{
    int b[9];
    _kernel_swi_regs r;
    on = !!on;
    if (on == P.fullscreen || !P.v)
        return;
    if (on && P.mini)
        player_set_mini(0);
    lg("full screen %s", on ? "on" : "off");
    ov_hide();
    if (on) {
        read_screen();
        if (P.full && (P.scr_w != P.vis_w || P.scr_h != P.vis_h))
            full_delete();          /* made for another screen size */
        if (!P.full && create_full_window() < 0)
            return;
        P.fullscreen = 1;
        P.bar_shown = 0;
        {                           /* the bar shows when the pointer moves from here */
            int p[5];
            r.r[1] = (intptr_t)p;
            if (!swi(Wimp_GetPointerInfo, &r)) {
                P.ptr_x = p[0];
                P.ptr_y = p[1];
            }
            P.ptr_cs = now_cs();
        }
        b[0] = P.full; b[1] = 0; b[2] = 0; b[3] = P.scr_w; b[4] = P.scr_h;
        b[5] = 0; b[6] = 0; b[7] = -1;
        r.r[1] = (intptr_t)b;
        swi(Wimp_OpenWindow, &r);
        layout_boxes();
        pic_make();
        pic_refresh();
        force_redraw(P.full, 0, -P.scr_h, P.scr_w, 0);
        set_caret(P.full);
    } else {
        pointer_show(1);
        r.r[1] = (intptr_t)&P.full;
        swi(Wimp_CloseWindow, &r);
        P.fullscreen = 0;
        layout_boxes();
        pic_make();
        pic_refresh();
        force_redraw(P.win, 0, -0x7FFFFFF, 0x7FFFFFF, 0);
        set_caret(P.win);
    }
}

int player_fullscreen(void) { return P.fullscreen; }
int player_owns(int w) { return w && (w == P.full || w == P.mini_win); }

/* ---- the mini player (as Reel's) ------------------------------------------------------ */

/* No title bar or other furniture: drag the picture to move it, the grip
   (bottom right) to resize it, double-click it for the normal window */
static int create_mini_window(void)
{
    struct {
        box_t vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        box_t ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        int title[3];
        int nicons;
    } w;
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    w.vis.x1 = MINI_W; w.vis.y1 = 400;
    w.behind = -1;
    w.flags = (int)0x80000002u;     /* new format, moveable; no furniture */
    w.tfg = 0xFF;                   /* no title */
    w.wfg = 7; w.wbg = 0xFF;        /* the task draws it all */
    w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -8192; w.ext.x1 = 8192; w.ext.y1 = 0;
    w.wbutton = 10 << 12;           /* click, drag, double-click */
    w.sprites = 1;
    w.minw = MINI_MIN_W; w.minh = MINI_BAR + 80;
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    P.mini_win = r.r[0];
    return 0;
}

/* The top of the icon bar, OS units (the icon bar is window -2) */
static int iconbar_top(void)
{
    int b[9];
    window_state(-2, b);
    return b[4] > 0 && b[4] < 512 ? b[4] : 134;
}

/* The mini player's picture height for a width: the video's shape */
static int mini_pic_h(int vw)
{
    int ph = P.v && reelcore_width(P.v) > 0 ? (int)((long long)vw * reelcore_height(P.v) / reelcore_width(P.v))
                                            : vw * 9 / 16;
    int most = P.scr_h - MINI_BAR - 160;
    if (ph < vw / 4) ph = vw / 4;   /* very wide, or tall: letterboxed */
    if (ph > vw * 3 / 4) ph = vw * 3 / 4;
    if (ph > most) ph = most;
    return ph - ph % (1 << P.yeig);
}

static void open_at(int w, int x0, int y0, int x1, int y1, int behind)
{
    int b[8] = { w, x0, y0, x1, y1, 0, 0, behind };
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
}

/* Opens (or moves and resizes) it for the video: the width it was last
   given, as tall as the video's shape needs, in the bottom right just
   above the icon bar, or where it was dragged to */
static void mini_show(void)
{
    int vw = P.mini_w > 0 ? P.mini_w : MINI_W, vh, x1, y0;
    read_screen();
    if (vw > P.scr_w) vw = P.scr_w;
    vh = mini_pic_h(vw) + MINI_BAR;
    x1 = P.scr_w - P.mini_right;
    y0 = P.mini_bottom >= 0 ? P.mini_bottom : iconbar_top() + MINI_LIFT;
    if (x1 > P.scr_w) x1 = P.scr_w;         /* on the screen, even after a mode change */
    if (x1 < vw) x1 = vw;
    if (y0 + vh > P.scr_h) y0 = P.scr_h - vh;
    if (y0 < 0) y0 = 0;
    lg("mini player %dx%d OS units at %d,%d", vw, vh, x1 - vw, y0);
    open_at(P.mini_win, x1 - vw, y0, x1, y0 + vh, -1);
    ov.placed[0] = 0;
    layout_boxes();
    pic_make();
    pic_refresh();
    force_redraw(P.mini_win, 0, -8192, 8192, 0);
}

/* The mini player decodes a little less: no deblocking on the pictures
   nothing is predicted from (invisible at that size), as Reel's */
static void apply_fast(void)
{
    if (P.v)
        reelcore_set_fast(P.v, P.mini ? REELCORE_FAST_LIGHT : REELCORE_FAST_OFF);
}

void player_set_mini(int on)
{
    _kernel_swi_regs r;
    on = !!on;
    if (on == P.mini || !P.v || !P.win)
        return;
    if (on && P.fullscreen)
        player_set_fullscreen(0);
    lg("mini player %s", on ? "on" : "off");
    ov_hide();
    ov.placed[0] = 0;
    if (on) {
        if (!P.mini_win && create_mini_window() < 0)
            return;
        window_state(P.win, P.main_st);     /* to come back to */
        r.r[1] = (intptr_t)&P.win;
        swi(Wimp_CloseWindow, &r);
        P.mini = 1;
        P.ontop_cs = now_cs();
        apply_fast();
        mini_show();
        set_caret(P.mini_win);
    } else {
        r.r[1] = (intptr_t)&P.mini_win;
        swi(Wimp_CloseWindow, &r);
        P.mini = 0;
        apply_fast();
        open_at(P.win, P.main_st[1], P.main_st[2], P.main_st[3], P.main_st[4], -1);
        layout_boxes();
        pic_make();
        pic_refresh();
        force_redraw(P.win, 0, -0x7FFFFFF, 0x7FFFFFF, 0);
        set_caret(P.win);
    }
}

int player_mini(void) { return P.mini; }
int player_ontop(void) { return P.ontop; }
void player_set_ontop(int on) { P.ontop = !!on; P.ontop_cs = now_cs(); }

void player_mini_place(int *w, int *right, int *bottom)
{
    *w = P.mini_w;
    *right = P.mini_right;
    *bottom = P.mini_bottom;
}

void player_set_mini_place(int w, int right, int bottom)
{
    P.mini_w = w >= MINI_MIN_W && w <= 4096 ? w : MINI_W;
    P.mini_right = right >= 0 ? right : MINI_EDGE;
    P.mini_bottom = bottom >= -1 ? bottom : -1;
}

/* Open_Window_Request for the mini player: the grip keeps the video's
   shape (top left put); where it's dragged to is kept. 1 = the place or
   size changed (the caller saves the choices). */
int player_mini_open_request(int *b)
{
    _kernel_swi_regs r;
    int st[9], moved, resized;
    window_state(P.mini_win, st);
    resized = b[3] - b[1] != st[3] - st[1] || b[4] - b[2] != st[4] - st[2];
    if (resized) {
        int vw = b[3] - b[1];
        if (vw < MINI_MIN_W)
            b[3] = b[1] + (vw = MINI_MIN_W);
        b[2] = b[4] - (mini_pic_h(vw) + MINI_BAR);
        P.mini_w = vw;
    }
    b[5] = b[6] = 0;
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
    moved = P.mini_right != P.scr_w - b[3] || P.mini_bottom != b[2];
    P.mini_right = P.scr_w - b[3] > 0 ? P.scr_w - b[3] : 0;
    P.mini_bottom = b[2] > 0 ? b[2] : 0;
    if (resized) {
        ov.placed[0] = 0;
        layout_boxes();
        pic_make();
        pic_refresh();
        force_redraw(P.mini_win, 0, -8192, 8192, 0);
    }
    return moved || resized;
}

/* Keep on top: while playing, about once a second, it comes back to the
   front if another window has been opened over it (never taking the caret) */
static void mini_keep_on_top(int t)
{
    int st[9];
    _kernel_swi_regs r;
    if (!P.mini || !P.ontop || t - P.ontop_cs < ONTOP_CS)
        return;
    P.ontop_cs = t;
    window_state(P.mini_win, st);
    if (st[7] == -1)
        return;
    st[7] = -1;
    r.r[1] = (intptr_t)st;
    swi(Wimp_OpenWindow, &r);
    lg("mini player: back on top");
}

/* ---- opening, closing ------------------------------------------------------- */

void player_init(int task, int overlay, int pic_mode, double volume)
{
    P.task = task;
    if (!P.mini_w)
        player_set_mini_place(MINI_W, MINI_EDGE, -1);
    P.hw = overlay;
    P.pic_mode = pic_mode >= 0 && pic_mode < PIC_COUNT ? pic_mode : PIC_FIT;
    P.vol = volume >= 0 && volume <= 1 ? volume : 1.0;
}

static void close_video(int keep_full);

int player_open(const player_src *s, int win)
{
    ReelCoreSource src;
    int keep = (P.fullscreen || P.mini) && P.win == win;    /* the next part (a seek, the next episode): stay full screen,
                                                               or in the mini player */
    close_video(keep);
    log_open();
    reelcore_set_log(P.log ? ff_log : NULL, 0);
    read_screen();
    P.win = win;
    snprintf(P.url, sizeof(P.url), "%s", s->url);
    snprintf(P.headers, sizeof(P.headers), "%s", s->headers ? s->headers : "");
    snprintf(P.agent, sizeof(P.agent), "%s", s->user_agent ? s->user_agent : "");
    snprintf(P.title, sizeof(P.title), "%s", s->title ? s->title : "");
    P.base = s->base > 0 ? s->base : 0;
    P.start = s->start > 0 ? s->start : 0;
    P.duration = s->duration > 0 ? s->duration : 0;
    P.convert = s->convert;
    P.subs = s->subs;
    P.based = 0;
    P.ready = P.ended = P.failed = 0;
    P.error[0] = 0;
    P.note[0] = 0;
    P.card = 0;
    P.skip[0] = 0;
    P.nchap = 0;
    for (int i = 0; s->chapters && i < s->nchapters && P.nchap < (int)(sizeof(P.chap) / sizeof(P.chap[0])); i++)
        P.chap[P.nchap++] = s->chapters[i];
    P.opened_cs = now_cs();
    memset(&src, 0, sizeof(src));
    src.url = P.url;
    src.headers = P.headers[0] ? P.headers : NULL;
    src.user_agent = P.agent[0] ? P.agent : NULL;
    src.title = P.title;
    lg("open %s (%s; starts at %.1f s, from %.1f s)", P.title, P.convert ? "transcoded" : "direct play",
       P.base, P.start);
    P.v = reelcore_open_source(&src, REELCORE_ASYNC);
    layout_boxes();
    pic_make();
    if (!P.v) {
        P.failed = 1;
        snprintf(P.error, sizeof(P.error), "%s", reelcore_last_error());
        lg("couldn't start: %s", P.error);
        return -1;
    }
    force_redraw(cur_win(), 0, -0x7FFFFFF, 0x7FFFFFF, 0);
    return 0;
}

static void close_video(int keep_full)
{
    if (!P.v && !P.full && !P.failed && !P.mini)
        return;
    lg("close");
    pointer_show(1);
    ov_destroy();
    if (P.mini && !keep_full) {     /* the caller opens its window again */
        _kernel_swi_regs r;
        r.r[1] = (intptr_t)&P.mini_win;
        swi(Wimp_CloseWindow, &r);
        P.mini = 0;
    }
    if (P.fullscreen && !keep_full) {
        _kernel_swi_regs r;
        r.r[1] = (intptr_t)&P.full;
        swi(Wimp_CloseWindow, &r);
        P.fullscreen = 0;
    }
    if (!keep_full)
        full_delete();
    if (P.v)
        reelcore_close(P.v);
    P.v = NULL;
    G.of = NULL;
    P.ready = P.ended = P.failed = 0;
    P.card = 0;
    sprite_free();
    if (P.log && !keep_full) {
        reelcore_set_log(NULL, 0);
        fclose(P.log);
        P.log = NULL;
    }
}

void player_close(void)
{
    close_video(0);
}

int player_active(void) { return P.v != NULL || P.failed; }
int player_ready(void) { return P.ready; }
const char *player_error(void) { return P.error; }
int player_ended(void) { return P.ended; }

/* The stream's times against the video's: a converted stream that started
   part way may count from 0, or from where it started */
static double stream_base(void)
{
    return P.based == 2 ? 0 : P.base;
}

double player_position(void)
{
    if (!P.v || !P.ready)
        return P.base + P.start;
    return stream_base() + reelcore_position(P.v);
}

double player_duration(void)
{
    double d = P.v && P.ready ? reelcore_duration(P.v) : 0;
    if (P.duration > 0 && (P.convert || d <= 0))
        return P.duration;
    return d > 0 ? d + stream_base() : P.duration;
}

int player_paused(void) { return P.v && P.ready && reelcore_paused(P.v); }

void player_pause(int paused)
{
    if (!P.v || !P.ready)
        return;
    if (P.ended && !paused) {
        P.ended = 0;
        player_seek(0);
    }
    reelcore_pause(P.v, paused);
    if (paused)
        pointer_show(1);
    lg("%s at %.2f", paused ? "pause" : "play", player_position());
    if (paused)
        ov_hide_and_draw();
    bar_refresh();
}

void player_seek(double t)
{
    double d = player_duration();
    if (!P.v)
        return;
    if (t < 0) t = 0;
    if (d > 0 && t > d - 1) t = d - 1;
    P.ended = 0;
    /* a converted stream too: the server's playlist covers the whole video,
       and the server starts converting from the segment asked for (as the
       Plex apps seek; test7 started a new stream there instead, which the
       server could answer with 400 Bad Request) */
    if (!P.ready) {
        P.start = t;
        return;
    }
    lg("seek to %.1f", t);
    reelcore_seek(P.v, t - stream_base());
    bar_refresh();
}

/* ---- null events -------------------------------------------------------------- */

static void opened(void)
{
    P.ready = 1;
    lg("opened in %.1f s: %dx%d, %.1f s", (now_cs() - P.opened_cs) / 100.0, reelcore_width(P.v),
       reelcore_height(P.v), reelcore_duration(P.v));
    {
        char info[256];
        reelcore_info(P.v, info, sizeof(info));
        lg("streams: %s", info);
    }
    reelcore_set_volume(P.v, P.vol * P.vol);
    if (P.mini) {
        apply_fast();
        mini_show();                /* the video's shape */
    }
    if (P.start > 0)
        reelcore_seek(P.v, P.start - P.base);
    pic_make();
    if (P.stats)
        panel_update(0);
}

int player_poll_cs(void)
{
    ReelCoreNet ns;
    if (!P.v)
        return -1;
    if (!P.ready || P.ov_pending)
        return 0;
    if (P.ended || reelcore_paused(P.v))
        return P.fullscreen || P.card || P.note[0] ? 25 : -1;  /* the bar and the card still need time */
    /* reading ahead over the network: the reader thread only runs while
       we're paged in, so don't sleep while it's short */
    if (reelcore_net(P.v, &ns) && (ns.buffering || ns.opening))
        return 0;
    return P.idle_cs > 0 ? P.idle_cs : 0;
}

/* The pointer shown again (shape 1), or hidden (OS_Byte 106) */
static void pointer_show(int on)
{
    _kernel_swi_regs r;
    if (on == !P.ptr_hidden)
        return;
    r.r[0] = 106;
    r.r[1] = on ? 1 : 0;
    swi(OS_Byte, &r);
    P.ptr_hidden = !on;
    lg("pointer %s", on ? "shown" : "hidden");
}

static void watch_pointer(void)
{
    int p[5];
    _kernel_swi_regs r;
    if (!P.fullscreen)
        return;
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r))
        return;
    /* full screen: the pointer goes after HIDE_POINTER while it's still over
       the picture and playing (PlexRO$NoHidePointer: never), back when it moves */
    if (p[0] == P.ptr_x && p[1] == P.ptr_y && !P.ptr_hidden && p[3] == P.full && !reelcore_paused(P.v) &&
        !P.ended && !P.card && now_cs() - P.ptr_cs >= HIDE_POINTER && !getenv("PlexRO$NoHidePointer"))
        pointer_show(0);
    if (p[0] != P.ptr_x || p[1] != P.ptr_y) {
        P.ptr_x = p[0];
        P.ptr_y = p[1];
        P.ptr_cs = now_cs();
        pointer_show(1);
        P.bar_until = now_cs() + SHOW_BAR;
        if (!P.bar_shown) {
            P.bar_shown = 1;
            ov.placed[0] = 0;       /* the overlay's clip changes */
            update_box(P.bar);
        }
    } else if (P.bar_shown && now_cs() - P.bar_until >= 0 && !reelcore_paused(P.v) && !P.ended && !P.skip[0]) {
        P.bar_shown = 0;
        ov.placed[0] = 0;
        force_redraw(P.full, P.bar.x0, P.bar.y0, P.bar.x1, P.bar.y1);
    }
}

int player_null(void)
{
    int r2, t = now_cs();
    if (!P.v)
        return PE_NONE;
    if (!P.ready) {
        r2 = reelcore_update(P.v);
        if (r2 == REELCORE_OPENING) {
            if (t - P.last_bar_cs >= 100) {
                P.last_bar_cs = t;
                bar_refresh();
            }
            return PE_NONE;
        }
        if (r2 == REELCORE_FAILED || reelcore_ready(P.v) < 0) {
            P.failed = 1;
            snprintf(P.error, sizeof(P.error), "Can't play it: %s", reelcore_last_error());
            lg("%s", P.error);
            reelcore_close(P.v);
            P.v = NULL;
            force_redraw(cur_win(), 0, -0x7FFFFFF, 0x7FFFFFF, 0);
            return PE_FAILED;
        }
        opened();
        return PE_READY;
    }
    watch_pointer();
    if (P.ended || reelcore_paused(P.v)) {
        if (P.note[0] && t - P.note_until >= 0) {
            P.note[0] = 0;
            bar_refresh();
        }
        return PE_NONE;
    }
    mini_keep_on_top(t);
    r2 = reelcore_update(P.v);
    P.idle_cs = (int)(reelcore_idle_time(P.v) * 100);
    if (r2 == REELCORE_NEW_FRAME) {
        if (!P.based) {             /* a converted stream counting from where it started? */
            double p = reelcore_position(P.v);
            ReelCoreStats st;
            reelcore_stats(P.v, &st);
            P.sync0 = st.position - st.clock;
            P.based = P.base > 5 && p > P.base - 5 ? 2 : 1;
            if (P.base > 0)
                lg("first picture at %.2f s: the stream counts from %s", p, P.based == 2 ? "the video's start" : "0");
        }
        show_frame();
    } else if (P.ov_pending && r2 == REELCORE_SAME_FRAME)
        show_pending();
    if (r2 == REELCORE_END) {
        lg("the end");
        P.ended = 1;
        ov_hide_and_draw();
        bar_refresh();
        return PE_END;
    }
    if (t - P.log_cs >= 0 && P.log) {
        static int last;
        if (t - last >= 100) {
            char d[300];
            ReelCoreNet ns;
            last = t;
            reelcore_debug(P.v, d, sizeof(d));
            if (reelcore_net(P.v, &ns))
                lg("%s; net: %s%.1f s ahead, %u KB, %lld read", d, ns.buffering ? "BUFFERING, " : "", ns.ahead,
                   ns.bytes_ahead / 1024, ns.bytes_read);
            else
                lg("%s", d);
        }
    }
    if (P.stats && t - G.prev_cs >= 100)
        panel_update(1);
    if (P.note[0] && t - P.note_until >= 0)
        P.note[0] = 0;
    if (t - P.last_bar_cs >= 25) {  /* the time and the position bar */
        P.last_bar_cs = t;
        if (bar_visible()) {
            char old[64];
            snprintf(old, sizeof(old), "%s", P.time_text);
            bar_text();
            if (strcmp(old, P.time_text) || track_fill() != P.fill_x)
                update_box(P.bar);
        }
    }
    return PE_NONE;
}

/* ---- the window: layout, clicks, keys -------------------------------------------- */

void player_layout(void)
{
    if (!P.win || P.fullscreen || P.mini)
        return;
    {
        int ow = P.vis_w, oh = P.vis_h;
        layout_boxes();
        if (ow != P.vis_w || oh != P.vis_h) {
            pic_make();
            pic_refresh();
            ov.placed[0] = 0;
            force_redraw(P.win, 0, -0x7FFFFFF, 0x7FFFFFF, 0);
        }
    }
}

void player_mode_change(void)
{
    read_screen();
    ov_destroy();                   /* the old one isn't freed by the mode change */
    ov.failed = 0;
    if (P.fullscreen) {
        player_set_fullscreen(0);
        player_set_fullscreen(1);
    } else if (P.mini)
        mini_show();                /* back on the screen, above the icon bar */
    else
        player_layout();
}

static int in_box(const box_t *b, int x, int y) { return x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1; }

static int toggle_pause(void)
{
    if (!P.v || !P.ready)
        return PE_NONE;
    player_pause(!(reelcore_paused(P.v) || P.ended));
    return reelcore_paused(P.v) ? PE_PAUSED : PE_PLAYING;
}

int player_click(const int *b)
{
    int st[9], x, y, buttons = b[2], w = b[3];
    if (w != cur_win())
        return PE_NONE;
    pointer_show(1);
    P.ptr_cs = now_cs();
    if (buttons & 2)
        return PE_MENU;
    window_state(w, st);
    x = b[0] - (st[1] - st[5]);
    y = b[1] - (st[4] - st[6]);
    if (P.fullscreen && !P.bar_shown) {             /* the first move or click shows the bar */
        P.ptr_x = P.ptr_y = -1;
        watch_pointer();
    }
    /* buttons act on the click (0x400 Select, 0x100 Adjust), not the double-click after it */
    if (P.card) {
        if (in_box(&P.btn[PB_CARD1], x, y))
            return buttons & 0x500 ? PE_CARD_1 : PE_NONE;
        if (in_box(&P.btn[PB_CARD2], x, y))
            return buttons & 0x500 ? PE_CARD_2 : PE_NONE;
    }
    if (P.mini) {
        int d[10];
        _kernel_swi_regs r;
        memset(d, 0, sizeof(d));
        d[0] = w;
        if (in_box(&P.btn[PB_GRIP], x, y) && (buttons & 0x550)) {
            d[1] = 2;                               /* the Wimp resizes it: Open_Window_Requests follow */
            r.r[1] = (intptr_t)d;
            swi(Wimp_DragBox, &r);
            return PE_NONE;
        }
        if (in_box(&P.pic, x, y)) {
            if (buttons & 0x50) {                   /* drag the picture: move it */
                d[1] = 1;
                r.r[1] = (intptr_t)d;
                swi(Wimp_DragBox, &r);
            } else if (buttons & 4)                 /* double-click: the normal window */
                player_set_mini(0);
            return PE_NONE;
        }
        if (!(buttons & 0x500))
            return PE_NONE;
        if (in_box(&P.btn[PB_PLAY], x, y))
            return toggle_pause();
        if (in_box(&P.btn[PB_NORMAL], x, y))
            player_set_mini(0);
        else if (x >= P.btn[PB_TRACK].x0 - 8 && x < P.btn[PB_TRACK].x1 + 8 && y >= P.btn[PB_TRACK].y0 - 12 &&
                 y < P.btn[PB_TRACK].y1 + 12 && player_duration() > 0) {
            const box_t *t = &P.btn[PB_TRACK];
            int cx = x < t->x0 ? t->x0 : x > t->x1 ? t->x1 : x;
            player_seek(player_duration() * (cx - t->x0) / (t->x1 - t->x0));
        }
        return PE_NONE;
    }
    if (bar_visible() && in_box(&P.bar, x, y)) {
        if (!(buttons & 0x500))
            return PE_NONE;
        if (in_box(&P.btn[PB_SKIP], x, y))
            return PE_SKIP;
        if (in_box(&P.btn[PB_BACK], x, y))
            return PE_BACK;
        if (in_box(&P.btn[PB_PLAY], x, y))
            return toggle_pause();
        if (in_box(&P.btn[PB_REW], x, y))
            player_seek(player_position() - 10);
        else if (in_box(&P.btn[PB_FWD], x, y))
            player_seek(player_position() + 10);
        else if (in_box(&P.btn[PB_STATS], x, y))
            player_set_stats(!P.stats);
        else if (in_box(&P.btn[PB_SUBS], x, y))
            return PE_SUBS;
        else if (in_box(&P.btn[PB_FULL], x, y))
            player_set_fullscreen(!P.fullscreen);
        else if (x >= P.btn[PB_TRACK].x0 - 8 && x < P.btn[PB_TRACK].x1 + 8 && y >= P.btn[PB_TRACK].y0 - 12 &&
                 y < P.btn[PB_TRACK].y1 + 12 && player_duration() > 0) {
            const box_t *t = &P.btn[PB_TRACK];
            int cx = x < t->x0 ? t->x0 : x > t->x1 ? t->x1 : x;
            player_seek(player_duration() * (cx - t->x0) / (t->x1 - t->x0));
        }
        return PE_NONE;
    }
    if (in_box(&P.pic, x, y) && !P.card) {
        if (buttons & 0x400)                            /* a click: pause, or play */
            return toggle_pause();
        if (buttons & 4) {                              /* the double-click after it: full screen, */
            int e = toggle_pause();                     /* and the click's pause undone */
            player_set_fullscreen(!P.fullscreen);
            return e;
        }
    }
    return PE_NONE;
}

int player_key(int k)
{
    switch (k) {
    case ' ':
        return toggle_pause();
    case 0x18C: player_seek(player_position() - 10); return PE_NONE;     /* Left */
    case 0x18D: player_seek(player_position() + 10); return PE_NONE;     /* Right */
    case 0x18E: player_seek(player_position() - 60); return PE_NONE;     /* Down */
    case 0x18F: player_seek(player_position() + 60); return PE_NONE;     /* Up */
    case 'f': case 'F':
        player_set_fullscreen(!P.fullscreen);
        return PE_NONE;
    case 'm': case 'M':
        player_set_mini(!P.mini);
        return PE_NONE;
    case 's': case 'S':
        player_set_stats(!P.stats);
        return PE_NONE;
    case '+': case '=':
        player_set_volume(P.vol + 0.1);
        return PE_NONE;
    case '-': case '_':
        player_set_volume(P.vol - 0.1);
        return PE_NONE;
    case 13:
        return P.card ? PE_CARD_1 : P.skip[0] ? PE_SKIP : PE_NONE;
    case 0x1B:
        if (P.fullscreen) {
            player_set_fullscreen(0);
            return PE_NONE;
        }
        if (P.mini) {
            player_set_mini(0);
            return PE_NONE;
        }
        return P.card ? PE_CARD_2 : PE_BACK;
    case 8: case 0x7F:
        return PE_BACK;
    }
    return -1;
}

/* ---- settings ---------------------------------------------------------------------- */

int player_tracks(void) { return P.v && P.ready ? reelcore_audio_tracks(P.v) : 0; }
int player_track(void) { return P.v && P.ready ? reelcore_audio_track(P.v) : -1; }

void player_track_name(int i, char *buf, int size)
{
    buf[0] = 0;
    if (P.v && P.ready)
        reelcore_audio_track_name(P.v, i, buf, size);
}

void player_set_track(int i)
{
    if (P.v && P.ready && i != reelcore_audio_track(P.v)) {
        lg("sound track %d", i);
        reelcore_set_audio_track(P.v, i);
    }
}

int player_sub_tracks(void) { return P.v && P.ready ? reelcore_subtitle_tracks(P.v) : 0; }
int player_sub_track(void) { return P.v && P.ready ? reelcore_subtitle_track(P.v) : -1; }

int player_set_sub(int i)
{
    if (!P.v || !P.ready)
        return -1;
    lg("subtitles %d", i);
    if (i == reelcore_subtitle_track(P.v))
        return 0;
    return reelcore_set_subtitle_track(P.v, i) < 0 ? -1 : 0;
}

int player_add_sub_file(const char *path)
{
    int t;
    if (!P.v || !P.ready)
        return -1;
    t = reelcore_add_subtitle_file(P.v, path);
    lg("subtitle file %s: %d", path, t);
    return t;
}

double player_volume(void) { return P.vol; }

void player_set_volume(double v)
{
    char t[40];
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    P.vol = v;
    if (P.v && P.ready)
        reelcore_set_volume(P.v, v * v);        /* the bar's volume, squared: sounds even */
    snprintf(t, sizeof(t), "Volume %d%%", (int)(v * 100 + 0.5));
    player_note(t);
}

int player_stats(void) { return P.stats; }

void player_set_stats(int on)
{
    P.stats = !!on;
    if (!P.v || !P.ready) {
        bar_refresh();
        return;
    }
    if (P.stats) {
        G.of = NULL;
        panel_update(0);
    } else
        reelcore_set_panel(P.v, NULL);
    if (reelcore_paused(P.v) || P.ended || !ov.shown) {
        pic_refresh();
        force_redraw(cur_win(), P.pic.x0, P.pic.y0, P.pic.x1, P.pic.y1);
    }
    bar_refresh();
}

int player_overlay(void) { return P.hw; }
int player_overlay_shown(void) { return ov.shown; }

void player_set_overlay(int on)
{
    P.hw = !!on;
    if (!P.hw) {
        ov_hide_and_draw();
        ov_destroy();
    }
    ov.failed = 0;
}

int player_pic_mode(void) { return P.pic_mode; }

void player_set_pic_mode(int m)
{
    if (m < 0 || m >= PIC_COUNT)
        return;
    P.pic_mode = m;
    ov.placed[0] = 0;
    if (P.v && P.ready) {
        pic_refresh();
        force_redraw(cur_win(), P.pic.x0, P.pic.y0, P.pic.x1, P.pic.y1);
    }
}

void player_card(const char *heading, const char *line, const char *line2, const char *b1, const char *b2)
{
    int was = P.card;
    if (!heading) {
        P.card = 0;
    } else {
        P.card = 1;
        snprintf(P.card_head, sizeof(P.card_head), "%s", heading);
        snprintf(P.card_line, sizeof(P.card_line), "%s", line ? line : "");
        snprintf(P.card_line2, sizeof(P.card_line2), "%s", line2 ? line2 : "");
        snprintf(P.card_b1, sizeof(P.card_b1), "%s", b1 ? b1 : "OK");
        snprintf(P.card_b2, sizeof(P.card_b2), "%s", b2 ? b2 : "Cancel");
        ov_hide_and_draw();
    }
    if (cur_win() && (was || P.card))
        force_redraw(cur_win(), P.pic.x0, P.pic.y0, P.pic.x1, P.pic.y1);
}

void player_skip(const char *label)
{
    if (!strcmp(P.skip, label ? label : ""))
        return;
    snprintf(P.skip, sizeof(P.skip), "%s", label ? label : "");
    if (P.mini || !cur_win())
        return;                     /* no room: Return does it */
    layout_boxes();
    if (P.fullscreen && P.skip[0] && !P.bar_shown) {
        P.bar_shown = 1;
        P.bar_until = now_cs() + SHOW_BAR;
        ov.placed[0] = 0;           /* the overlay's clip changes */
    }
    if (bar_visible())
        update_box(P.bar);
}

const char *player_skip_label(void) { return P.skip; }

void player_note(const char *text)
{
    snprintf(P.note, sizeof(P.note), "%s", text ? text : "");
    P.note_until = now_cs() + NOTE_CS;
    bar_text();
    bar_refresh();
}

#ifdef PLEXRO_TEST
int player_test_button_xy(int id, int *x, int *y)
{
    int st[9], w = cur_win();
    if (id <= 0 || id >= PB_COUNT || !w)
        return -1;
    window_state(w, st);
    if (id == PB_TRACK) {                   /* its middle, on the bar itself */
        *x = (P.btn[id].x0 + P.btn[id].x1) / 2 + st[1] - st[5];
        *y = (P.btn[id].y0 + P.btn[id].y1) / 2 + st[4] - st[6];
        return 0;
    }
    *x = (P.btn[id].x0 + P.btn[id].x1) / 2 + st[1] - st[5];
    *y = (P.btn[id].y0 + P.btn[id].y1) / 2 + st[4] - st[6];
    return 0;
}
const char *player_test_time(void) { bar_text(); return P.time_text; }
int player_test_panel_rows(void) { return P.stats ? G.pp.rows : 0; }
const char *player_test_panel(int row, int value)
{
    return row >= 0 && row < G.pp.rows ? (value ? G.pp.value[row] : G.pp.label[row]) : "";
}
int player_test_sprite_plots(void) { return (int)P.sprite_plots; }
int player_test_idle(void) { return P.idle_cs; }
#endif
