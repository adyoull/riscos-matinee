/*
 * ui_test.c - Matinee's front end (src/ui.c) against a scripted fake Wimp
 * and fakeplex.py, with the real plex.c, caps.c and handoff.c (net_sock.c
 * stands in for FFmpeg's avio). Built for arm-linux and run under qemu
 * (Wimp blocks hold 32-bit pointers), by tests/host/run.sh:
 *   ui_test PORT OUTDIR
 *
 * The script: Select on the icon opens the sign-in window with a code;
 * the checks (Wimp_PollIdle every 2 s) sign in, pick the server and open
 * the browser; posters are fetched a tile at a time and drawn into
 * sprites; libraries, shows and seasons open and Back returns; a video is
 * handed to ReelEGL (DataOpen to that task alone, deleted on DataLoadAck),
 * or ReelEGL/Reel is started when it isn't running or doesn't claim it;
 * Reel's own sources.c reads every hand-off file; Resume and Play from
 * start; Mark watched; Save original file with the DataSave protocol, 256KB
 * a null event, and Stop saving; the 4.7GB DVD rip refused; the Choices
 * file; a server typed by hand; Sign out; one copy only; Quit.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "kernel.h"
#include "ui.h"
#include "net.h"
#include "cJSON.h"
#include "sources.h"            /* Reel's (riscos-ffmpeg player/sources.c) */
#include "panel_font.h"         /* Reel's bitmap font (riscos-ffmpeg reelcore/), for the pictures */
#include "player.h"
#include "reelcore.h"
#include "fake_reelcore.h"
#include "imgcache.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
/* in the fake SWIs, called many times: a failure counts, each call isn't a check */
#define SCHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static char base[64], outdir[256], scrap[300], choices[300];
static int task = 0x55;

/* ---- the fake Wimp's state ------------------------------------------------- */

typedef struct { int box[4]; unsigned flags; int data[3]; } icon_t;
typedef struct {
    int handle, open, nicons;
    int vis[4], sx, sy;
    unsigned flags;
    icon_t icon[16];
} win_t;
static win_t wins[8];
static int nwins;
static int drag_win, drag_type;             /* the last Wimp_DragBox */

static int bar_icon_made, proginfo_made, info_sub = -99;
static char bar_sprite[13];
static int *menu_open;                  /* the last Wimp_CreateMenu block (NULL: closed) */
static int menus_closed;
static int redraws_forced, icons_refreshed, extent_h;
static int hourglass_depth, hourglass_bad;
static int reports;
static char last_report[256];
static int report_answer = 1;
static int fake_cs = 1000;
static int last_poll, last_mask, idle_time;
static int keys_passed;

/* tasks TaskManager lists */
static struct { int handle; const char *name; } tasks[4];
static int ntasks;

/* messages sent */
typedef struct { int reason, to, icon; int b[64]; source_t src; int nsrc; int file_there; } sent_t;
static sent_t sent[64];
static int nsent, myref = 0x1000;

/* tasks started */
static char started[8][512];
static int nstarted;
static source_t started_src[8];
static int started_nsrc[8];

/* filetypes set */
static char typed_path[8][300];
static int typed_type[8], ntyped;

/* plotting */
static int plots, plot_sprites, plot_texts;
static char plotted_text[8192];
static int *plotted_area;               /* the last poster sprite plotted */
static int *plotted_wide;               /* the widest one (a backdrop) */
static int *output_sprite;              /* where output goes (NULL: the screen) */
static int jpeg_plots, jpeg_scale[4], jpeg_into_sprite = 1;
static int jpeg_topcut = -1;          /* of pictures cut top and bottom: the most cut off the top, in % of what's cut */
static int drag_started;
static int pointer_w = -1, pointer_i = -1, pointer_x = 640, pointer_y = 480;
static int poll_null_mask_bad;

/* the fake VideoOverlay module */
static int ovl_on = 1, ovl_created, ovl_destroyed, ovl_id, ovl_sel_fourcc, ovl_sel_flags, ovl_banks;
static int ovl_scale[2], ovl_pos[6], ovl_win, ovl_display = -9, ovl_displays, ovl_redraws, ovl_maps, ovl_mapped;
static int vsyncs, pointer_off;
static uint8_t ovl_mem[3][1280 * 720 * 3 / 2];
static int ovl_planes[6];
static const char *const ovl_swis[] = { "VideoOverlay_Create", "VideoOverlay_Destroy", "VideoOverlay_DisplayBuffer",
    "VideoOverlay_MapBuffer", "VideoOverlay_UnmapBuffer", "VideoOverlay_DiscardBuffer", "VideoOverlay_Vet",
    "VideoOverlay_SetScale", "VideoOverlay_SetWindow", "VideoOverlay_SetPosition", "VideoOverlay_RedrawWindow" };
static int sprite_plots_52, plot_52_bad, updates, deleted_win;
int fake_rc_cs(void);

/* ---- a small renderer: the screen as the Wimp would show it ------------------
   1920 x 1080 pixels (eig 1). OS_Plot rectangles, circles and triangles in
   the ColourTrans colour; text from Reel's bitmap font (a title at twice
   the size); sprites with their masks. Only for looking at: tests/host
   writes the browser and the details window out as PPM pictures. */
#define FB_W 1920
#define FB_H 1080
static unsigned fb[FB_W * FB_H];        /* 0xRRGGBB */
static int clip[4] = { 0, 0, FB_W * 2, FB_H * 2 };      /* OS units, x1 y1 exclusive */
static unsigned gcol, text_fg, font_fg;
static int pts[2][2];                   /* the last two points plotted */
static int redraw_ox, redraw_oy;        /* the window being redrawn: its work area origin */
static int shapes, glyphs;
static int *last_glyph;                 /* the last smooth shape plotted */

static unsigned rgb_of(long pal)        /* &BBGGRR00 -> 0xRRGGBB */
{
    unsigned p = (unsigned)pal;
    return (p >> 8 & 255) << 16 | (p >> 16 & 255) << 8 | (p >> 24 & 255);
}

static void px_set(int x, int y, unsigned c)            /* OS units */
{
    int X = x >> 1, Y = FB_H - 1 - (y >> 1);
    if (x < clip[0] || x >= clip[2] || y < clip[1] || y >= clip[3])
        return;
    if (X >= 0 && X < FB_W && Y >= 0 && Y < FB_H)
        fb[Y * FB_W + X] = c;
}

static unsigned fb_at(int x, int y)                     /* OS units */
{
    int X = x >> 1, Y = FB_H - 1 - (y >> 1);
    return X >= 0 && X < FB_W && Y >= 0 && Y < FB_H ? fb[Y * FB_W + X] : 0xFFFFFFFF;
}

static void fill_rect(int x0, int y0, int x1, int y1)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0 & ~1; y <= y1; y += 2)
        for (int x = x0 & ~1; x <= x1; x += 2)
            px_set(x, y, gcol);
}

static void fill_circle(int cx, int cy, int r)
{
    for (int y = cy - r; y <= cy + r; y += 2)
        for (int x = cx - r; x <= cx + r; x += 2)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                px_set(x, y, gcol);
}

static long edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (long)(bx - ax) * (py - ay) - (long)(by - ay) * (px - ax);
}

static void fill_tri(int ax, int ay, int bx, int by, int cx, int cy)
{
    int x0 = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx), x1 = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int y0 = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy), y1 = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    for (int y = y0 & ~1; y <= y1; y += 2)
        for (int x = x0 & ~1; x <= x1; x += 2) {
            long e0 = edge(ax, ay, bx, by, x, y), e1 = edge(bx, by, cx, cy, x, y), e2 = edge(cx, cy, ax, ay, x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                px_set(x, y, gcol);
        }
}

/* Reel's 15-pixel panel font (riscos-ffmpeg 0.1.21's panel_fonts[]) */
#define PANEL_FONT_W (panel_fonts[3].w)
#define PANEL_FONT_H (panel_fonts[3].h)

static void fb_text(int x, int y, const char *s, unsigned fg, int scale)
{
    /* the baseline at y: the font's cell is 18 pixels, its baseline 14 down */
    int top = y + (14 * scale) * 2;
    for (; *s; s++, x += PANEL_FONT_W * 2 * scale) {
        unsigned char c = (unsigned char)*s;
        int g = c >= 32 && c <= 126 ? c - 32 : c >= 160 ? 95 + c - 160 : 0;
        for (int row = 0; row < PANEL_FONT_H * scale; row++)
            for (int col = 0; col < PANEL_FONT_W * scale; col++) {
                int a = panel_fonts[3].data[(size_t)g * PANEL_FONT_W * PANEL_FONT_H + (row / scale) * PANEL_FONT_W + col / scale];
                int X = (x >> 1) + col, Y = FB_H - 1 - (top >> 1) + row;
                int ox = X * 2, oy = (FB_H - 1 - Y) * 2;
                if (!a || ox < clip[0] || ox >= clip[2] || oy < clip[1] || oy >= clip[3] ||
                    X < 0 || X >= FB_W || Y < 0 || Y >= FB_H)
                    continue;
                {
                    unsigned b = fb[Y * FB_W + X], o = 0;
                    for (int k = 0; k < 24; k += 8)
                        o |= (((fg >> k & 255) * a + (b >> k & 255) * (255 - a)) / 255) << k;
                    fb[Y * FB_W + X] = o;
                }
            }
    }
}

/* A 32bpp sprite (area + 16), in the middle of box (screen OS units) */
static void fb_sprite(const int *spr, int x0, int y0, int x1, int y1)
{
    int w = spr[4] + 1, h = spr[5] + 1, words = (w + 31) / 32;
    const unsigned *img = (const unsigned *)((const char *)spr + spr[8]);
    const unsigned *mask = spr[9] != spr[8] ? (const unsigned *)((const char *)spr + spr[9]) : NULL;
    int sx = ((x0 + x1) / 2 >> 1) - w / 2, sy = FB_H - (((y0 + y1) / 2) >> 1) - h / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned p = img[y * w + x];
            int X = sx + x, Y = sy + y, ox = X * 2, oy = (FB_H - 1 - Y) * 2;
            if (mask && !(mask[y * words + x / 32] >> (x & 31) & 1))
                continue;
            if (ox < clip[0] || ox >= clip[2] || oy < clip[1] || oy >= clip[3] || X < 0 || X >= FB_W || Y < 0 || Y >= FB_H)
                continue;
            fb[Y * FB_W + X] = (p & 255) << 16 | (p >> 8 & 255) << 8 | (p >> 16 & 255);
        }
}

/* A sprite (by pointer) with its bottom left at x, y (screen OS units), as OS_SpriteOp 52 plots it */
static void fb_sprite_at(const int *spr, int x, int y)
{
    int w = spr[4] + 1, h = spr[5] + 1;
    const unsigned *img = (const unsigned *)((const char *)spr + spr[8]);
    int top = FB_H - 1 - ((y >> 1) + h - 1);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            unsigned p = img[r * w + c];
            int X = (x >> 1) + c, Y = top + r, ox = X * 2, oy = (FB_H - 1 - Y) * 2;
            if (ox < clip[0] || ox >= clip[2] || oy < clip[1] || oy >= clip[3] || X < 0 || X >= FB_W || Y < 0 || Y >= FB_H)
                continue;
            fb[Y * FB_W + X] = (p & 255) << 16 | (p >> 8 & 255) << 8 | (p >> 16 & 255);
        }
}

/* The window's visible area, as a PPM picture */
static void fb_save(const char *path, const int *vis)
{
    FILE *f = fopen(path, "wb");
    int X0 = vis[0] >> 1, X1 = vis[2] >> 1, Y0 = FB_H - (vis[3] >> 1), Y1 = FB_H - (vis[1] >> 1);
    if (!f)
        return;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > FB_W) X1 = FB_W;
    if (Y1 > FB_H) Y1 = FB_H;
    fprintf(f, "P6\n%d %d\n255\n", X1 - X0, Y1 - Y0);
    for (int y = Y0; y < Y1; y++)
        for (int x = X0; x < X1; x++) {
            unsigned p = fb[y * FB_W + x];
            putc(p >> 16 & 255, f); putc(p >> 8 & 255, f); putc(p & 255, f);
        }
    fclose(f);
}

static int menu_window;                 /* a window opened as a menu (the save box) */

static win_t *win(int h)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i].handle == h)
            return &wins[i];
    return NULL;
}

static const char *icon_text(int w, int i)
{
    win_t *x = win(w);
    return x && i < x->nicons ? (const char *)(intptr_t)x->icon[i].data[0] : "";
}


/* a menu item's text and the rest */
static const char *menu_text(const int *m, int i) { return (const char *)(intptr_t)m[7 + i * 6 + 3]; }
static int menu_flags(const int *m, int i) { return m[7 + i * 6]; }
static int menu_sub(const int *m, int i) { return m[7 + i * 6 + 1]; }
static int menu_shaded(const int *m, int i) { return (m[7 + i * 6 + 2] & 0x400000) != 0; }

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ---- the SWIs ---------------------------------------------------------------- */

static int script(int *b, int mask);

/* A profile of the SWIs called (PROFILE=1): which, and how often, between
   profile_begin and profile_end */
static struct { int swi; unsigned n; } prof[128];
static int nprof, profiling;
static void profile_count(int swi)
{
    if (!profiling)
        return;
    for (int i = 0; i < nprof; i++)
        if (prof[i].swi == swi) {
            prof[i].n++;
            return;
        }
    if (nprof < 128) {
        prof[nprof].swi = swi;
        prof[nprof++].n = 1;
    }
}
static void profile_begin(void) { nprof = 0; profiling = getenv("PROFILE") != NULL; }
extern unsigned draw_test_made, draw_test_scans, draw_test_widths, draw_test_fits, draw_test_wraps;
static unsigned made0, scans0, w0, f0, r0;
static void profile_end(const char *what)
{
    unsigned total = 0;
    if (!profiling)
        return;
    printf("  shapes made since last: %u, scans: %u (draw_width %u, fit %u, wrap %u)\n", draw_test_made - made0,
           draw_test_scans - scans0, draw_test_widths - w0, draw_test_fits - f0, draw_test_wraps - r0);
    w0 = draw_test_widths; f0 = draw_test_fits; r0 = draw_test_wraps;
    made0 = draw_test_made;
    scans0 = draw_test_scans;
    profiling = 0;
    for (int i = 0; i < nprof; i++)
        total += prof[i].n;
    printf("  profile %s: %u SWIs:", what, total);
    for (int i = 0; i < nprof; i++)
        if (prof[i].n * 50 >= total)
            printf(" &%X x%u", prof[i].swi, prof[i].n);
    printf("\n");
}

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "fake: not handled" };
    profile_count(swi);
    switch (swi) {
    case 0x400C0:                                   /* Wimp_Initialise */
        SCHECK(!strcmp((const char *)(intptr_t)in->r[2], "Matinee"), "task name");
        out->r[1] = task;
        return NULL;
    case 0x42681: {                                 /* TaskManager_EnumerateTasks */
        int *p = (int *)(intptr_t)in->r[1];
        for (int i = 0; i < ntasks; i++) {
            p[0] = tasks[i].handle; p[1] = (int)(intptr_t)tasks[i].name; p[2] = 0; p[3] = 0;
            p += 4;
        }
        out->r[1] = (intptr_t)p;
        out->r[0] = -1;
        return NULL;
    }
    case 0x42: out->r[0] = fake_cs; return NULL;    /* OS_ReadMonotonicTime */
    case 0x35:                                      /* OS_ReadModeVariable */
        out->r[2] = in->r[1] == 4 || in->r[1] == 5 ? 1 : in->r[1] == 11 ? 1919 : in->r[1] == 12 ? 1079 :
                    in->r[1] == 9 ? 5 : 0;
        return NULL;
    case 0x39: {                                    /* OS_SWINumberFromString */
        const char *n = (const char *)(intptr_t)in->r[1];
        for (int i = 0; ovl_on && i < 11; i++)
            if (!strcmp(n, ovl_swis[i])) {
                out->r[0] = 0x59CC0 + i;
                return NULL;
            }
        return &err;
    }
    case 0x68:                                      /* OS_Memory 0: our page at &8000's page number */
        if ((in->r[0] & 0xFF) == 0 && (in->r[0] & 0x200) && (in->r[0] & 0x800)) {
            int *blk = (int *)(intptr_t)in->r[1];
            SCHECK(blk[1] == 0x8000 && in->r[2] == 1, "OS_Memory 0: the page at &8000");
            blk[0] = fake_rc.app_page ? fake_rc.app_page : (fake_rc.app_page = 4242);
            return NULL;
        }
        return &err;
    case 0x59CC0: {                                 /* VideoOverlay_Create */
        const int *sel = (const int *)(intptr_t)in->r[0];
        for (int i = 5; sel[i] != -1; i += 2) {
            if (sel[i] == 0) ovl_sel_flags = sel[i + 1];
            if (sel[i] == 3) ovl_sel_fourcc = sel[i + 1];
            if (sel[i] == 13) ovl_banks = sel[i + 1];
        }
        SCHECK(sel[3] == 7 && in->r[3] == task && (in->r[2] & 1), "overlay: planar, for our task, scaled");
        ovl_created++;
        ovl_id = 7;
        out->r[0] = 7; out->r[1] = 1; out->r[2] = 16; out->r[3] = 16; out->r[4] = 4096; out->r[5] = 4096;
        return NULL;
    }
    case 0x59CC1: ovl_destroyed++; ovl_id = 0; ovl_display = -9; return NULL;
    case 0x59CC2: ovl_display = (int)in->r[1]; ovl_displays++; return NULL;
    case 0x59CC3: {                                 /* MapBuffer: Y, Cb, Cr planes of 1280 x 720 */
        int b = (int)in->r[1];
        if (ovl_mapped) { SCHECK(0, "overlay: mapped twice"); }
        ovl_planes[0] = (int)(intptr_t)ovl_mem[b]; ovl_planes[1] = 1280;
        ovl_planes[2] = (int)(intptr_t)(ovl_mem[b] + 1280 * 720); ovl_planes[3] = 640;
        ovl_planes[4] = (int)(intptr_t)(ovl_mem[b] + 1280 * 720 * 5 / 4); ovl_planes[5] = 640;
        out->r[0] = (intptr_t)ovl_planes;
        ovl_maps++;
        ovl_mapped = 1;
        return NULL;
    }
    case 0x59CC4: ovl_mapped = 0; return NULL;
    case 0x59CC7: ovl_scale[0] = (int)in->r[1]; ovl_scale[1] = (int)in->r[2]; return NULL;
    case 0x59CC8: ovl_win = (int)in->r[1]; return NULL;
    case 0x59CC9: for (int i = 0; i < 6; i++) ovl_pos[i] = (int)(&in->r[1])[i]; return NULL;
    case 0x59CCA: ovl_redraws++; return NULL;
    case 0x06:                                      /* OS_Byte 176: the vsync count */
        if (in->r[0] == 176) { out->r[1] = ++vsyncs & 255; return NULL; }
        if (in->r[0] == 19) return NULL;
        if (in->r[0] == 106) { pointer_off = in->r[1] == 0; return NULL; }     /* the pointer: off, or shape 1 */
        return &err;
    case 0x46: {                                    /* OS_WriteN: VDU 24, the graphics window */
        const unsigned char *v = (const unsigned char *)(intptr_t)in->r[0];
        if (in->r[1] == 9 && v[0] == 24) {
            clip[0] = (short)(v[1] | v[2] << 8); clip[1] = (short)(v[3] | v[4] << 8);
            clip[2] = (short)(v[5] | v[6] << 8) + 1; clip[3] = (short)(v[7] | v[8] << 8) + 1;
        }
        return NULL;
    }
    case 0x400C3: deleted_win = ((int *)(intptr_t)in->r[1])[0]; return NULL;    /* Wimp_DeleteWindow */
    case 0x400C9: {                                 /* Wimp_UpdateWindow: the box asked for, in view */
        int *b = (int *)(intptr_t)in->r[1];
        win_t *x = win(b[0]);
        int ox, oy;
        if (!x) return &err;
        ox = x->vis[0] - x->sx; oy = x->vis[3] - x->sy;
        b[7] = ox + b[1]; b[8] = oy + b[2]; b[9] = ox + b[3]; b[10] = oy + b[4];
        if (b[7] < x->vis[0]) b[7] = x->vis[0];
        if (b[8] < x->vis[1]) b[8] = x->vis[1];
        if (b[9] > x->vis[2]) b[9] = x->vis[2];
        if (b[10] > x->vis[3]) b[10] = x->vis[3];
        memcpy(b + 1, x->vis, 16);
        b[5] = x->sx; b[6] = x->sy;
        updates++;
        out->r[0] = x->open && b[7] < b[9] && b[8] < b[10];
        if (out->r[0]) {
            clip[0] = b[7]; clip[1] = b[8]; clip[2] = b[9]; clip[3] = b[10];
            redraw_ox = ox; redraw_oy = oy;
        }
        return NULL;
    }
    case 0x0C: {                                    /* OS_GBPB 10: a directory's entries with their information */
        DIR *d = opendir((const char *)(intptr_t)in->r[1]);
        struct dirent *e;
        char *o = (char *)(intptr_t)in->r[2];
        int k = 0, n = 0, want = (int)in->r[3], skip = (int)in->r[4];
        if (in->r[0] != 10 || !d) {
            if (d) closedir(d);
            return &err;
        }
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            char path[600];
            int *w = (int *)o;
            if (e->d_name[0] == '.')
                continue;
            if (k++ < skip)
                continue;
            if (n == want)
                break;
            snprintf(path, sizeof(path), "%s/%s", (const char *)(intptr_t)in->r[1], e->d_name);
            if (stat(path, &st) != 0)
                continue;
            w[0] = (int)(0xFFFFFF00u);                  /* a stamp: the file's time, in the exec word */
            w[1] = (int)st.st_mtime;
            w[2] = (int)st.st_size;
            w[3] = 3;
            w[4] = S_ISDIR(st.st_mode) ? 2 : 1;
            strcpy(o + 20, e->d_name);
            o += (20 + strlen(e->d_name) + 1 + 3) & ~3;
            n++;
        }
        out->r[3] = n;
        out->r[4] = e ? k - 1 : -1;
        closedir(d);
        return NULL;
    }
    case 0x08:                                      /* OS_File */
        if (in->r[0] == 8) { mkdir((const char *)(intptr_t)in->r[1], 0755); return NULL; }
        if (in->r[0] == 18) {
            snprintf(typed_path[ntyped & 7], 300, "%s", (const char *)(intptr_t)in->r[1]);
            typed_type[ntyped & 7] = (int)in->r[2];
            ntyped++;
            return NULL;
        }
        return &err;
    case 0x50B00: {                                 /* MimeMap_Translate */
        const char *w = (const char *)(intptr_t)in->r[1];
        if (in->r[0] == 2 && !strcmp(w, "video/mp4")) { out->r[3] = 0xBF4; return NULL; }
        if (in->r[0] == 3 && !strcmp(w, "mp4")) { out->r[3] = 0xBF4; return NULL; }
        if (in->r[0] == 3 && !strcmp(w, "mkv")) { out->r[3] = 0xB9A; return NULL; }
        return &err;
    }
    case 0x400C1: {                                 /* Wimp_CreateWindow */
        int *b = (int *)(intptr_t)in->r[1];
        win_t *x = &wins[nwins];
        memset(x, 0, sizeof(*x));
        x->flags = (unsigned)b[7];
        x->nicons = b[21];
        memcpy(x->vis, b, 16);
        memcpy(x->icon, b + 22, x->nicons * sizeof(icon_t));
        if (x->flags == 0x84000012u && x->nicons == 8) {    /* About this program */
            proginfo_made++;
            x->handle = 0x7000;
        } else {
            x->handle = 0x100 * (nwins + 1);
        }
        nwins++;
        out->r[0] = x->handle;
        return NULL;
    }
    case 0x400C2: {                                 /* Wimp_CreateIcon */
        int *b = (int *)(intptr_t)in->r[1];
        bar_icon_made++;
        memcpy(bar_sprite, &b[6], 12);
        out->r[0] = 3;
        return NULL;
    }
    case 0x400C5: {                                 /* Wimp_OpenWindow */
        int *b = (int *)(intptr_t)in->r[1];
        win_t *x = win(b[0]);
        if (!x) return &err;
        memcpy(x->vis, b + 1, 16);
        x->sx = b[5]; x->sy = b[6];
        x->open = 1;
        return NULL;
    }
    case 0x400C6: {                                 /* Wimp_CloseWindow */
        win_t *x = win(((int *)(intptr_t)in->r[1])[0]);
        if (x) x->open = 0;
        return NULL;
    }
    case 0x400CB: {                                 /* Wimp_GetWindowState */
        int *b = (int *)(intptr_t)in->r[1];
        win_t *x = win(b[0]);
        if (!x) return &err;
        memcpy(b + 1, x->vis, 16);
        b[5] = x->sx; b[6] = x->sy; b[7] = -1;
        b[8] = (int)(x->flags | (x->open ? 1u << 16 : 0));
        return NULL;
    }
    case 0x400D7:                                   /* Wimp_SetExtent */
        extent_h = -((int *)(intptr_t)in->r[1])[1];
        return NULL;
    case 0x400C8: {                                 /* Wimp_RedrawWindow: one rectangle, the whole window */
        int *b = (int *)(intptr_t)in->r[1];
        win_t *x = win(b[0]);
        if (!x) return &err;
        memcpy(b + 1, x->vis, 16);
        b[5] = x->sx; b[6] = x->sy;
        memcpy(b + 7, x->vis, 16);
        clip[0] = x->vis[0]; clip[1] = x->vis[1]; clip[2] = x->vis[2]; clip[3] = x->vis[3];
        redraw_ox = x->vis[0] - x->sx;
        redraw_oy = x->vis[3] - x->sy;
        out->r[0] = 1;
        return NULL;
    }
    case 0x400CA: out->r[0] = 0; clip[0] = 0; clip[1] = 0; clip[2] = FB_W * 2; clip[3] = FB_H * 2;
        return NULL;                                /* Wimp_GetRectangle: that was the only one */
    case 0x40743: gcol = rgb_of(in->r[0]); return NULL;         /* ColourTrans_SetGCOL */
    case 0x45: {                                    /* OS_Plot */
        int x = (int)in->r[1], y = (int)in->r[2], code = (int)in->r[0];
        if (code == 101) { fill_rect(pts[0][0], pts[0][1], x, y); shapes++; }
        else if (code == 157) {
            int dx = x - pts[0][0], dy = y - pts[0][1], r = 0;
            while ((r + 1) * (r + 1) <= dx * dx + dy * dy) r++;
            fill_circle(pts[0][0], pts[0][1], r); shapes++;
        } else if (code == 85) { fill_tri(pts[1][0], pts[1][1], pts[0][0], pts[0][1], x, y); shapes++; }
        else if (code != 4) return &err;
        pts[1][0] = pts[0][0]; pts[1][1] = pts[0][1];
        pts[0][0] = x; pts[0][1] = y;
        return NULL;
    }
    case 0x40081:                                   /* Font_FindFont: Homerton.Bold 12pt (1) or 20pt (2) */
        SCHECK(!strcmp((const char *)(intptr_t)in->r[1], "Homerton.Bold"), "font name");
        out->r[0] = in->r[2] >= 320 ? 2 : 1;
        return NULL;
    case 0x40082: return NULL;                      /* Font_LoseFont */
    case 0x4074F: font_fg = rgb_of(in->r[2]); return NULL;      /* ColourTrans_SetFontColours */
    case 0x400A1: {                                 /* Font_ScanString: millipoints, to the limit in R3 */
        const char *t = (const char *)(intptr_t)in->r[1];
        long per = (in->r[0] == 2 ? 32 : 16) * 400, n = (in->r[2] & 0x80) ? in->r[7] : (long)strlen(t);
        if (n > (long)strlen(t))
            n = (long)strlen(t);
        if (in->r[3] >= 0 && n * per > in->r[3])
            n = in->r[3] / per;                     /* it stops before the character that goes past */
        out->r[1] = (intptr_t)(t + n);
        out->r[3] = n * per;
        return NULL;
    }
    case 0x40086: {                                 /* Font_Paint */
        const char *t = (const char *)(intptr_t)in->r[1];
        SCHECK((in->r[2] & 0x110) == 0x110, "Font_Paint in OS units, with a handle");
        fb_text((int)in->r[3], (int)in->r[4], t, font_fg, in->r[0] == 2 ? 2 : 1);
        if (strlen(plotted_text) + strlen(t) + 2 < sizeof(plotted_text)) {
            strcat(plotted_text, t);
            strcat(plotted_text, "|");
        }
        return NULL;
    }
    case 0x400E2: {                                 /* Wimp_PlotIcon */
        const icon_t *ic = (const icon_t *)(intptr_t)in->r[1];
        plots++;
        if ((ic->flags & 3) == 2 && (ic->flags & 0x100)) {
            int *area = (int *)(intptr_t)ic->data[1];
            if (*(const char *)(intptr_t)ic->data[0] == 'p') {  /* a poster or backdrop ("g": a shape) */
                plot_sprites++;
                plotted_area = area;
                if (!plotted_wide || area[4 + 4] > plotted_wide[4 + 4])
                    plotted_wide = area;
            } else {
                glyphs++;
                last_glyph = area;
            }
            fb_sprite(area + 4, redraw_ox + ic->box[0], redraw_oy + ic->box[1],
                      redraw_ox + ic->box[2], redraw_oy + ic->box[3]);
        } else if (ic->flags & 1) {
            plot_texts++;
            if (strlen(plotted_text) + 100 < sizeof(plotted_text)) {
                strcat(plotted_text, (const char *)(intptr_t)ic->data[0]);
                strcat(plotted_text, "|");
            }
        }
        return NULL;
    }
    case 0x400D1: redraws_forced++; return NULL;    /* Wimp_ForceRedraw */
    case 0x400D0: {                                 /* Wimp_DragBox */
        const int *d = (const int *)(intptr_t)in->r[1];
        drag_win = d[0];
        drag_type = d[1];
        return NULL;
    }
    case 0x400CD: icons_refreshed++; return NULL;   /* Wimp_SetIconState */
    case 0x400D2: case 0x400D3: return NULL;        /* caret */
    case 0x400F9:                                   /* Wimp_TextOp */
        if (in->r[0] == 0) { text_fg = rgb_of(in->r[1]); return NULL; }
        if (in->r[0] == 1) {                        /* a width: R2 characters (0: all) */
            long n = (long)strlen((const char *)(intptr_t)in->r[1]);
            if (in->r[2] > 0 && in->r[2] < n)
                n = in->r[2];
            out->r[0] = n * 18;
            return NULL;
        }
        if ((in->r[0] & 0xFF) == 2) {
            const char *t = (const char *)(intptr_t)in->r[1];
            fb_text((int)in->r[4], (int)in->r[5], t, text_fg, 1);
            if (strlen(plotted_text) + strlen(t) + 2 < sizeof(plotted_text)) {
                strcat(plotted_text, t);
                strcat(plotted_text, "|");
            }
            return NULL;
        }
        return &err;
    case 0x406C0: hourglass_depth++; return NULL;
    case 0x406C1: if (--hourglass_depth < 0) hourglass_bad = 1; return NULL;
    case 0x400D4:                                   /* Wimp_CreateMenu */
        if (in->r[1] == -1) { menu_open = NULL; menus_closed++; menu_window = 0; return NULL; }
        if (in->r[1] > 0 && in->r[1] < 0x10000) { menu_window = (int)in->r[1]; menu_open = NULL; return NULL; }
        menu_open = (int *)(intptr_t)in->r[1];
        menu_window = 0;
        if (menu_open && !strncmp(menu_text(menu_open, 0), "Info", 4))
            info_sub = menu_sub(menu_open, 0);
        return NULL;
    case 0x400CF: {                                 /* Wimp_GetPointerInfo */
        int *b = (int *)(intptr_t)in->r[1];
        b[0] = pointer_x; b[1] = pointer_y; b[2] = 4; b[3] = pointer_w; b[4] = pointer_i;
        return NULL;
    }
    case 0x42400: drag_started++; return NULL;      /* DragASprite_Start */
    case 0x42401: return NULL;
    case 0x400E7: {                                 /* Wimp_SendMessage */
        int *b = (int *)(intptr_t)in->r[1];
        sent_t *s = &sent[nsent & 63];
        if (in->r[0] != 19)
            b[2] = ++myref;
        memset(s, 0, sizeof(*s));
        s->reason = (int)in->r[0]; s->to = (int)in->r[2]; s->icon = (int)in->r[3];
        memcpy(s->b, b, b[0] > 0 && b[0] <= 256 ? b[0] : 24);
        if (b[4] == 5) {                            /* DataOpen: Reel reads the file */
            s->file_there = file_exists((const char *)&b[11]);
            s->nsrc = sources_from_file((const char *)&b[11], &s->src, 1);
        }
        nsent++;
        return NULL;
    }
    case 0x400DE: {                                 /* Wimp_StartTask: the player reads the file */
        const char *cmd = (const char *)(intptr_t)in->r[0], *file = strrchr(cmd, ' ');
        int k = nstarted & 7;
        snprintf(started[k], sizeof(started[k]), "%s", cmd);
        started_nsrc[k] = file && file_exists(file + 1) ? sources_from_file(file + 1, &started_src[k], 1) : -9;
        nstarted++;
        return NULL;
    }
    case 0x400DF: {                                 /* Wimp_ReportError */
        _kernel_oserror *e = (_kernel_oserror *)(intptr_t)in->r[0];
        reports++;
        snprintf(last_report, sizeof(last_report), "%s", e->errmess);
        printf("  report: %s\n", e->errmess);
        out->r[1] = report_answer;
        return NULL;
    }
    case 0x49980: {                                 /* JPEG_Info */
        const unsigned char *j = (const unsigned char *)(intptr_t)in->r[1];
        if (in->r[2] < 4 || j[0] != 0xFF || j[1] != 0xD8) return &err;
        out->r[2] = 120; out->r[3] = 180;
        return NULL;
    }
    case 0x2E:                                      /* OS_SpriteOp 60: output to a sprite, and back */
        if ((in->r[0] & 0xFF) == 52) {              /* PutSpriteScaled: the player's picture */
            if (!(in->r[0] == 52 + 512 && in->r[6] == 0 && in->r[7] == 0))
                plot_52_bad = 1;                    /* checked once, at the end */
            fb_sprite_at((int *)(intptr_t)in->r[2], (int)in->r[3], (int)in->r[4]);
            sprite_plots_52++;
            return NULL;
        }
        if ((in->r[0] & 0xFF) != 60) return &err;
        if (in->r[2] == 0) { output_sprite = NULL; return NULL; }
        SCHECK((in->r[0] & 0x200) && in->r[2] == in->r[1] + 16, "output switched to the sprite by pointer");
        output_sprite = (int *)(intptr_t)in->r[2];
        out->r[0] = 60 + 512; out->r[1] = 0; out->r[2] = 0; out->r[3] = 1;
        return NULL;
    case 0x49982: {                                 /* JPEG_PlotScaled: a picture from the JPEG's bytes */
        int *sc = (int *)(intptr_t)in->r[3];
        const unsigned char *j = (const unsigned char *)(intptr_t)in->r[0];
        unsigned h = 2166136261u;
        int w, ht;
        jpeg_plots++;
        if (!output_sprite) { jpeg_into_sprite = 0; return NULL; }
        memcpy(jpeg_scale, sc, sizeof(jpeg_scale));
        {
            int bh = output_sprite[5] + 1, y = (int)in->r[2] >> 1, excess = sc[1] - bh;
            if (excess > 2) {
                int top = (y + sc[1] - bh) * 100 / excess;
                if (top > jpeg_topcut)
                    jpeg_topcut = top;
            }
        }
        for (long i = 0; i < in->r[4]; i++)
            h = (h ^ j[i]) * 16777619u;
        w = output_sprite[4] + 1; ht = output_sprite[5] + 1;
        for (int y = 0; y < ht; y++)                /* a diagonal blend of two colours, and a band */
            for (int x = 0; x < w; x++) {
                int t = (x + y) * 255 / (w + ht);
                unsigned r = ((h & 255) * (255 - t) + (h >> 24) * t) / 255;
                unsigned g = ((h >> 8 & 255) * (255 - t) + (h >> 4 & 255) * t) / 255;
                unsigned b = ((h >> 16 & 255) * (255 - t) + (h >> 12 & 255) * t) / 255;
                if (y > ht * 6 / 10 && y < ht * 7 / 10)
                    r = g = b = 235;
                ((unsigned *)(output_sprite + 11))[y * w + x] = r | g << 8 | b << 16;
            }
        output_sprite[11] = 0x00123456;             /* the first pixel: a mark for the test */
        return NULL;
    }
    case 0x400DC: keys_passed++; return NULL;       /* Wimp_ProcessKey */
    case 0x400DD: return NULL;                      /* Wimp_CloseDown */
    case 0x400C7: case 0x400E1:                     /* Wimp_Poll, Wimp_PollIdle */
        last_poll = swi;
        last_mask = (int)in->r[0];
        if (swi == 0x400E1)
            idle_time = (int)in->r[2];
        out->r[0] = script((int *)(intptr_t)in->r[1], (int)in->r[0]);
        return NULL;
    }
    printf("  unhandled SWI %x\n", swi);
    return &err;
}

/* ---- the fake server's record ------------------------------------------------ */

static cJSON *server_log(void)
{
    char url[128];
    net_buf b;
    cJSON *j;
    snprintf(url, sizeof(url), "%s/_log", base);
    if (net_fetch(url, NULL, NULL, &b, 5000, NULL, 0) != 0)
        return NULL;
    j = cJSON_Parse(b.data);
    net_buf_free(&b);
    return j;
}

static int log_count(const char *path, const char *qkey, const char *qval)
{
    cJSON *log = server_log(), *r;
    int n = 0;
    cJSON_ArrayForEach(r, log) {
        const cJSON *q;
        if (strcmp(cJSON_GetObjectItem(r, "path")->valuestring, path))
            continue;
        if (qkey) {
            q = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "query"), qkey);
            q = cJSON_IsArray(q) ? cJSON_GetArrayItem(q, 0) : NULL;
            if (!cJSON_IsString(q) || strcmp(q->valuestring, qval))
                continue;
        }
        n++;
    }
    cJSON_Delete(log);
    return n;
}

static char *read_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    static char buf[8192];
    size_t n;
    if (!f)
        return "";
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* ---- events ---------------------------------------------------------------------- */

static int w_browser, w_save;

static int ev_click(int *b, int w, int i, int x, int y, int buttons)
{
    memset(b, 0, 24);
    b[0] = x; b[1] = y; b[2] = buttons; b[3] = w; b[4] = i;
    return 6;
}

static int ev_tile(int *b, int t, int buttons)
{
    int x = 0, y = 0;
    CHECK(ui_test_tile_xy(t, &x, &y) == 0, "tile %d is there", t);
    return ev_click(b, w_browser, -1, x, y, buttons);
}

static int ev_menu(int *b, int a, int c)
{
    b[0] = a; b[1] = c; b[2] = -1;
    return 9;
}

static int ev_menu3(int *b, int a, int c, int d)
{
    b[0] = a; b[1] = c; b[2] = d; b[3] = -1;
    return 9;
}

static int ev_key(int *b, int w, int i, int k)
{
    memset(b, 0, 28);
    b[0] = w; b[1] = i; b[6] = k;
    return 8;
}

static int ev_msg(int *b, int action, int your_ref, int from)
{
    memset(b, 0, 256);
    b[0] = 256; b[1] = from; b[2] = 0x9999; b[3] = your_ref; b[4] = action;
    return 17;
}

/* The tile with that title (or its start, if it was cut to fit: "Big B...") */
static int find_tile(const char *text)
{
    for (int i = 0; i < ui_test_items(); i++) {
        const char *t = ui_test_item(i, 0);
        size_t n = strlen(t);
        if (!strcmp(t, text) || (n > 3 && !strcmp(t + n - 3, "...") && !strncmp(t, text, n - 3)))
            return i;
    }
    return -1;
}

static const sent_t *last_sent(int action)
{
    for (int i = nsent - 1; i >= 0 && i >= nsent - 64; i--)
        if (sent[i & 63].b[4] == action)
            return &sent[i & 63];
    return NULL;
}

static unsigned file_sum(const char *p, long *len)
{
    FILE *f = fopen(p, "rb");
    unsigned sum = 0;
    int c;
    *len = 0;
    if (!f)
        return 0;
    while ((c = getc(f)) != EOF) {
        sum = sum * 31 + (unsigned)c;
        (*len)++;
    }
    fclose(f);
    return sum;
}

/* ---- the script --------------------------------------------------------------------- */

int fake_rc_cs(void) { return fake_cs; }

#define NEAR(a, b, e) ((a) - (b) < (e) && (b) - (a) < (e))

/* the full screen window: no furniture, open */
/* the mini player: moveable, no furniture */
static int mini_win(void)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i].flags == 0x80000002u)
            return wins[i].handle;
    return 0;
}

static int full_win(void)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i].flags == 0x80000040u && wins[i].handle != deleted_win)
            return wins[i].handle;
    return 0;
}

/* the picture's box in the browser window, in pixels (the bar's 168 OS units below it) */
static void pic_px(int *w, int *h)
{
    win_t *x = win(w_browser);
    *w = (x->vis[2] - x->vis[0]) >> 1;
    *h = (x->vis[3] - x->vis[1] - 168) >> 1;
}

static int player_button(int *b, int w, int id)
{
    int x = 0, y = 0;
    CHECK(player_test_button_xy(id, &x, &y) == 0, "player button %d is there", id);
    return ev_click(b, w, -1, x, y, 0x400);
}

static int pc, speed_nulls, save_nulls, drain_n, prev_nsent, prev_started, prev_reports, prev_count;
static char save_path[300], save_path2[300];
static int n_null, open0, count0, draws0, wfull, wmini, items0, seeks0;
static char sid0[40];
static double p0;

#define NULL_EVENT 0
/* hands out null events while the program wants them (at most n), then
   moves on */
#define DRAIN(n) do { if (!(mask & 1) && drain_n++ < (n)) return NULL_EVENT; drain_n = 0; pc++; } while (0)

/* a click on a button the program draws */
static int ev_button(int *b, int w, int id, int buttons)
{
    int x = 0, y = 0;
    CHECK(ui_test_button_xy(w, id, &x, &y) == 0, "button %d is there", id);
    return ev_click(b, w, -1, x, y, buttons);
}

static int ev_redraw(int *b, int w)
{
    memset(b, 0, 64);
    b[0] = w;
    plotted_text[0] = 0;
    profile_begin();
    plot_sprites = 0;
    plotted_area = plotted_wide = NULL;
    shapes = 0;
    glyphs = 0;
    return 1;                                           /* Redraw_Window_Request */
}

static void save_picture(const char *name, int w)
{
    profile_end(name);
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", outdir, name);
    fb_save(path, win(w)->vis);
    printf("  picture: %s\n", path);
}

static const char *started_url(void)
{
    int k = (nstarted - 1) & 7;
    return nstarted && started_nsrc[k] == 1 ? started_src[k].url : "";
}

static int script(int *b, int mask)
{
    static int wait_save, type_i;
    static char type_str[80];
    char want[400];
    for (;;) {
        switch (pc) {
        /* ---- start-up: sign in with a code */
        case 0:
            ui_test_windows(&w_browser, &w_save);
            CHECK(bar_icon_made == 1 && !strcmp(bar_sprite, "!matinee"), "icon bar icon '%s'", bar_sprite);
            CHECK(proginfo_made == 1, "Info window");
            CHECK(strstr(read_file(choices), "client_id matinee-") != NULL, "a client id kept from the start");
            CHECK(mask & 1, "no null events while there's nothing to do");
            pc = 160;
            return ev_click(b, -2, 3, 1000, 20, 4);             /* Select on the icon */
        case 160:
            CHECK(win(w_browser)->open && ui_test_signin_mode() == SI_PICK, "Select, no server yet: Add a server, its choice");
            pc++;
            return ev_redraw(b, w_browser);
        case 161:
            CHECK(strstr(plotted_text, "Add a server|") && strstr(plotted_text, "Plex|") && strstr(plotted_text, "Jellyfin|") &&
                  strstr(plotted_text, "Cancel|"), "drawn: %s", plotted_text);
            save_picture("signin-pick.ppm", w_browser);
            pc++;
            return ev_button(b, w_browser, S_CANCEL, 0x400);
        case 162:
            CHECK(!win(w_browser)->open && ui_test_page() != PG_SIGNIN, "Cancel with no server: the window closed");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 163:
            CHECK(ui_test_signin_mode() == SI_PICK, "Select again: the choice");
            pc = 1;
            return ev_button(b, w_browser, S_PICK_PLEX, 0x400);
        case 1:
            CHECK(win(w_browser)->open && ui_test_page() == PG_SIGNIN,
                  "Select before signing in: the window, on its sign-in page");
            CHECK(!strcmp(ui_test_signin(0), "ABCD"), "the code: %s", ui_test_signin(0));
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 200, "checked every 2 s (PollIdle %d, now %d)",
                  idle_time, fake_cs);
            {   /* 75% of the screen (3840 x 2160; no icon bar in the fake Wimp: 134 high), in the middle */
                win_t *x = win(w_browser);
                CHECK(x->vis[2] - x->vis[0] == 2840 && x->vis[3] - x->vis[1] == 1478 && x->vis[0] == 480 &&
                      x->vis[1] == 388, "the window: 75%% of the screen, centred (%d,%d %dx%d)", x->vis[0], x->vis[1],
                      x->vis[2] - x->vis[0], x->vis[3] - x->vis[1]);
            }
            pc = 150;
            return ev_redraw(b, w_browser);
        case 150:
            CHECK(strstr(plotted_text, "Sign in|") && strstr(plotted_text, "Sign in to Plex with a code|") && strstr(plotted_text, "Back|") &&
                  strstr(plotted_text, "A  B  C  D|") && strstr(plotted_text, "Waiting for the code...|") &&
                  strstr(plotted_text, "Use these|"), "the sign-in page drawn: %s", plotted_text);
            save_picture("signin.ppm", w_browser);
            fake_cs = idle_time;
            pc = 2;
            return NULL_EVENT;
        case 2:
            CHECK(ui_test_page() == PG_SIGNIN, "not signed in after the first check");
            CHECK(strstr(ui_test_signin(1), "Waiting"), "status: %s", ui_test_signin(1));
            fake_cs = idle_time;
            pc = 151;
            return NULL_EVENT;
        case 151:
            pc = 3;
            memset(b, 0, 32);                   /* the rest of the script: the size it was made for (4 posters a row) */
            b[0] = w_browser; b[1] = 1366; b[2] = 570; b[3] = 1366 + 1108; b[4] = 570 + 1100; b[7] = -1;
            return 2;                           /* Open_Window_Request */
        case 3:
            CHECK(ui_test_page() == PG_GRID, "signed in: the grid");
            CHECK(win(w_browser)->open, "in the same window");
            {   /* the home page: its rows */
                int rows = 0, pick = -1, st = 0, n = 0, vis = 0;
                CHECK(ui_test_home(&rows, &pick) && rows == 3 && pick == 0, "the top: the home page, 3 rows (%d)", rows);
                CHECK(ui_test_items() == 9, "the top list: %d items", ui_test_items());
                CHECK(ui_test_home_row(0, &st, &n, &vis) && !strcmp(ui_test_home_row(0, &st, &n, &vis), "Continue watching") &&
                      st == 0 && n == 1, "Continue watching first");
                CHECK(!strcmp(ui_test_home_row(1, &st, &n, &vis), "Recently added in Films") && st == 1 && n == 6 && vis == 3,
                      "then Recently added in Films: 6, 3 fit the window (%d)", vis);
                CHECK(!strcmp(ui_test_home_row(2, &st, &n, &vis), "Recently added in TV Programmes") && n == 2,
                      "then TV Programmes'");
                CHECK(!ui_test_home_row(3, &st, &n, &vis), "no row of libraries: they're tabs, under the bar");
                CHECK(!strcmp(ui_test_item(7, 0), "Space Show") && !strncmp(ui_test_item(7, 1), "S1 E6 Ep", 8) &&
                      !strcmp(ui_test_item(8, 0), "Space Show") && !strcmp(ui_test_item(8, 1), "Series 2"),
                      "recently added episodes and series under their show's name: %s / %s, %s / %s", ui_test_item(7, 0),
                      ui_test_item(7, 1), ui_test_item(8, 0), ui_test_item(8, 1));
            }
            CHECK(!strcmp(ui_test_path(), "Attic"), "where: %s", ui_test_path());
            {
                int cur = -1;
                CHECK(ui_test_tab(&cur) == 4 && cur == 0, "the tabs: Home (here) and the 3 libraries");
            }
            snprintf(want, sizeof(want), "server_base %s\n", base);
            CHECK(strstr(read_file(choices), "account_token ACCT-TOKEN\n") && strstr(read_file(choices), want) &&
                  strstr(read_file(choices), "server_token SRV-TOKEN\n") &&
                  strstr(read_file(choices), "server_name Attic\n") &&
                  strstr(read_file(choices), "player ReelEGL\n") && strstr(read_file(choices), "poster_size 1\n"),
                  "Choices after signing in:\n%s", read_file(choices));
            CHECK(!hourglass_depth && !hourglass_bad, "hourglass on and off in pairs");
            pc++;
            return ev_redraw(b, w_browser);
        case 4:
            CHECK(strstr(plotted_text, "Attic|") && strstr(plotted_text, "9 items.|Home|Films|TV Programmes|Music|"),
                  "the header, and the tabs under it: %s", plotted_text);
            CHECK(strstr(plotted_text, "CONTINUE WATCHING|Space Show|S1 E3 Episode Three|2020|1h 30m|PG|") &&
                  strstr(plotted_text, "|Resume|Details|81 min left|") && strstr(plotted_text, "|Continue watching|Space Show|"),
                  "the featured part, and the first row: %s", plotted_text);
            CHECK(shapes > 20 && glyphs > 20 && plot_sprites == 1,
                  "drawn in shapes and smooth corners; the featured backdrop, no posters yet (%d, %d, %d)", shapes, glyphs,
                  plot_sprites);
            CHECK(log_count("/photo/:/transcode", "url", "/library/metadata/213/art/1700000000") == 1 &&
                  log_count("/photo/:/transcode", "width", "1420") == 1, "the featured backdrop, the window's width (as it opened)");
            fake_cs += 60;                                      /* and again once the window's been made smaller */
            {   /* a smooth shape has colours between its own and the background's */
                const int *g = last_glyph + 4;
                const unsigned *px = (const unsigned *)(g + 11);
                int n = (g[4] + 1) * (g[5] + 1), ncol = 0;
                unsigned col[3];
                for (int i = 0; i < n && ncol < 3; i++) {
                    int k = 0;
                    while (k < ncol && col[k] != px[i])
                        k++;
                    if (k == ncol)
                        col[ncol++] = px[i];
                }
                CHECK(last_glyph && ncol >= 3, "anti-aliased edges (%d colours: more than the shape's and the background's)",
                      ncol);
            }
            CHECK(!(mask & 1), "null events for the posters");
            pc++;
            continue;
        case 5:
            DRAIN(20);
            continue;
        case 6:
            CHECK(mask & 1, "no more null events once the posters are there");
            CHECK(log_count("/photo/:/transcode", "url", "/library/metadata/213/art/1700000000") == 3 &&   /* (and its card's) */
                  log_count("/photo/:/transcode", "width", "554") == 1, "the featured backdrop again, at the window's new width (%d, %d)",
                  log_count("/photo/:/transcode", "url", "/library/metadata/213/art/1700000000"), log_count("/photo/:/transcode", "width", "554"));
            CHECK(log_count("/photo/:/transcode", "width", "200") >= 1 && log_count("/photo/:/transcode", "height", "112") >= 1,
                  "Continue watching: 16:9 pictures, 200 x 112 pixels");
            CHECK(log_count("/photo/:/transcode", "width", "116") >= 1 && log_count("/photo/:/transcode", "height", "174") >= 1,
                  "posters asked for at the tile's size in pixels");
            CHECK(jpeg_topcut >= 0 && jpeg_topcut <= 25,
                  "a tall picture in a wide box: cut mostly from the foot (%d%% off the top)", jpeg_topcut);
            pc = 60100;
            return ev_redraw(b, w_browser);
        case 60100:
            save_picture("home.ppm", w_browser);
            pc = 7;
            return ev_button(b, w_browser, TB_TAB + 1, 0x400);  /* the Films tab */
        case 7:
            CHECK(ui_test_items() == 6 && !strcmp(ui_test_path(), "Attic > Films"), "Films: %d items, %s",
                  ui_test_items(), ui_test_path());
            CHECK(ui_test_sel() == 0, "the first selected");
            pc++;
            continue;
        case 8:
            DRAIN(40);
            continue;
        case 9: {
            int failed = -1, n = ui_test_posters(&failed);
            CHECK(n >= 7 && failed == 0, "posters: %d made, %d failed", n, failed);
            CHECK(jpeg_into_sprite && jpeg_plots >= 7, "JPEGs drawn into sprites, not the screen (%d)", jpeg_plots);
            CHECK(jpeg_scale[0] == 116 && jpeg_scale[1] == 174 && jpeg_scale[2] == 120 && jpeg_scale[3] == 180,
                  "JPEG scaled to fit: %d/%d x %d/%d", jpeg_scale[0], jpeg_scale[2], jpeg_scale[1], jpeg_scale[3]);
            CHECK(!output_sprite, "output back on the screen");
            pc++;
            return ev_redraw(b, w_browser);
        }
        case 10: {
            const int *spr = plotted_area ? plotted_area + 4 : NULL;
            CHECK(plot_sprites >= 6 && spr && plotted_area[1] == 1 && spr[11] == 0x00123456,
                  "posters plotted from their sprites (%d)", plot_sprites);
            CHECK(spr && spr[4] == 115 && spr[5] == 173 && spr[10] == (int)(1u | 90u << 1 | 90u << 14 | 6u << 27),
                  "a 116 x 174 32bpp sprite at 90 dpi");
            CHECK(spr && spr[9] == spr[8] + 116 * 174 * 4 && !(((const unsigned *)((const char *)spr + spr[9]))[0] & 1) &&
                  (((const unsigned *)((const char *)spr + spr[9]))[80 * 4] >> 20 & 1),
                  "a mask: the corners cut, the middle solid");
            CHECK(strstr(plotted_text, "Big Buck Bunny|2008|"), "titles and years: %s", plotted_text);
            save_picture("browser.ppm", w_browser);
            pc++;
            return ev_tile(b, find_tile("Dvd Rip"), 0x400);     /* one click: selects */
        }
        case 11:
            CHECK(ui_test_sel() == find_tile("Dvd Rip"), "a click selects");
            CHECK(ui_test_badge(find_tile("Dvd Rip")) == 1 && ui_test_badge(find_tile("Big Buck Bunny")) == 0,
                  "a tick on the film watched, none on one part watched");
            pc++;
            return ev_tile(b, find_tile("Dvd Rip"), 2);         /* Menu */
        case 12:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_SAVE), "Save original file") &&
                  menu_sub(menu_open, MI_SAVE) == -1 && !menu_shaded(menu_open, MI_SAVE),
                  "4.7GB: no save box");
            CHECK(menu_open && menu_shaded(menu_open, MI_RESUME), "no Resume without a place to resume from");
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_DETAILS), "Details...") &&
                  !strcmp(menu_text(menu_open, MI_SUBS), "Subtitles") && menu_shaded(menu_open, MI_SUBS),
                  "Details; no subtitle tracks to choose");
            prev_reports = reports;
            pc++;
            return ev_menu(b, MI_SAVE, -1);
        case 13:
            CHECK(reports == prev_reports + 1 && strstr(last_report, "4.7GB") && strstr(last_report, "can't be saved"),
                  "the 4.7GB file refused: %s", last_report);
            CHECK(!ui_test_saving(), "and not saved");
            setenv("ReelEGL$Dir", "SDFS::Pi.$.Apps.!ReelEGL", 1);
            setenv("Reel$Dir", "SDFS::Pi.$.Apps.!Reel", 1);
            pc = 130;
            memset(b, 0, 32);                                   /* scrolled down a little first */
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[5] = 0; b[6] = -40; b[7] = -1;
            return 2;
        case 130:
            CHECK(win(w_browser)->sy == -40, "scrolled");
            pc = 14;
            return ev_tile(b, find_tile("Big Buck Bunny"), 4);  /* double-click: the details */
        /* ---- the details window */
        case 14: {
            CHECK(ui_test_page() == PG_DETAILS && win(w_browser)->open && win(w_browser)->sy == 0,
                  "double-click on a video: its details, in the same window, from the top");
            CHECK(!strcmp(ui_test_path(), "Attic > Films > Big Buck Bunny"), "where: %s", ui_test_path());
            CHECK(!strcmp(ui_test_det(0), "Big Buck Bunny"), "title %s", ui_test_det(0));
            CHECK(strstr(ui_test_det(1), "2008") && strstr(ui_test_det(1), "1h 30m") && strstr(ui_test_det(1), "PG") &&
                  strstr(ui_test_det(1), "7.5"), "year, time, rating: %s", ui_test_det(1));
            CHECK(!strncmp(ui_test_det(3), "Big Buck Bunny: a test film.", 28), "summary: %s", ui_test_det(3));
            CHECK(ui_test_button(D_PLAY) && ui_test_button(D_RESUME) && !strcmp(ui_test_button(D_RESUME), "Resume from 42:10") &&
                  ui_test_button(D_START) && ui_test_button(D_SAVE) && !strcmp(ui_test_button(D_WATCHED), "Mark watched") &&
                  ui_test_button(D_SUBS) && !strcmp(ui_test_button(D_SUBS), "Subtitles: None"), "the buttons");
            CHECK(strstr(ui_test_det(2), "Transcoded (") && strstr(ui_test_det(2), "bigger"),
                  "how it will play: %s", ui_test_det(2));
            CHECK(log_count("/library/metadata/101", NULL, NULL) >= 1 &&
                  log_count("/photo/:/transcode", "url", "/library/metadata/101/art/1700000000") == 1 &&
                  log_count("/photo/:/transcode", "width", "554") >= 1, "the details and the backdrop fetched (%d, %d, %d)",
                  log_count("/library/metadata/101", NULL, NULL),
                  log_count("/photo/:/transcode", "url", "/library/metadata/101/art/1700000000"),
                  log_count("/photo/:/transcode", "width", "554"));
            pc++;
            return ev_redraw(b, w_browser);
        }
        case 15: {
            const int *spr = plotted_wide ? plotted_wide + 4 : NULL, *pos = plotted_area ? plotted_area + 4 : NULL;
            int w = spr ? spr[4] + 1 : 0, h = spr ? spr[5] + 1 : 0;
            unsigned bottom = spr ? ((const unsigned *)(spr + 11))[(h - 1) * w + w / 2] : 0;
            CHECK(spr && w == 554 && h == 302 && spr[9] == spr[8],
                  "the backdrop: the window's width, at most 55%% of its height, no mask (%d x %d)", w, h);
            CHECK(abs((int)(bottom & 255) - 0x18) < 3 && abs((int)(bottom >> 8 & 255) - 0x1A) < 3 &&
                  abs((int)(bottom >> 16) - 0x1F) < 3, "faded to the window's grey at its foot (%06x)", bottom);
            CHECK(pos && pos[4] + 1 == 116 && pos[5] + 1 == 174 && pos[9] != pos[8],
                  "the poster beside it: 232 OS units (the narrowest), 2:3, rounded (%d x %d)", pos ? pos[4] + 1 : 0,
                  pos ? pos[5] + 1 : 0);
            CHECK(log_count("/photo/:/transcode", "width", "116") >= 1, "the poster fetched at that size");
            CHECK(strstr(plotted_text, "|2008|1h 30m|PG|Critics 7.5|Audience 8.1|Animation|Comedy|Short|"),
                  "the labels: the facts, then the genres: %s", plotted_text);
            CHECK(strstr(plotted_text, "Big Buck Bunny|") && strstr(plotted_text, "Play|") &&
                  strstr(plotted_text, "Resume from 42:10|") && strstr(plotted_text, "Subtitles: None|"),
                  "the details drawn: %s", plotted_text);
            save_picture("details.ppm", w_browser);
            CHECK(!(mask & 1) || last_poll == 0x400E1, "null events: the cast's photos to fetch");
            drain_n = 0;
            pc = 1500;
            return NULL_EVENT;
        }
        case 1500:                                               /* the cast's photos, one a null event */
            DRAIN(10);
            if (pc != 1501)
                return NULL_EVENT;
            continue;
        case 1501:
            CHECK(log_count("/photo/:/transcode", "url", "https://metadata-static.plex.tv/people/frank.jpg") == 1 &&
                  log_count("/photo/:/transcode", "url", "https://metadata-static.plex.tv/people/bunny.jpg") == 1 &&
                  log_count("/photo/:/transcode", "url", "https://metadata-static.plex.tv/people/gamera.jpg") == 1 &&
                  log_count("/photo/:/transcode", "width", "72") >= 3, "the cast's photos fetched, 72 pixels");
            memset(b, 0, 32);                                   /* taller: the backdrop grows */
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[2] -= 500; b[7] = -1;
            pc = 15010;
            return 2;
        case 15010:
            CHECK(last_poll == 0x400E1 && idle_time - fake_cs == 50, "the backdrop fetched again once resizing stops (%d cs)",
                  idle_time - fake_cs);
            CHECK(log_count("/photo/:/transcode", "height", "311") == 0, "not while it's being resized");
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 15011:
            CHECK(log_count("/photo/:/transcode", "height", "311") == 1 && log_count("/photo/:/transcode", "width", "554") >= 2,
                  "then at the new size: 554 x 311, 16:9");
            memset(b, 0, 32);                                   /* back as it was */
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[2] += 500; b[7] = -1;
            pc++;
            return 2;
        case 15012:
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 15013:
            memset(b, 0, 32);                                   /* down to the cast */
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[6] = -1300; b[7] = -1;
            pc = 1502;
            return 2;
        case 1502:
            pc = 1503;
            return ev_redraw(b, w_browser);
        case 1503: {
            const int *spr = plotted_area ? plotted_area + 4 : NULL;
            CHECK(strstr(plotted_text, "Directed by|Sacha Goedegebure|") &&
                  strstr(plotted_text, "Released|10 April 2008|") && strstr(plotted_text, "Critics 7.5") &&
                  strstr(plotted_text, "Audience 8.1") && strstr(plotted_text, "1080p H.264") && strstr(plotted_text, "AAC 5.1"),
                  "the credits: %s", plotted_text);
            CHECK(strstr(plotted_text, "Cast|") && strstr(plotted_text, "Frank|Flying s") &&
                  strstr(plotted_text, "Gamera|Chinchilla|"), "the cast, with their parts");
            CHECK(spr && spr[4] + 1 == 72 && spr[9] != spr[8], "a photo: round (masked), 72 pixels");
            {
                int x, y;
                CHECK(strstr(plotted_text, "More like this|Hevc Film|2020|Sixty|2021|"), "More like this: %s", plotted_text);
                CHECK(log_count("/library/metadata/101/extras", NULL, NULL) >= 1 && ui_test_button_xy(w_browser, 2000, &x, &y) == 0,
                      "and its trailer, under it");
            }
            save_picture("cast.ppm", w_browser);
            pc = 15030;
            return ev_button(b, w_browser, 1000, 0x400);        /* More like this: Hevc Film */
        }
        case 15030:
            CHECK(ui_test_page() == PG_DETAILS && !strcmp(ui_test_det(0), "Hevc Film") &&
                  log_count("/library/metadata/102", NULL, NULL) >= 1, "its details, in place: %s", ui_test_det(0));
            pc++;
            return ev_key(b, w_browser, -1, 8);                 /* Back: the films */
        case 15031:
            CHECK(ui_test_page() == PG_GRID, "Back: the films");
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 4);
        case 15032: {
            CHECK(ui_test_page() == PG_DETAILS && !strcmp(ui_test_det(0), "Big Buck Bunny"), "Big Buck Bunny again");
            memset(b, 0, 32);                                   /* back to the top */
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[6] = 0; b[7] = -1;
            prev_started = nstarted;
            pc = 16;
            return 2;
        }
        case 16: {
            int k;
            if (nstarted == prev_started && ev_button(b, w_browser, D_PLAY, 4))
                return 6;                                       /* Play (after the scroll back) */
            k = (nstarted - 1) & 7;
            snprintf(want, sizeof(want), "Run <ReelEGL$Dir>.!Run %s/Matinee/Play0", scrap);
            CHECK(nstarted == prev_started + 1 && !strcmp(started[k], want), "Play: started %s", started[k]);
            CHECK(strstr(started_url(), "/video/:/transcode/universal/start.m3u8?") &&
                  strstr(started_url(), "&offset=2530&") && strstr(started_url(), "videoResolution=1280x720"),
                  "720p (the default): converted, from where it was left: %s", started_url());
            CHECK(started_nsrc[k] == 1 && started_src[k].title && !strcmp(started_src[k].title, "Big Buck Bunny") &&
                  started_src[k].headers && strstr(started_src[k].headers, "X-Plex-Token: SRV-TOKEN"),
                  "title and headers for Reel");
            CHECK(ntyped && typed_type[(ntyped - 1) & 7] == 0xBF4, "typed as video/mp4");
            snprintf(want, sizeof(want), "%s/Matinee/Play0", scrap);
            CHECK(!file_exists(want), "the file deleted once the player has started");
            CHECK(strstr(ui_test_status(), "Transcoded (") && strstr(ui_test_status(), "bigger"), "status: %s",
                  ui_test_status());
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);             /* the icon bar menu */
        }
        case 17:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MB_QUALITY), "Quality") &&
                  !strcmp(menu_text(menu_open, MB_SIZE), "Poster size") &&
                  menu_flags(menu_open, MB_DIRECT) & 1, "icon bar menu; Direct play ticked");
            CHECK(info_sub == 0x7000, "Info leads to the Info window");
            pc++;
            return ev_menu(b, MB_QUALITY, 0);                   /* 1080p */
        case 18:
            CHECK(strstr(read_file(choices), "quality 0\n") != NULL, "quality kept");
            CHECK(strstr(ui_test_det(2), "Direct Play:"), "the details say so: %s", ui_test_det(2));
            tasks[0].handle = 0x777; tasks[0].name = "ReelEGL\r";
            tasks[1].handle = 0x778; tasks[1].name = "Reel\r";
            tasks[2].handle = task; tasks[2].name = "Matinee\r";
            ntasks = 3;
            prev_nsent = nsent;
            prev_started = nstarted;
            pc++;
            return ev_key(b, w_browser, -1, 13);                    /* Return in the details: Play */
        case 19: {
            const sent_t *s = last_sent(5);
            CHECK(s && nsent == prev_nsent + 1 && s->reason == 18 && s->to == 0x777 && s->b[10] == 0xBF4,
                  "DataOpen, recorded, to ReelEGL alone, typed as video");
            CHECK(s && s->file_there && s->nsrc == 1, "the file's there for ReelEGL to read");
            snprintf(want, sizeof(want), "%s/library/parts/11/101/file.mp4", base);
            CHECK(s && s->nsrc == 1 && !strcmp(s->src.url, want) && s->src.key && !strcmp(s->src.key, "plex:MID/101"),
                  "1080p: direct play, with Reel's carry-on key");
            CHECK(nstarted == prev_started, "nothing started");
            pc++;
            return ev_msg(b, 4, s ? s->b[2] : 0, 0x777);        /* DataLoadAck */
        }
        case 20:
            snprintf(want, sizeof(want), "%s/Matinee/Play1", scrap);
            CHECK(!file_exists(want), "deleted on DataLoadAck");
            CHECK(strstr(ui_test_status(), "in ReelEGL") && strstr(ui_test_status(), "Direct Play"),
                  "status: %s", ui_test_status());
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 21:
            pc++;
            return ev_menu(b, MB_PLAYER, PLAYER_REEL);          /* Reel */
        case 22:
            CHECK(strstr(read_file(choices), "player Reel\n") != NULL, "player kept");
            pc++;
            return ev_key(b, w_browser, -1, 0x18D);             /* Right: the next video's details */
        case 23:
            CHECK(!strcmp(ui_test_det(0), "Hevc Film") && ui_test_button(D_SUBS) == NULL &&
                  ui_test_page() == PG_DETAILS, "Right: the next video: %s", ui_test_det(0));
            prev_nsent = nsent;
            prev_started = nstarted;
            pc++;
            return ev_key(b, w_browser, -1, 13);                /* Return: play */
        case 24: {
            const sent_t *s = last_sent(5);
            CHECK(s && nsent == prev_nsent + 1 && s->to == 0x778, "DataOpen to Reel, not ReelEGL");
            CHECK(s && s->nsrc == 1 && strstr(s->src.url, "start.m3u8") && !s->src.key, "HEVC converted, no key");
            pc++;
            memcpy(b, s ? s->b : b, 256);
            return 19;                                          /* nobody claimed it: it comes back */
        }
        case 25: {
            int k = (nstarted - 1) & 7;
            CHECK(nstarted == prev_started + 1 && !strncmp(started[k], "Run <Reel$Dir>.!Run ", 20),
                  "unclaimed: Reel started: %s", started[k]);
            CHECK(strstr(started_url(), "start.m3u8"), "Reel reads the same file");
            ntasks = 1;
            tasks[0].handle = task; tasks[0].name = "Matinee\r";
            unsetenv("Reel$Dir");
            prev_reports = reports;
            prev_started = nstarted;
            pc++;
            return ev_key(b, w_browser, -1, 13);
        }
        case 26:
            CHECK(reports == prev_reports + 1 && strstr(last_report, "Reel hasn't been seen by the Filer"),
                  "no Reel$Dir: %s", last_report);
            CHECK(nstarted == prev_started, "nothing started");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 27:
            pc++;
            return ev_menu(b, MB_PLAYER, PLAYER_REELEGL);       /* back to ReelEGL */
        case 28:
            pc = 280;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape: back to the grid */
        case 280:
            CHECK(ui_test_page() == PG_GRID && win(w_browser)->sy == -40 && ui_test_sel() == find_tile("Hevc Film"),
                  "back to the grid, where it was (%d), the last video selected", win(w_browser)->sy);
            pc = 29;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);  /* Menu on it */
        case 29:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_RESUME), "Resume from 42:10") &&
                  !menu_shaded(menu_open, MI_RESUME), "Resume from: %s", menu_open ? menu_text(menu_open, MI_RESUME) : "");
            CHECK(menu_open && menu_sub(menu_open, MI_SAVE) == w_save, "the save box is Save's submenu");
            CHECK(menu_open && !menu_shaded(menu_open, MI_SUBS) && menu_sub(menu_open, MI_SUBS) > 0x10000,
                  "Subtitles has its submenu");
            prev_started = nstarted;
            pc++;
            return ev_menu(b, MI_RESUME, -1);
        case 30:
            CHECK(nstarted == prev_started + 1 && strstr(started_url(), "start.m3u8") &&
                  strstr(started_url(), "&offset=2530&") && strstr(started_url(), "directStream=1"),
                  "Resume: the server streams from 42:10 (direct play off)");
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        case 31:
            prev_started = nstarted;
            pc++;
            return ev_menu(b, MI_START, -1);
        case 32: {
            int k = (nstarted - 1) & 7;
            snprintf(want, sizeof(want), "%s/library/parts/11/101/file.mp4", base);
            CHECK(nstarted == prev_started + 1 && !strcmp(started_url(), want) && !started_src[k].key,
                  "Play from start: direct, without the carry-on key");
            pc = 320;
            return ev_tile(b, find_tile("Big Buck Bunny"), 4);  /* its details */
        case 320:
            CHECK(ui_test_page() == PG_DETAILS && !strcmp(ui_test_det(0), "Big Buck Bunny"), "details again");
            pc = 33;
            return ev_button(b, w_browser, D_SUBS, 4);          /* Subtitles */
        }
        /* ---- subtitles */
        case 33:
            CHECK(menu_open && !strcmp(menu_text(menu_open, 0), "None") && menu_flags(menu_open, 0) & 1 &&
                  !strcmp(menu_text(menu_open, 1), "English (SRT)") &&
                  !strcmp(menu_text(menu_open, 2), "English (SRT External)") &&
                  !strcmp(menu_text(menu_open, 3), "French Forced (PGS)") && menu_flags(menu_open, 3) & 0x80,
                  "the subtitles menu");
            pc++;
            return ev_menu(b, 2, -1);                           /* English (SRT External) */
        case 34: {
            cJSON *log = server_log(), *r;
            const cJSON *put = NULL;
            cJSON_ArrayForEach(r, log)
                if (!strcmp(cJSON_GetObjectItem(r, "method")->valuestring, "PUT"))
                    put = r;
            CHECK(put && !strcmp(cJSON_GetObjectItem(put, "path")->valuestring, "/library/parts/11101") &&
                  strstr(cJSON_PrintUnformatted(cJSON_GetObjectItem(put, "query")), "\"subtitleStreamID\":[\"1002\"]"),
                  "chosen on the server");
            cJSON_Delete(log);
            CHECK(ui_test_button(D_SUBS) && !strcmp(ui_test_button(D_SUBS), "Subtitles: English (SRT External)"),
                  "the details show it: %s", ui_test_button(D_SUBS));
            CHECK(strstr(ui_test_det(2), "subtitles burnt in: English (SRT External))"), "how: %s", ui_test_det(2));
            CHECK(strstr(ui_test_status(), "Subtitles: English (SRT External)"), "status %s", ui_test_status());
            prev_started = nstarted;
            pc++;
            return ev_button(b, w_browser, D_PLAY, 4);
        }
        case 35:
            CHECK(nstarted == prev_started + 1 && strstr(started_url(), "start.m3u8") &&
                  strstr(started_url(), "subtitles=burn"), "subtitles: converted, burnt in (%s)", started_url());
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        case 36:
            pc++;
            return ev_menu(b, MI_SUBS, 0);                      /* None, from the poster's menu */
        case 37:
            CHECK(ui_test_button(D_SUBS) && !strcmp(ui_test_button(D_SUBS), "Subtitles: None"), "none again");
            CHECK(strstr(ui_test_status(), "Subtitles off"), "status %s", ui_test_status());
            pc++;
            return ev_button(b, w_browser, D_WATCHED, 4);
        case 38:
            CHECK(log_count("/:/scrobble", "key", "101") == 1, "marked watched on the server");
            CHECK(!strcmp(ui_test_button(D_WATCHED), "Mark unwatched"), "the button turns round");
            CHECK(strstr(ui_test_status(), "watched"), "status: %s", ui_test_status());
            /* ---- your rating: the stars */
            CHECK(ui_test_button(D_STAR) && !strcmp(ui_test_button(D_STAR), "-") && !strcmp(ui_test_button(D_STAR + 4), "-"),
                  "five stars, none lit");
            pc = 3801;
            return ev_button(b, w_browser, D_STAR + 3, 4);
        case 3801:
            CHECK(log_count("/:/rate", "rating", "8") == 1 && log_count("/:/rate", "key", "101") == 1 &&
                  log_count("/:/rate", "identifier", "com.plexapp.plugins.library") == 1, "the fourth star: rated 8 (of 10)");
            CHECK(!strcmp(ui_test_button(D_STAR + 3), "*") && !strcmp(ui_test_button(D_STAR), "*") &&
                  !strcmp(ui_test_button(D_STAR + 4), "-"), "four stars lit");
            CHECK(strstr(ui_test_status(), "Rated 4 stars"), "status: %s", ui_test_status());
            pc++;
            return ev_button(b, w_browser, D_STAR + 3, 4);
        case 3802:
            CHECK(log_count("/:/rate", "rating", "-1") == 1 && !strcmp(ui_test_button(D_STAR), "-"),
                  "the same star again: the rating taken away");
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        case 3803:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_RATE), "Rate") && !menu_shaded(menu_open, MI_RATE) &&
                  menu_sub(menu_open, MI_RATE) > 0x10000, "the poster's menu: Rate");
            {
                const int *rm = menu_open ? (const int *)(intptr_t)menu_sub(menu_open, MI_RATE) : NULL;
                CHECK(rm && !strcmp(menu_text(rm, 0), "No rating") && (menu_flags(rm, 0) & 1) &&
                      !strcmp(menu_text(rm, 2), "2 stars"), "No rating (ticked), 1 star... 5 stars");
            }
            pc++;
            return ev_menu(b, MI_RATE, 2);
        case 3804:
            CHECK(log_count("/:/rate", "rating", "4") == 1 && !strcmp(ui_test_button(D_STAR + 1), "*") &&
                  !strcmp(ui_test_button(D_STAR + 2), "-"), "2 stars from the menu: rated 4, shown on the page");
            pc = 3805;
            return ev_redraw(b, w_browser);
        case 3805:                                          /* scrolled to the stars, for a picture */
            memset(b, 0, 32);
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[5] = 0; b[6] = -360; b[7] = -1;
            pc++;
            return 2;
        case 3806:
            pc++;
            return ev_redraw(b, w_browser);
        case 3807:
            save_picture("rating.ppm", w_browser);
            memset(b, 0, 32);
            memcpy(b + 1, win(w_browser)->vis, 16);
            b[0] = w_browser; b[5] = 0; b[6] = 0; b[7] = -1;
            pc++;
            return 2;
        case 3808:
            /* ---- Save original file, from the details */
            pc = 39;
            return ev_button(b, w_browser, D_SAVE, 4);
        case 39:
            CHECK(menu_window == w_save, "Save file: the save box, as a menu");
            CHECK(!strcmp(icon_text(w_save, SV_NAME), "Big\xa0" "Buck\xa0" "Bunny\xa0(2008)/mp4"), "save as: %s",
                  icon_text(w_save, SV_NAME));
            CHECK(!strcmp(icon_text(w_save, SV_FILE), "file_bf4"), "file icon %s", icon_text(w_save, SV_FILE));
            win(w_save)->open = 1;
            pc++;
            return ev_click(b, w_save, SV_FILE, 700, 500, 0x40);      /* drag the icon */
        case 40:
            CHECK(drag_started == 1, "DragASprite");
            pointer_w = 0x9000; pointer_i = 3;                       /* dropped on a Filer window */
            prev_nsent = nsent;
            pc++;
            return 7;                                                /* User_Drag_Box */
        case 41: {
            const sent_t *s = last_sent(1);
            CHECK(s && nsent == prev_nsent + 1 && s->reason == 17 && s->to == 0x9000 && s->icon == 3 &&
                  s->b[5] == 0x9000 && s->b[10] == 0xBF4 && s->b[9] == 0x7FFFFFFF &&
                  !strcmp((const char *)&s->b[11], "Big\xa0" "Buck\xa0" "Bunny\xa0(2008)/mp4"),
                  "DataSave to the Filer window");
            pointer_w = pointer_i = -1;
            snprintf(save_path, sizeof(save_path), "%s/saved/bbb.mp4", outdir);
            ev_msg(b, 2, s ? s->b[2] : 0, 0x400);                    /* DataSaveAck */
            b[9] = 1234;
            snprintf((char *)&b[11], 200, "%s", save_path);
            menus_closed = 0;
            pc++;
            return 17;
        }
        case 42:
            CHECK(ui_test_saving(), "saving");
            CHECK(menus_closed == 1, "the menu closed");
            CHECK(!(mask & 1), "null events while saving");
            save_nulls = 0;
            pc++;
            continue;
        case 43:
            if (ui_test_saving()) {
                if (mask & 1) {
                    poll_null_mask_bad = 1;
                    pc++;
                    continue;
                }
                save_nulls++;
                if (save_nulls == 3)
                    CHECK(strstr(ui_test_status(), "Saving Big Buck Bunny: 0 of 5 MB (10%)") ||
                          strstr(ui_test_status(), "Saving Big Buck Bunny: 1 of 5 MB (10%)"),
                          "progress: %s", ui_test_status());
                if (save_nulls == 15)
                    CHECK(strstr(ui_test_status(), ", 2.5 MB/s"), "how fast: %s", ui_test_status());
                fake_cs += 10;                                  /* 256KB every 0.1 s */
                return NULL_EVENT;
            }
            pc++;
            continue;
        case 44: {
            long len;
            unsigned sum = file_sum(save_path, &len);
            CHECK(!poll_null_mask_bad, "nulls all the way");
            CHECK(save_nulls == 20, "256KB a null event: %d nulls for 5MB", save_nulls);
            CHECK(len == 5 * 1024 * 1024 && sum == 0x2d800000u, "the file saved whole: %ld bytes, %08x", len, sum);
            CHECK(ntyped && !strcmp(typed_path[(ntyped - 1) & 7], save_path) && typed_type[(ntyped - 1) & 7] == 0xBF4,
                  "typed from its extension");
            CHECK(strstr(ui_test_status(), "Saved Big Buck Bunny (5 MB)"), "status: %s", ui_test_status());
            pc = 440;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);  /* the menu: the speed test */
        }
        case 440:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_SPEED), "Test speed") && !menu_shaded(menu_open, MI_SPEED),
                  "Test speed on the menu");
            prev_reports = reports;
            prev_count = log_count("/library/parts/11/101/file.mp4", NULL, NULL);
            pc++;
            return ev_menu(b, MI_SPEED, -1);
        case 441:
            CHECK(ui_test_speed() && !(mask & 1), "the speed test runs, on null events");
            CHECK(log_count("/library/parts/11/101/file.mp4", NULL, NULL) == prev_count + 1, "the file asked for");
            speed_nulls = 0;
            pc++;
            continue;
        case 442:
            if (ui_test_speed()) {
                speed_nulls++;
                fake_cs += 50;                                  /* 256KB every 0.5 s: a slow server */
                return NULL_EVENT;
            }
            pc++;
            continue;
        case 443:
            CHECK(speed_nulls == 16, "stopped after 8 s: %d nulls", speed_nulls);
            CHECK(reports == prev_reports + 1 &&
                  strstr(last_report, "Speed test, Big Buck Bunny: 4.0 MB in 8.0 s: 4.2 Mbit/s, over http") &&
                  strstr(last_report, "It needs 5.0 Mbit/s: too slow to play directly. Untick Direct play, or save the file first."),
                  "the result: %s", last_report);
            CHECK(strstr(ui_test_status(), "Speed test: 4.2 Mbit/s"), "status: %s", ui_test_status());
            pc = 45;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);  /* again, from the menu, stopped part way */
        case 45:
            pc++;
            return ev_click(b, w_save, SV_FILE, 700, 500, 0x40);
        case 46:
            pointer_w = 0x9000; pointer_i = 3;
            pc++;
            return 7;
        case 47: {
            const sent_t *s = last_sent(1);
            snprintf(save_path2, sizeof(save_path2), "%s/saved/bbb2.mp4", outdir);
            ev_msg(b, 2, s ? s->b[2] : 0, 0x400);
            b[9] = 1234;
            snprintf((char *)&b[11], 200, "%s", save_path2);
            wait_save = 0;
            pc++;
            return 17;
        }
        case 48:
            if (wait_save++ < 2)
                return NULL_EVENT;
            CHECK(ui_test_saving() && file_exists(save_path2), "saving the second");
            pointer_w = pointer_i = -1;
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        case 49:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_SAVE), "Stop saving"), "Stop saving on the menu");
            pc++;
            return ev_menu(b, MI_SAVE, -1);
        case 50:
            CHECK(!ui_test_saving() && !file_exists(save_path2), "stopped, the half file deleted");
            CHECK(strstr(ui_test_status(), "Stopped saving"), "status: %s", ui_test_status());
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape: back */
        case 51:
            CHECK(ui_test_page() == PG_GRID && win(w_browser)->sy == -40, "Escape: the grid, where it was (%d)",
                  win(w_browser)->sy);
            pc++;
            return ev_key(b, w_browser, -1, 'I');               /* I: details again */
        case 52:
            CHECK(ui_test_page() == PG_DETAILS && !strcmp(ui_test_det(0), "Big Buck Bunny"), "I: details");
            pc++;
            return ev_button(b, w_browser, B_BACK, 0x400);      /* the Back button */
        case 53:
            CHECK(ui_test_page() == PG_GRID, "Back: the grid");
            /* ---- TV: show, season, episodes, Back */
            pc++;
            return ev_key(b, w_browser, -1, 8);                 /* Backspace */
        case 54:
            {
                int cur = -1;
                CHECK(ui_test_items() == 9 && !strcmp(ui_test_path(), "Attic") && ui_test_tab(&cur) && cur == 0,
                      "Back: home again (%d, %s)", ui_test_items(), ui_test_path());
            }
            pc++;
            return ev_button(b, w_browser, TB_TAB + 2, 0x400);  /* the TV Programmes tab */
        case 55:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_item(0, 1), "3 seasons") && ui_test_badge(0) == 2,
                  "a show, with the count of episodes not seen");
            pc = 551;
            return ev_redraw(b, w_browser);
        case 551:
            CHECK(strstr(plotted_text, "|10|Space Show|3 seasons|"), "its badge: 10 episodes unwatched: %s", plotted_text);
            pc = 56;
            return ev_key(b, w_browser, -1, 13);                /* Return opens the one selected */
        case 56: {
            int se = -1;
            CHECK(ui_test_show(&se) && se == 1 && !strcmp(ui_test_path(), "Attic > TV Programmes > Space Show"),
                  "a show: its page, Series 1 (the first with episodes unwatched, not Specials): %s", ui_test_path());
            CHECK(ui_test_items() == 6 && !strcmp(ui_test_item(2, 0), "3. Episode '3'") &&
                  !strcmp(ui_test_item(2, 1), "45 min"), "its episodes as rows, Latin-1: %s / %s", ui_test_item(2, 0),
                  ui_test_item(2, 1));
            CHECK(ui_test_sel() == 2, "the first not seen, selected (%d)", ui_test_sel());
            CHECK(ui_test_badge(0) == 1 && ui_test_badge(2) == 0, "ticks on the episodes seen");
            CHECK(!strcmp(ui_test_show_text(1), "6 episodes   \xb7   4 unwatched") &&
                  !strcmp(ui_test_show_text(2), "Play S1 E3|Mark watched|"), "the count, and the buttons: %s / %s",
                  ui_test_show_text(1), ui_test_show_text(2));
            CHECK(log_count("/library/metadata/20", NULL, NULL) == 1 && log_count("/photo/:/transcode", "url", "/library/metadata/20/art/1") >= 1,
                  "the show's details, and its backdrop");
            pc++;
            return ev_redraw(b, w_browser);
        }
        case 57:
            CHECK(strstr(plotted_text, "Space Show|2020|3 series|12|8.2|10 unwatched|Five strangers") &&
                  strstr(plotted_text, "Specials|Series 1|Series 2|6 episodes") && strstr(plotted_text, "1. Episode '1'|45 min|"),
                  "the show page drawn: %s", plotted_text);
            save_picture("show.ppm", w_browser);
            drain_n = 0;
            pc = 570;
            return NULL_EVENT;
        case 570:                                           /* the episodes' pictures */
            DRAIN(10);
            if (pc != 571)
                return NULL_EVENT;
            continue;
        case 571:
            CHECK(log_count("/photo/:/transcode", "url", "/library/metadata/211/thumb/1700000000") == 1 &&
                  log_count("/photo/:/transcode", "width", "160") >= 2, "the episodes' pictures: 16:9, 160 pixels (%d, %d)", log_count("/photo/:/transcode", "url", "/library/metadata/211/thumb/1700000000"), log_count("/photo/:/transcode", "width", "160"));
            pc++;
            return ev_key(b, w_browser, -1, 0x18E);             /* Down: the next episode */
        case 572:
            CHECK(ui_test_sel() == 3, "Down: the next row (%d)", ui_test_sel());
            pc++;
            return ev_button(b, w_browser, SH_TAB + 2, 0x400);  /* Series 2 */
        case 573: {
            int se = -1;
            CHECK(ui_test_show(&se) && se == 2 && ui_test_items() == 6 && !strcmp(ui_test_item(0, 0), "1. Episode '1'") &&
                  log_count("/library/metadata/22/children", NULL, NULL) == 1, "Series 2's episodes, in place");
            CHECK(!strcmp(ui_test_show_text(2), "Play S2 E1|Mark watched|"), "Play: its first (%s)", ui_test_show_text(2));
            memset(b, 0, 32);                                   /* as wide as the window opens */
            b[0] = w_browser; b[1] = 480; b[2] = 388; b[3] = 480 + 2840; b[4] = 388 + 1478; b[7] = -1;
            n_null = 0;
            pc = 574;
            return 2;
        }
        case 574:
            if (n_null++ < 12) {                                /* its pictures at that size */
                fake_cs += 20;
                return NULL_EVENT;
            }
            pc++;
            return ev_redraw(b, w_browser);
        case 575:
            save_picture("show-wide.ppm", w_browser);
            CHECK(log_count("/photo/:/transcode", "url", "/library/metadata/20/art/1") >= 2, "the backdrop again, wider");
            memset(b, 0, 32);                                   /* back as it was */
            b[0] = w_browser; b[1] = 1366; b[2] = 570; b[3] = 1366 + 1108; b[4] = 570 + 1100; b[7] = -1;
            pc = 59;
            return 2;
        case 59:
            pc = 60;
            return ev_key(b, w_browser, -1, 0x1CC);             /* F12: not ours */
        case 60:
            CHECK(keys_passed == 1, "other keys passed on");
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape */
        case 61:
            CHECK(!strcmp(ui_test_path(), "Attic > TV Programmes") && !ui_test_show(&count0),
                  "Escape goes back to where the show was opened (not Series 1): %s", ui_test_path());
            pc++;
            return ev_key(b, w_browser, -1, 0x7F);              /* Delete: back too */
        case 62:
            pc = 63;
            continue;
        case 63:
            CHECK(!strcmp(ui_test_path(), "Attic"), "the top: %s", ui_test_path());
            prev_count = log_count("/library/sections", NULL, NULL);
            pc++;
            return ev_button(b, w_browser, B_REFRESH, 0x400);   /* Refresh */
        case 64:
            CHECK(log_count("/library/sections", NULL, NULL) == prev_count + 1 && ui_test_items() == 9,
                  "Refresh fetches the list again");
            pc++;
            return ev_button(b, w_browser, TB_TAB + 3, 0x400);  /* the Music tab */
        case 65:
            CHECK(ui_test_items() == 9 && strstr(ui_test_status(), "can't be opened yet"), "music: %s",
                  ui_test_status());
            pc = 6500;
            return ev_button(b, w_browser, TB_TAB + 1, 0x400);  /* the grid, for the next steps */
        case 6500: {
            int v = -1, so = -1, un = -1;
            CHECK(ui_test_lib(&v, &so, &un) && v == 0 && so == 0 && !un, "Films: its bar (Library, by title)");
            CHECK(ui_test_az_letter("The Matrix") == 13 && ui_test_az_letter("An Owl") == 15 &&
                  ui_test_az_letter("A Bug's Life") == 2 && ui_test_az_letter("Aardvark") == 1 &&
                  ui_test_az_letter("2001") == 0, "A to Z: The, An and A left off, as Plex sorts; digits under #");
            pc++;
            return ev_redraw(b, w_browser);
        }
        case 6501:
            save_picture("library.ppm", w_browser);
            CHECK(strstr(plotted_text, "Library|Collections|Playlists|Unwatched|Sort: Title|#|A|B|C|"),
                  "the bar: the views, Unwatched, Sort, A to Z: %s", plotted_text);
            pc++;
            return ev_button(b, w_browser, LB_AZ + 'R' - 'A' + 1, 0x400);   /* R */
        case 6502:
            CHECK(ui_test_sel() == find_tile("Remux"), "R: the first title with R selected (%d)", ui_test_sel());
            pc++;
            return ev_button(b, w_browser, LB_SORT, 0x400);
        case 6503:
            CHECK(menu_open && !strcmp(menu_text(menu_open, 2), "Year") && (menu_flags(menu_open, 0) & 1), "the Sort menu");
            pc++;
            return ev_menu(b, 2, -1);                           /* Year */
        case 6504: {
            int v = -1, so = -1, un = -1, cur = -1;
            CHECK(ui_test_lib(&v, &so, &un) && so == 2 && log_count("/library/sections/1/all", "sort", "year:desc") == 1 &&
                  !strcmp(ui_test_item(0, 0), "Sixty") && !strcmp(ui_test_item(5, 0), "Dvd Rip"), "by year, newest first");
            CHECK(ui_test_tab(&cur) && cur == 1, "still in the Films tab");
            pc++;
            return ev_button(b, w_browser, LB_UNWATCHED, 0x400);
        }
        case 6505: {
            int v = -1, so = -1, un = -1;
            CHECK(ui_test_lib(&v, &so, &un) && so == 2 && un && ui_test_items() == 5 && find_tile("Dvd Rip") < 0,
                  "Unwatched: Dvd Rip (watched) left out, still by year");
            pc++;
            return ev_button(b, w_browser, LB_VIEW + 1, 0x400); /* Collections */
        }
        case 6506:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_item(0, 0), "Open Movies") && !strcmp(ui_test_item(0, 1), "2 items"),
                  "Collections");
            pc++;
            return ev_tile(b, 0, 4);
        case 6507:
            CHECK(ui_test_items() == 2 && !strcmp(ui_test_item(0, 0), "Big Buck Bunny"), "a collection's films");
            pc++;
            return ev_key(b, w_browser, -1, 8);                 /* Back: the collections */
        case 6508:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_item(0, 0), "Open Movies"), "Back: the collections");
            pc++;
            return ev_button(b, w_browser, LB_VIEW + 2, 0x400); /* Playlists */
        case 6509: {
            int cur = -1;
            CHECK(ui_test_items() == 2 && !strcmp(ui_test_item(0, 0), "Friday Night") && !strcmp(ui_test_item(0, 1), "3 items") &&
                  ui_test_tab(&cur) && cur == 1, "Playlists (in the Films tab)");
            pc++;
            return ev_tile(b, 0, 4);
        }
        case 6510:
            CHECK(ui_test_items() == 3 && !strcmp(ui_test_item(0, 0), "Ten Bit"), "a playlist's videos");
            pc++;
            return ev_button(b, w_browser, TB_TAB + 1, 0x400); /* the Films tab: as it was */
        case 6511: {
            int v = -1, so = -1, un = -1;
            CHECK(ui_test_lib(&v, &so, &un) && v == 0 && so == 0 && !un && ui_test_items() == 6, "Films again, by title");
            pc = 650;
            continue;
        }
        case 650:                                           /* the posters first */
            if (!(mask & 1))
                return NULL_EVENT;
            /* ---- the pointer over a poster */
            memset(b, 0, 64);
            b[0] = w_browser;
            pc = 66;
            return 5;                                           /* Pointer_Entering_Window */
        case 66: {
            int x, y;
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 10 && !(mask & 1), "the pointer watched (PollIdle 10cs)");
            ui_test_tile_xy(2, &x, &y);
            pointer_x = x; pointer_y = y; pointer_w = w_browser; pointer_i = -1;
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        }
        case 67:
            CHECK(ui_test_hover() == 2, "the poster under the pointer: %d", ui_test_hover());
            pointer_x = 640; pointer_y = 480; pointer_w = pointer_i = -1;
            memset(b, 0, 64);
            b[0] = w_browser;
            pc++;
            return 4;                                           /* Pointer_Leaving_Window */
        case 68:
            CHECK(ui_test_hover() == -1 && (mask & 1), "and gone when it leaves");
            /* ---- a narrower window: fewer columns */
            memset(b, 0, 32);
            b[0] = w_browser; b[1] = 100; b[2] = 100; b[3] = 100 + 2 * (232 + 36) + 36 + 10; b[4] = 1100;
            b[5] = 0; b[6] = 0; b[7] = -1;
            pc++;
            return 2;                                           /* Open_Window_Request */
        case 69: {
            int x0, y0, x1, y1;
            ui_test_tile_xy(0, &x0, &y0);
            ui_test_tile_xy(2, &x1, &y1);
            CHECK(x0 == x1 && y1 < y0, "two columns: the third tile on the second row");
            ev_msg(b, 0x400C1, 0, 0);                           /* mode change */
            b[0] = 20;
            pc++;
            return 17;
        }
        case 70:
            CHECK(ui_test_posters(NULL) == 0, "posters dropped on a mode change");
            pc++;
            continue;
        case 71:
            DRAIN(20);
            continue;
        case 72:
            CHECK(ui_test_posters(NULL) >= 1, "and fetched again");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 73:
            pc++;
            return ev_menu(b, MB_SIZE, 2);                      /* Large posters */
        case 74:
            CHECK(ui_test_psize() == 2 && strstr(read_file(choices), "poster_size 2\n") && ui_test_posters(NULL) == 0,
                  "Large: kept, posters to be made again");
            pc++;
            continue;
        case 75:
            DRAIN(20);
            continue;
        case 76:
            CHECK(log_count("/photo/:/transcode", "width", "160") >= 1 && log_count("/photo/:/transcode", "height", "240") >= 1,
                  "large posters: 160 x 240 pixels");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 77:
            pc = 7701;
            return ev_menu(b, MB_SIZE, 0);                      /* Small */
        case 7701: {
            int t = find_tile("Big Buck Bunny");
            const char *l = t >= 0 ? ui_test_item(t, 0) : "";
            CHECK(ui_test_page() != PG_GRID || ui_test_home(&t, &t) ||
                  (strlen(l) > 3 && !strcmp(l + strlen(l) - 3, "...") && strlen(l) < 14),
                  "Small posters: a title too long for the tile is cut (\"%s\")", l);
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 7702:
            pc = 899;
            return ev_menu(b, MB_SIZE, 1);
        case 899:
            pc = 900;                                           /* the built-in player next, from the top */
            return ev_key(b, w_browser, -1, 8);

        /* ---- the built-in player ------------------------------------------------ */
        case 900:
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 901: {
            int sub = menu_open ? menu_sub(menu_open, MB_PLAYER) : 0;
            const int *pm = (const int *)(intptr_t)sub;
            CHECK(pm && !strcmp(menu_text(pm, PLAYER_BUILTIN), "Built-in") && (menu_flags(pm, PLAYER_REELEGL) & 1),
                  "the Player menu: Built-in first, ReelEGL ticked (the test's Choices)");
            pc++;
            return ev_menu(b, MB_PLAYER, PLAYER_BUILTIN);
        }
        case 902:
            CHECK(strstr(read_file(choices), "player Built-in\n") != NULL, "Built-in kept");
            pc++;
            return ev_button(b, w_browser, TB_TAB + 1, 0x400);
        case 903: {
            int t = find_tile("Big Buck Bunny");
            CHECK(t >= 0, "Films open");
            pc++;
            return ev_tile(b, t, 4);
        }
        case 904:
            CHECK(ui_test_page() == PG_DETAILS, "its details");
            pc = 9040;
            memset(b, 0, 32);                   /* wider, for the player (as it opens: 75% of the screen) */
            b[0] = w_browser; b[1] = 480; b[2] = 388; b[3] = 480 + 2840; b[4] = 388 + 1478; b[7] = -1;
            return 2;
        case 9040:
            CHECK(ui_test_page() == PG_DETAILS, "still its details");
            CHECK(last_poll == 0x400E1, "the backdrop and the poster wanted at the new size");
            fake_cs = idle_time;
            n_null = 0;
            pc = 90404;
            return NULL_EVENT;
        case 90404:
            if (n_null++ < 6) {
                fake_cs += 20;
                return NULL_EVENT;
            }
            pc = 90405;
            continue;
        case 90405:
            CHECK(log_count("/photo/:/transcode", "url", "/library/metadata/101/art/1700000000") >= 2 && log_count("/photo/:/transcode", "width", "240") == 1,
                  "fetched at the window's new size: the backdrop 1420 pixels wide, the poster 240 (%d, %d, %d)",
                  log_count("/photo/:/transcode", "width", "1420"), log_count("/photo/:/transcode", "width", "240"), last_poll);
            pc = 9041;
            return ev_redraw(b, w_browser);
        case 9041:
            save_picture("details-wide.ppm", w_browser);
            pc = 905;
            fake_rc.len = 5400;
            open0 = fake_rc.opens;
            return ev_button(b, w_browser, D_PLAY, 0x400);                  /* Play: from where it was left */
        case 905:
            snprintf(want, sizeof(want), "%s/library/parts/11/101/file.mp4", base);
            CHECK(ui_test_page() == PG_PLAYER && ui_test_player(), "the player page");
            CHECK(fake_rc.opens == open0 + 1 && fake_rc.async && !strcmp(fake_rc.url, want),
                  "direct play: the file itself, opened without waiting: %s", fake_rc.url);
            CHECK(strstr(fake_rc.headers, "X-Plex-Token: SRV-TOKEN\r\n") && !strstr(fake_rc.headers, "Accept:"),
                  "with the token, and no Accept: JSON");
            CHECK(!(mask & 1), "null events while it opens");
            CHECK(strstr(player_test_time(), "Opening") != NULL, "the bar: %s", player_test_time());
            pc++;
            return ev_redraw(b, w_browser);
        case 906:
            CHECK(strstr(plotted_text, "Opening...") && strstr(plotted_text, "Big Buck Bunny"), "Opening, drawn: %s",
                  plotted_text);
            n_null = 0;
            pc++;
            continue;
        case 907:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            CHECK(player_ready() && fake_rc.seeks == 1 && NEAR(fake_rc.seek_to, 2530, 0.01),
                  "open, and carried on from 42:10 (%.2f)", fake_rc.seek_to);
            CHECK(log_count("/:/timeline", "state", "playing") >= 1 && log_count("/:/timeline", "time", "2530000") >= 1,
                  "the server told: playing, at 42:10");
            CHECK(log_count("/playQueues", NULL, NULL) >= 1 && log_count("/:/timeline", "playQueueItemID", "59265") >= 1 &&
                  log_count("/:/timeline", "containerKey", "/playQueues/3141") >= 1,
                  "played from a play queue, as the Plex apps (the dashboard's Now Playing)");
            CHECK(strstr(fake_rc.headers, "X-Plex-Session-Identifier: ") && strstr(fake_rc.headers, "X-Plex-Provides: player"),
                  "the stream asked for with the session id");
            n_null = 0;
            pc++;
            continue;
        case 908: {
            int bw, bh;
            static int plots0;
            if (n_null == 0) {
                profile_begin();
                plots0 = plots;
            }
            if (n_null++ < 30) {
                fake_cs += 4;
                return NULL_EVENT;
            }
            profile_end("playing 1.2 s (30 null events)");
            CHECK(plots - plots0 < 12, "playing: the time and the position bar redrawn, not the bar's buttons (%d icons)",
                  plots - plots0);
            pic_px(&bw, &bh);
            CHECK(ovl_created == 1 && ovl_sel_fourcc == 0x32315659 && ovl_banks == 3 && ovl_sel_flags == 0xE000,
                  "an overlay: YV12, 3 buffers, BT.709 video range (&%x)", ovl_sel_flags);
            CHECK(ovl_display >= 0 && fake_rc.yuv_draws > 0 && fake_rc.yuv_w == 1280 && fake_rc.yuv_h == 720 && !ovl_mapped,
                  "pictures copied into it and shown (%d)", fake_rc.yuv_draws);
            CHECK(ovl_win == w_browser && (ovl_scale[0] == bw || ovl_scale[1] == bh) &&
                  ovl_scale[0] <= bw && ovl_scale[1] <= bh && abs(ovl_scale[0] * 9 - ovl_scale[1] * 16) <= 16,
                  "scaled to fit the picture box %dx%d: %dx%d", bw, bh, ovl_scale[0], ovl_scale[1]);
            CHECK(ovl_pos[3] == -(win(w_browser)->vis[3] - win(w_browser)->vis[1]) + 168 && ovl_pos[5] == 0,
                  "clipped above the bar (%d)", ovl_pos[3]);
            CHECK(strstr(player_test_time(), "42:1") && strstr(player_test_time(), "/ 1:30:00"), "the time: %s",
                  player_test_time());
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 4, "asleep between pictures (PollIdle %d)", idle_time - fake_cs);
            fake_rc.ahead = 2;                                  /* the read-ahead runs short */
            count0 = player_test_feeds;
            pc = 9080;
            return NULL_EVENT;
        }
        case 9080:
            CHECK(last_poll == 0x400C7 && !(mask & 1) && player_test_feeds == count0 + 1,
                  "2 s read ahead: no sleeping, the time between pictures given to the reader (%x, %d)", last_poll,
                  player_test_feeds - count0);
            fake_rc.ahead = 7;
            pc++;
            return NULL_EVENT;
        case 9081:
            CHECK(last_poll == 0x400C7 && player_test_feeds == count0 + 2, "7 s: still filling, up to 9.5 s");
            fake_rc.ahead = 9.8;
            pc++;
            return NULL_EVENT;
        case 9082:
            CHECK(last_poll == 0x400E1 && player_test_feeds == count0 + 2, "9.8 s: asleep between pictures again");
            fake_rc.ahead = 0;
            for (int i = 0; i < FB_W * FB_H; i++)   /* what was there before: the details page */
                fb[i] = 0x123456;
            pc = 909;
            return ev_redraw(b, w_browser);
        case 909:
            {   /* the picture box outside the overlay (above and below a 16:9 picture) */
                win_t *x = win(w_browser);
                CHECK(fb_at(x->vis[0] + 20, x->vis[3] - 6) == 0 && fb_at(x->vis[2] - 20, x->vis[1] + 168 + 6) == 0 &&
                      fb_at((x->vis[0] + x->vis[2]) / 2, x->vis[3] - 6) == 0,
                      "the picture's box blacked out under the overlay (no page left showing): %06x",
                      fb_at(x->vis[0] + 20, x->vis[3] - 6));
            }
            CHECK(ovl_redraws > 0 && strstr(plotted_text, "Big Buck Bunny") && strstr(plotted_text, "-10 s") &&
                  strstr(plotted_text, "Stats") && strstr(plotted_text, "Subtitles|"),
                  "the redraw: the overlay's part, and the bar: %s", plotted_text);
            count0 = log_count("/:/timeline", "state", "paused");
            pc++;
            return ev_key(b, w_browser, -1, ' ');
        case 910:
            CHECK(player_paused() && ovl_display == -1, "Space: paused, the overlay hidden");
            CHECK(log_count("/:/timeline", "state", "paused") == count0 + 1, "the server told: paused");
            CHECK(mask & 1, "no null events while paused");
            pc++;
            return ev_redraw(b, w_browser);
        case 911:
            CHECK(!plot_52_bad, "the picture plotted 1:1, by pointer, no table");
            CHECK(sprite_plots_52 > 0 && fake_rc.draws > 0 && fake_rc.draw_w == (win(w_browser)->vis[2] - win(w_browser)->vis[0]) >> 1,
                  "paused: the picture plotted as a sprite (%d wide)", fake_rc.draw_w);
            save_picture("player.ppm", w_browser);
            pc++;
            return ev_key(b, w_browser, -1, ' ');
        case 912:
            CHECK(!player_paused(), "Space: playing");
            fake_rc.buffering = 1;                                  /* the network runs short */
            fake_cs += 4;
            pc = 9120;
            return NULL_EVENT;
        case 9120:
            CHECK(last_poll == 0x400C7 && !(mask & 1), "buffering: no sleeping, so the reader gets the time");
            CHECK(strstr(player_test_time(), "Buffering") != NULL, "the bar says so: %s", player_test_time());
            fake_rc.buffering = 0;
            n_null = 0;
            pc = 913;
            return ev_key(b, w_browser, -1, 'S');
        case 913: {
            int found = 0;
            if (n_null++ < 5) {
                fake_cs += 30;
                return NULL_EVENT;
            }
            for (int i = 0; i < player_test_panel_rows(); i++)
                if (!strcmp(player_test_panel(i, 0), "Connection") && strstr(player_test_panel(i, 1), "http, reading ahead"))
                    found = 1;
            CHECK(fake_rc.panel_rows >= 8 && found, "S: the stats panel, with the connection (%d rows)", fake_rc.panel_rows);
            CHECK(!strcmp(player_test_panel(0, 0), "Video / Source") && strstr(player_test_panel(0, 1), " / Direct Play"),
                  "Source: Direct Play (%s)", player_test_panel(0, 1));
            p0 = player_position();
            pc++;
            return ev_key(b, w_browser, -1, 0x18D);                 /* Right: 10 s on */
        }
        case 914:
            CHECK(fake_rc.seeks == 2 && NEAR(fake_rc.seek_to, p0 + 10, 0.1), "Right: 10 s on (%.2f)", fake_rc.seek_to);
            pc++;
            return player_button(b, w_browser, PB_FULL);
        case 915:
            wfull = full_win();
            CHECK(wfull && win(wfull)->open && win(wfull)->vis[0] == 0 && win(wfull)->vis[2] == 3840 &&
                  win(wfull)->vis[3] == 2160, "full screen: a window over the whole screen");
            CHECK(player_fullscreen() && win(w_browser)->open, "and the browser stays");
            pointer_w = wfull;                                      /* the pointer over the picture */
            n_null = 0;
            pc++;
            continue;
        case 916:
            if (n_null++ < 5) {
                fake_cs += 50;                                      /* 2.5 s, the pointer still */
                return NULL_EVENT;
            }
            CHECK(pointer_off, "full screen: the pointer hidden once it's been still 2 s");
            CHECK(NEAR(fake_rc.yuv_scale, 1280.0 / 1920, 0.01), "the stats drawn to be seen at their size (%.3f)",
                  fake_rc.yuv_scale);
            CHECK(ovl_win == wfull && ovl_scale[0] == 1920 && ovl_scale[1] == 1080 && ovl_pos[3] == -2160,
                  "the overlay stretched to the screen: %dx%d", ovl_scale[0], ovl_scale[1]);
            pointer_x += 50;                                        /* the pointer moves: the bar */
            n_null = 0;
            pc++;
            continue;
        case 917:
            if (n_null++ < 3) {
                fake_cs += 4;
                return NULL_EVENT;
            }
            CHECK(ovl_pos[3] == -2160 + 168, "the bar shows: the overlay stops above it (%d)", ovl_pos[3]);
            CHECK(!pointer_off, "and the pointer is back");
            pc++;
            return ev_key(b, wfull, -1, 0x1B);
        case 918:
            CHECK(!player_fullscreen() && !win(wfull)->open && ui_test_page() == PG_PLAYER, "Escape: back in the window");
            n_null = 0;
            pc++;
            continue;
        case 919:
            if (n_null++ < 3) {
                fake_cs += 4;
                return NULL_EVENT;
            }
            CHECK(ovl_win == w_browser, "the overlay back in the window");
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 920:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MP_AUDIO), "Sound track") && !menu_shaded(menu_open, MP_AUDIO) &&
                  (menu_flags(menu_open, MP_OVERLAY) & 1) && !strcmp(menu_text(menu_open, MP_STOP), "Stop"),
                  "Menu over the picture: the player's menu");
            pc++;
            return ev_menu(b, MP_AUDIO, 1);
        case 921: {
            win_t *x = win(w_browser);
            CHECK(fake_rc.track == 1, "the file's second sound track");
            memset(b, 0, 32);                                       /* taller: not 16:9 any more */
            b[0] = w_browser; b[1] = x->vis[0]; b[2] = x->vis[1] - 300; b[3] = x->vis[2]; b[4] = x->vis[3];
            b[6] = -300;                                            /* and scrolled (the scroll bar dragged) */
            b[7] = -1;
            pc = 9210;
            return 2;                                               /* Open_Window_Request */
        }
        case 9210:
            CHECK(win(w_browser)->sy == 0 && win(w_browser)->sx == 0, "the player page doesn't scroll (%d)",
                  win(w_browser)->sy);
            pc = 9211;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        /* ---- subtitles, playing the file itself: reelcore draws them */
        case 9211:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MP_SUBS), "Subtitles") && !menu_shaded(menu_open, MP_SUBS) &&
                  menu_sub(menu_open, MP_SUBS) > 0x10000, "the player's menu: Subtitles");
            open0 = fake_rc.opens;
            pc++;
            return ev_menu(b, MP_SUBS, 1);                          /* English (SRT), in the file */
        case 9212:
            CHECK(log_count("/library/parts/11101", "subtitleStreamID", "1001") == 1, "chosen on the server");
            CHECK(fake_rc.sub_track == 0 && fake_rc.opens == open0, "the file's first subtitle track, shown by the player");
            CHECK(strstr(player_test_time(), "Subtitles: English (SRT)"), "the bar says: %s", player_test_time());
            pc++;
            return player_button(b, w_browser, PB_SUBS);
        case 9213:
            CHECK(menu_open && !strcmp(menu_text(menu_open, 0), "None") && (menu_flags(menu_open, 1) & 1) &&
                  !strcmp(menu_text(menu_open, 2), "English (SRT External)"), "Subtitles on the bar: the menu");
            pc++;
            return ev_menu(b, 2, -1);                               /* the file beside it */
        case 9214:
            CHECK(fake_rc.sub_files == 1 && strstr(fake_rc.sub_text, "A big buck") && fake_rc.sub_track == 2 &&
                  fake_rc.opens == open0, "the file beside it fetched and given to the player (%s)", fake_rc.sub_file);
            pc++;
            return player_button(b, w_browser, PB_SUBS);
        case 9215:
            pc++;
            return ev_menu(b, 3, -1);                               /* French Forced (PGS): pictures */
        case 9216:
            CHECK(fake_rc.sub_track == 1 && fake_rc.opens == open0, "picture subtitles: the file's second track");
            pc++;
            return player_button(b, w_browser, PB_SUBS);
        case 9217:
            pc++;
            return ev_menu(b, 2, -1);                               /* the file beside it again */
        case 9218:
            CHECK(fake_rc.sub_files == 1 && fake_rc.sub_track == 2, "not fetched twice");
            pc = 92181;
            return ev_key(b, w_browser, -1, 8);                     /* Back, and Play again: */
        case 92181:
            CHECK(ui_test_page() == PG_DETAILS && !ui_test_player(), "back to the details");
            CHECK(player_test_page_moves() == 0, "the page at &8000 stayed put (open, overlay, close)");
            fake_rc.move_page_on_open = 1;                      /* as a decoder taking it for its memory would */
            open0 = fake_rc.opens;
            n_null = 0;
            pc++;
            return ev_button(b, w_browser, D_PLAY, 0x400);
        case 92182:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            CHECK(player_test_page_moves() == 1, "a decoder moving the page at &8000: noticed (and logged)");
            fake_rc.move_page_on_open = 0;
            snprintf(want, sizeof(want), "%s/library/parts/11/101/file.mp4", base);
            CHECK(fake_rc.opens == open0 + 1 && !strcmp(fake_rc.url, want) && fake_rc.sub_track == 2 &&
                  fake_rc.sub_files == 2, "subtitles chosen: still the file itself, the player showing them (%s)",
                  fake_rc.url);
            open0 = fake_rc.opens;
            pc = 9219;
            return player_button(b, w_browser, PB_SUBS);
        case 9219:
            pc = 9220;
            return ev_menu(b, 0, -1);                               /* None */
        case 9220:
            CHECK(fake_rc.sub_track == -1 && fake_rc.opens == open0 &&
                  log_count("/library/parts/11101", "subtitleStreamID", "0") >= 1, "None: off, on the server too");
            CHECK(strstr(player_test_time(), "Subtitles off"), "the bar says: %s", player_test_time());
            pc = 9221;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        /* ---- chapters (Plex's, from includeChapters=1) */
        case 9221: {
            const int *cm = menu_open ? (const int *)(intptr_t)menu_sub(menu_open, MP_CHAPTERS) : NULL;
            CHECK(menu_open && !strcmp(menu_text(menu_open, MP_CHAPTERS), "Chapters") &&
                  !menu_shaded(menu_open, MP_CHAPTERS) && cm, "the player's menu: Chapters");
            CHECK(cm && !strcmp(menu_text(cm, 0), "0:00  Opening") && !strcmp(menu_text(cm, 1), "0:05  The meadow") &&
                  !strcmp(menu_text(cm, 2), "0:10  Chapter 3") &&
                  (menu_flags(cm, player_position() < 5 ? 0 : player_position() < 10 ? 1 : player_position() < 15 ? 2 : 3) & 1),
                  "the chapters, the one playing ticked (%s; at %.1f s)", cm ? menu_text(cm, 1) : "", player_position());
            seeks0 = fake_rc.seeks;
            pc++;
            return ev_menu(b, MP_CHAPTERS, 2);
        }
        case 9222:
            CHECK(fake_rc.seeks == seeks0 + 1 && fake_rc.seek_to == 10, "chapter 3: to 0:10 (%.1f)", fake_rc.seek_to);
            pc++;
            return ev_key(b, w_browser, -1, 0x19E);                 /* Page Down */
        case 9223:
            CHECK(fake_rc.seek_to == 15, "Page Down: the next chapter (%.1f)", fake_rc.seek_to);
            pc++;
            return ev_key(b, w_browser, -1, 0x19F);                 /* Page Up, just after its start */
        case 9224:
            CHECK(fake_rc.seek_to == 10, "Page Up: the chapter before (%.1f)", fake_rc.seek_to);
            n_null = 0;
            pc = 9230;
            return ev_key(b, w_browser, -1, 'M');
        /* ---- the mini player */
        case 9230: {
            win_t *m = win(mini_win());
            wmini = mini_win();
            CHECK(player_mini() && m && m->open && !win(w_browser)->open, "M: the mini player, in place of the window");
            CHECK(m && m->vis[2] == 3840 - 32 && m->vis[1] == 134 + 16 && m->vis[2] - m->vis[0] == 640 &&
                  m->vis[3] - m->vis[1] == 360 + 72, "bottom right, above the icon bar, 640 x 360 + the controls (%d,%d-%d,%d)",
                  m ? m->vis[0] : 0, m ? m->vis[1] : 0, m ? m->vis[2] : 0, m ? m->vis[3] : 0);
            CHECK(fake_rc.fast == 2 /* REELCORE_FAST_LIGHT */, "decoding a little less (%d)", fake_rc.fast);
            if (n_null++ < 3) {
                fake_cs += 4;
                return NULL_EVENT;
            }
            CHECK(ovl_win == wmini && ovl_scale[0] == 320 && ovl_scale[1] == 180, "the overlay in it, %dx%d",
                  ovl_scale[0], ovl_scale[1]);
            pc++;
            return player_button(b, wmini, PB_GRIP);
        }
        case 9231: {
            CHECK(drag_win == wmini && drag_type == 2, "the grip: the Wimp resizes it (%d)", drag_type);
            memset(b, 0, 32);                                   /* made 800 wide by it, top left put */
            b[0] = wmini; b[1] = 3840 - 32 - 640; b[2] = 134 + 16 - 100; b[3] = b[1] + 800; b[4] = 134 + 16 + 432; b[7] = -1;
            pc++;
            return 2;
        }
        case 9232: {
            win_t *m = win(wmini);
            CHECK(m->vis[2] - m->vis[0] == 800 && m->vis[3] - m->vis[1] == 450 + 72 && m->vis[3] == 134 + 16 + 432,
                  "the video's shape kept, the top put (%d x %d)", m->vis[2] - m->vis[0], m->vis[3] - m->vis[1]);
            pc++;
            return player_button(b, wmini, PB_NORMAL);
        }
        case 9233:
            CHECK(!player_mini() && win(w_browser)->open && !win(wmini)->open && win(w_browser)->vis[2] - win(w_browser)->vis[0] == 2840,
                  "Normal: the window again, as it was");
            CHECK(fake_rc.fast == 0, "decoding as before");
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 9234:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MP_MINI), "Mini player") &&
                  !strcmp(menu_text(menu_open, MP_ONTOP), "Keep on top") && !(menu_flags(menu_open, MP_ONTOP) & 1),
                  "the player's menu: Mini player, Keep on top");
            pc++;
            return ev_menu(b, MP_ONTOP, -1);
        case 9235:
            CHECK(player_ontop() && strstr(read_file(choices), "keep_on_top 1"), "Keep on top, kept in Choices");
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 9236:
            pc++;
            return ev_menu(b, MP_MINI, -1);
        case 9237: {
            win_t *m = win(wmini);
            CHECK(player_mini() && m->open && m->vis[2] - m->vis[0] == 800, "from the menu, at the size it was given");
            CHECK(strstr(read_file(choices), "mini_width 800"), "its size kept in Choices");
            pc++;
            return ev_key(b, wmini, -1, 0x1B);
        }
        case 9238:
            CHECK(!player_mini() && win(w_browser)->open, "Escape: the window again");
            pc = 922;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 922:
            n_null = 0;
            pc++;
            return ev_menu(b, MP_PICTURE, PIC_STRETCH);
        case 923: {
            int bw, bh;
            if (n_null++ < 3) {
                fake_cs += 4;
                return NULL_EVENT;
            }
            pic_px(&bw, &bh);
            CHECK(strstr(read_file(choices), "picture 2\n") && ovl_scale[0] == bw && ovl_scale[1] == bh,
                  "Stretch: the overlay fills the box %dx%d (%dx%d)", bw, bh, ovl_scale[0], ovl_scale[1]);
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        }
        case 924:
            n_null = 0;
            pc++;
            return ev_menu(b, MP_OVERLAY, -1);
        case 925:
            if (n_null == 0) {
                CHECK(!ovl_id && strstr(read_file(choices), "hardware_overlay 0\n"), "Hardware overlay off: gone");
                draws0 = fake_rc.draws;
                count0 = updates;
            }
            if (n_null++ < 5) {
                fake_cs += 4;
                return NULL_EVENT;
            }
            CHECK(fake_rc.draws > draws0 + 3 && updates > count0 + 3 && fake_rc.draw_flags == 1,
                  "each picture converted and plotted (stretched)");
            count0 = log_count("/:/timeline", "state", "stopped");
            pc++;
            return player_button(b, w_browser, PB_BACK);
        case 926:
            CHECK(ui_test_page() == PG_DETAILS && !ui_test_player() && fake_rc.open_now == 0,
                  "Back: the details again, the video closed");
            CHECK(log_count("/:/timeline", "state", "stopped") == count0 + 1 &&
                  log_count("/video/:/transcode/universal/stop", NULL, NULL) == 0, "the server told: stopped");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 927:
            pc++;
            return ev_menu(b, MB_DIRECT, -1);                       /* Direct play off: converted */
        case 928:
            open0 = fake_rc.opens;
            fake_rc.clock_start = 11;                               /* the stream's timestamps start at 11 s */
            pc++;
            return ev_button(b, w_browser, D_START, 0x400);
        case 929:
            CHECK(fake_rc.opens == open0 + 1 && strstr(fake_rc.url, "/video/:/transcode/universal/start.m3u8?") &&
                  strstr(fake_rc.url, "&offset=0&"), "converted by the server, from the start");
            snprintf(sid0, sizeof(sid0), "%s", strstr(fake_rc.url, "&session=") ? strstr(fake_rc.url, "&session=") + 9 : "");
            sid0[strcspn(sid0, "&")] = 0;
            n_null = 0;
            pc = 930;
            continue;
        case 930:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            n_null = 0;
            pc++;
            continue;
        case 931:
            if (n_null++ < 10) {
                fake_cs += 11;
                return NULL_EVENT;
            }
            CHECK(strstr(player_test_time(), "0:01 / 1:30:00"), "the time, with Plex's length: %s (%d frames, pos %.2f)",
                  player_test_time(), fake_rc.frames, player_position());
            open0 = fake_rc.opens;
            count0 = log_count("/video/:/transcode/universal/stop", NULL, NULL);
            draws0 = fake_rc.seeks;
            n_null = 0;
            pc++;
            return ev_key(b, w_browser, -1, 0x18D);                 /* Right */
        case 932:
            if (n_null++ < 3) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            CHECK(fake_rc.opens == open0 && fake_rc.seeks == draws0 + 1 && NEAR(fake_rc.seek_to, 11.1, 0.2) &&
                  log_count("/video/:/transcode/universal/stop", NULL, NULL) == count0,
                  "a seek in a converted stream: in the stream (the server converts from there), no new one (%.2f)",
                  fake_rc.seek_to);
            n_null = 0;
            pc++;
            continue;
        case 933:
            fake_cs += 50;
            pc++;
            return NULL_EVENT;
        case 934: {
            int ok = 0;
            if (n_null++ < 4) {
                fake_cs += 50;
                return NULL_EVENT;
            }
            for (int i = 0; i < player_test_panel_rows(); i++)
                if (!strcmp(player_test_panel(i, 0), "Timing") && strstr(player_test_panel(i, 1), "sync +0 ms"))
                    ok = 1;
            CHECK(ok, "sync against the first picture, not the stream's start (11 s off)");
            ok = 0;
            for (int i = 0; i < player_test_panel_rows(); i++)
                ok |= (!strcmp(player_test_panel(i, 0), "Network Activity")) | (!strcmp(player_test_panel(i, 0), "Date")) << 1 |
                      (!strcmp(player_test_panel(i, 0), "Codecs")) << 2;
            CHECK(ok == 7, "ReelEGL's rows: Network Activity, Codecs, Date");
            ok = 0;
            for (int i = 0; i < player_test_panel_rows(); i++)
                if (!strcmp(player_test_panel(i, 0), "Decoder") && !strcmp(player_test_panel(i, 1), "VideoCore (hardware)"))
                    ok = 1;
            CHECK(ok, "Decoder: the VideoCore (h264_vchiq), from reelcore's stats");
            CHECK(strstr(player_test_panel(0, 1), " / Transcoded"), "Source: Transcoded (%s)", player_test_panel(0, 1));
            CHECK(strstr(player_test_time(), "0:13"), "the time: %s", player_test_time());
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        }
        case 935:
            CHECK(menu_open && !menu_shaded(menu_open, MP_AUDIO), "two sound tracks on the server");
            open0 = fake_rc.opens;
            count0 = log_count("/video/:/transcode/universal/stop", NULL, NULL);
            n_null = 0;
            pc++;
            return ev_menu(b, MP_AUDIO, 1);
        case 936:
            if (fake_rc.opens == open0 && n_null++ < 10)
                return NULL_EVENT;
            CHECK(log_count("/library/parts/11101", "audioStreamID", "1013") == 1, "chosen on the server");
            CHECK(strstr(fake_rc.url, "&offset=0&") && !strstr(fake_rc.url, sid0) &&
                  log_count("/video/:/transcode/universal/stop", "session", sid0) == 1,
                  "a new conversion for the sound track, the one before stopped");
            n_null = 0;
            pc = 9360;
            continue;
        case 9360:                                                  /* it carries on from where it was */
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            CHECK(fake_rc.seek_to > 12.5 && fake_rc.seek_to < 14.5, "and seeks to where it was (%.2f)", fake_rc.seek_to);
            pc = 9361;
            return ev_key(b, w_browser, -1, ' ');                   /* paused */
        case 9361:
            CHECK(player_paused() && last_poll == 0x400E1 && idle_time - fake_cs > 2000 && idle_time - fake_cs <= 3000,
                  "paused, converted: woken now and then (%d cs)", idle_time - fake_cs);
            count0 = log_count("/video/:/transcode/universal/ping", NULL, NULL);
            fake_cs = idle_time;
            pc = 9362;
            return NULL_EVENT;
        case 9362:
            CHECK(log_count("/video/:/transcode/universal/ping", NULL, NULL) == count0 + 1,
                  "the server told the conversion is still wanted");
            pc = 9363;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        /* ---- subtitles, converted: the server burns them in, from here */
        case 9363:
            open0 = fake_rc.opens;
            snprintf(sid0, sizeof(sid0), "%s", strstr(fake_rc.url, "&session=") + 9);
            *strchr(sid0, '&') = 0;
            n_null = 0;
            pc++;
            return ev_menu(b, MP_SUBS, 1);
        case 9364:
            if (fake_rc.opens == open0 && n_null++ < 10)
                return NULL_EVENT;
            CHECK(fake_rc.opens == open0 + 1 && log_count("/video/:/transcode/universal/stop", "session", sid0) == 1 &&
                  !strstr(fake_rc.url, sid0) && fake_rc.sub_track == -1,
                  "converted: a new conversion with them burnt in, not drawn by the player");
            n_null = 0;
            pc++;
            continue;
        case 9365:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            open0 = fake_rc.opens;
            n_null = 0;
            pc++;
            return player_button(b, w_browser, PB_SUBS);
        case 9366:
            pc++;
            return ev_menu(b, 0, -1);                               /* None again */
        case 9367:
            if (fake_rc.opens == open0 && n_null++ < 10)
                return NULL_EVENT;
            CHECK(fake_rc.opens == open0 + 1, "None: converted again, without");
            n_null = 0;
            pc++;
            continue;
        case 9368:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            pc = 937;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 937:
            count0 = log_count("/video/:/transcode/universal/stop", NULL, NULL);
            fake_rc.clock_start = 0;
            pc++;
            return ev_menu(b, MP_STOP, -1);
        case 938:
            CHECK(ui_test_page() == PG_DETAILS && !ui_test_player() &&
                  log_count("/video/:/transcode/universal/stop", NULL, NULL) == count0 + 1, "Stop: the conversion stopped too");
            CHECK(fake_rc.sub_file[0] && !file_exists(fake_rc.sub_file), "the subtitle file fetched is gone");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 939:
            pc++;
            return ev_menu(b, MB_DIRECT, -1);                       /* Direct play on again */
        /* episodes: the next one at the end */
        case 940:
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);                  /* the grid */
        case 941:
            pc++;
            return ev_key(b, w_browser, -1, 8);                     /* the top */
        case 942:
            pc++;
            return ev_button(b, w_browser, TB_TAB + 2, 0x400);
        case 943:
            pc = 945;
            return ev_tile(b, find_tile("Space Show"), 4);      /* its page: Series 1's episodes */
        case 945:
            CHECK(find_tile("5. Episode '5'") == 4, "the episodes");
            pc++;
            return ev_tile(b, 4, 4);
        case 946:
            fake_rc.len = 20;
            open0 = fake_rc.opens;
            pc++;
            return ev_button(b, w_browser, D_START, 0x400);
        case 947:
            snprintf(want, sizeof(want), "%s/library/parts/11/215/file.mp4", base);
            CHECK(!strcmp(fake_rc.url, want), "episode 5: %s", fake_rc.url);
            n_null = 0;
            pc = 9471;
            continue;
        /* ---- Skip intro (Plex's markers, from includeMarkers=1) */
        case 9471:
            if (!*player_skip_label() && n_null++ < 20) {
                fake_cs += 50;
                return NULL_EVENT;
            }
            CHECK(!strcmp(player_skip_label(), "Skip intro") && player_position() >= 2 && player_position() < 8,
                  "the intro: Skip intro (at %.1f s)", player_position());
            pc++;
            return ev_redraw(b, w_browser);
        case 9472:
            CHECK(strstr(plotted_text, "Skip intro") != NULL, "on the bar: %s", plotted_text);
            save_picture("skip.ppm", w_browser);
            seeks0 = fake_rc.seeks;
            pc++;
            return ev_key(b, w_browser, -1, 13);                    /* Return */
        case 9473:
            CHECK(fake_rc.seeks == seeks0 + 1 && fake_rc.seek_to == 8 && !*player_skip_label(),
                  "Return: to the intro's end (%.1f)", fake_rc.seek_to);
            n_null = 0;
            pc = 948;
            continue;
        case 948:
            if (!ui_test_upnext() && n_null++ < 80) {
                fake_cs += 50;
                return NULL_EVENT;
            }
            CHECK(ui_test_upnext() && ui_test_player() && player_ended(), "the end: Up next");
            CHECK(log_count("/:/scrobble", "key", "215") == 1 && log_count("/library/metadata/20/allLeaves", NULL, NULL) == 1,
                  "watched, and the next found");
            CHECK(!(mask & 1) || last_poll == 0x400E1, "the countdown needs time");
            pc++;
            return ev_redraw(b, w_browser);
        case 949:
            CHECK(strstr(plotted_text, "Up next") && strstr(plotted_text, "S1 E6") && strstr(plotted_text, "Playing in"),
                  "the card: %s", plotted_text);
            save_picture("upnext.ppm", w_browser);
            open0 = fake_rc.opens;
            pc++;
            return ev_key(b, w_browser, -1, 13);                    /* Return: play it now */
        case 950:
            snprintf(want, sizeof(want), "%s/library/parts/11/216/file.mp4", base);
            CHECK(fake_rc.opens == open0 + 1 && !strcmp(fake_rc.url, want) && !ui_test_upnext() &&
                  ui_test_page() == PG_PLAYER, "episode 6");
            n_null = 0;
            pc++;
            continue;
        case 951:
            if (strcmp(player_skip_label(), "Skip credits") && ui_test_player() && n_null++ < 80) {
                fake_cs += 50;
                return NULL_EVENT;
            }
            CHECK(!strcmp(player_skip_label(), "Skip credits") && player_position() >= 14, "the credits: Skip credits (%.1f s)",
                  player_position());
            pc = 9511;
            return player_button(b, w_browser, PB_SKIP);
        case 9511:
            CHECK(!ui_test_player() && ui_test_page() == PG_DETAILS && log_count("/:/scrobble", "key", "216") == 1,
                  "Skip credits that run to the end: watched; the last episode, so back to the details");
            pc = 9512;
            continue;
        case 9512:
            if (ui_test_player() && n_null++ < 80) {
                fake_cs += 50;
                return NULL_EVENT;
            }
            CHECK(!ui_test_player() && ui_test_page() == PG_DETAILS && log_count("/:/scrobble", "key", "216") == 1,
                  "the last episode: watched, and back to the details");
            pc = 9520;
            return ev_key(b, w_browser, -1, 0x1B);
        /* ---- Plex Home: who's watching */
        case 9520: {
            int nu = 0;
            CHECK(!strcmp(ui_test_user(&nu), "Andrew") && nu == 2 && log_count("/api/v2/home/users", NULL, NULL) >= 1,
                  "Plex Home: two people; watching as the owner, Andrew (%s, %d)", ui_test_user(&nu), nu);
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 9521: {
            const int *um = menu_open ? (const int *)(intptr_t)menu_sub(menu_open, MB_USERS) : NULL;
            CHECK(menu_open && !strcmp(menu_text(menu_open, MB_USERS), "Switch user") && !menu_shaded(menu_open, MB_USERS) && um,
                  "the icon bar menu: Switch user");
            CHECK(um && !strcmp(menu_text(um, 0), "Andrew") && (menu_flags(um, 0) & 1) && !strcmp(menu_text(um, 1), "Kids") &&
                  menu_sub(um, 1) == -1, "Andrew ticked; Kids (no PIN)");
            count0 = log_count("/api/v2/home/users/u-kids/switch", NULL, NULL);
            pc++;
            return ev_menu3(b, MB_USERS, 1, -1);
        }
        case 9522: {
            int nu = 0;
            CHECK(log_count("/api/v2/home/users/u-kids/switch", NULL, NULL) == count0 + 1 && !strcmp(ui_test_user(&nu), "Kids"),
                  "switched to Kids (%s)", ui_test_user(&nu));
            CHECK(strstr(read_file(choices), "account_token KID-ACCT\n") && strstr(read_file(choices), "server_token KID-SRV\n") &&
                  strstr(read_file(choices), "user_title Kids\n"), "their account token, and the server's token for them, kept");
            int hr = 0, hp = 0;
            CHECK(ui_test_home(&hr, &hp) && strstr(ui_test_status(), "Kids"), "the home page again, as Kids: %s", ui_test_status());
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 9523: case 9525: {
            const int *um = menu_open ? (const int *)(intptr_t)menu_sub(menu_open, MB_USERS) : NULL;
            const int *pm = um ? (const int *)(intptr_t)menu_sub(um, 0) : NULL;
            CHECK(um && !strcmp(menu_text(um, 0), "Andrew (PIN)") && pm && (menu_flags(pm, 0) & 4) && (menu_flags(um, 1) & 1),
                  "Kids ticked; Andrew has a PIN: a writable item to type it in");
            if (pm)
                strcpy((char *)menu_text(pm, 0), pc == 9523 ? "1111" : "1234\r");
            prev_reports = reports;
            pc++;
            return ev_menu3(b, MB_USERS, 0, 0);
        }
        case 9524: {
            int nu = 0;
            CHECK(reports == prev_reports + 1 && strstr(last_report, "isn't Andrew's PIN") && !strcmp(ui_test_user(&nu), "Kids"),
                  "a wrong PIN: said so, still Kids (%s)", last_report);
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 9526: {
            int nu = 0;
            CHECK(log_count("/api/v2/home/users/u-admin/switch", "pin", "1234") == 1 && !strcmp(ui_test_user(&nu), "Andrew") &&
                  strstr(read_file(choices), "server_token SRV-TOKEN\n"), "the right PIN: Andrew again, with his tokens");
            pc = 952;
            continue;
        }
        case 952:
            CHECK(ui_test_page() == PG_GRID, "the grid");
            items0 = ui_test_items();
            pc = 960;
            return ev_key(b, w_browser, -1, '/');
        /* ---- search */
        case 960:
            CHECK(ui_test_page() == PG_GRID && ui_test_items() == 0 && !strcmp(ui_test_query(), "") &&
                  !strcmp(ui_test_path() + strlen(ui_test_path()) - 6, "Search"), "/: the search page (%s)", ui_test_path());
            count0 = log_count("/hubs/search", NULL, NULL);
            snprintf(type_str, sizeof(type_str), "bun");
            type_i = 0;
            pc++;
            continue;
        case 961:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            CHECK(!strcmp(ui_test_query(), "bun") && log_count("/hubs/search", NULL, NULL) == count0,
                  "typed, and nothing asked yet");
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 40, "asleep until a moment after the last key");
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 962:
            CHECK(log_count("/hubs/search", NULL, NULL) == count0 + 1 && log_count("/hubs/search", "query", "bun") == 1 &&
                  ui_test_items() == 1 && !strcmp(ui_test_item(0, 0), "Big Buck Bunny") &&
                  !strcmp(ui_test_item(0, 1), "Film \xb7 2008"), "one search, a moment later: %d found", ui_test_items());
            pc++;
            return ev_redraw(b, w_browser);
        case 963:
            CHECK(strstr(plotted_text, "bun|") != NULL, "the field: %s", plotted_text);
            pc++;
            return ev_key(b, w_browser, -1, 21);                /* Ctrl-U */
        case 964:
            snprintf(type_str, sizeof(type_str), "space");
            type_i = 0;
            pc++;
            continue;
        case 965:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 966:
            CHECK(ui_test_items() == 7 && !strcmp(ui_test_item(0, 0), "Space Show") &&
                  strstr(ui_test_item(1, 1), "S1 E1"), "the show first, then its episodes");
            DRAIN(10);
            if (pc == 967)
                return ev_redraw(b, w_browser);
            continue;
        case 967:
            save_picture("search.ppm", w_browser);
            pc++;
            return ev_tile(b, 0, 4);                            /* the show */
        case 968:
            CHECK(!strcmp(ui_test_query(), "") && ui_test_show(&count0), "the show opens");
            pc = 9680;
            return ev_key(b, w_browser, -1, '/');               /* another search from there */
        case 9680:
            pc = 9681;
            return ev_key(b, w_browser, -1, 'x');
        case 9681:
            CHECK(!strcmp(ui_test_query(), "x"), "typed in the second search");
            pc = 9682;
            return ev_key(b, w_browser, -1, 0x1B);              /* Back: the show */
        case 9682:
            pc = 969;
            return ev_key(b, w_browser, -1, 0x1B);              /* Back: the first search */
        case 969:
            CHECK(!strcmp(ui_test_query(), "space") && ui_test_items() == 7, "Back: the search, as it was");
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);
        case 970:
            CHECK(!strcmp(ui_test_query(), "") && ui_test_items() == items0 && ui_test_page() == PG_GRID,
                  "Escape: out of the search, the list before it");
            pc = 78;
            continue;
        /* ---- sign out, then a server typed by hand */
        case 78:
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 79:
            report_answer = 2;                                  /* Cancel */
            prev_reports = reports;
            pc = 790;
            return ev_menu(b, MB_SIGNOUT, -1);
        case 790:
            CHECK(reports == prev_reports + 1 && strstr(last_report, "Sign out?") && win(w_browser)->open &&
                  strstr(read_file(choices), "server_token SRV-TOKEN\n"), "Sign out asks first; Cancel keeps you signed in");
            report_answer = 1;
            pc = 791;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 791:
            pc = 80;
            return ev_menu(b, MB_SIGNOUT, -1);
        case 80:
            CHECK(!win(w_browser)->open, "signed out: the browser closed");
            CHECK(strstr(read_file(choices), "account_token \n") && strstr(read_file(choices), "server_token \n"),
                  "and the tokens forgotten:\n%s", read_file(choices));
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 81:
            CHECK(ui_test_signin_mode() == SI_PICK && win(w_browser)->open, "Select: Add a server again");
            pc = 8100;
            return ev_button(b, w_browser, S_PICK_PLEX, 0x400);
        case 8100:
            CHECK(ui_test_signin_mode() == SI_PLEX, "Plex's sign-in");
            snprintf(type_str, sizeof(type_str), "127.0.0.1:%sx", base + 17);   /* x: a slip, deleted */
            type_i = 0;
            pc = 810;
            return ev_button(b, w_browser, S_ADDR, 0x400);      /* the address field */
        case 810:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            pc = 811;
            return ev_key(b, w_browser, -1, 8);                 /* Backspace */
        case 811:
            snprintf(want, sizeof(want), "127.0.0.1:%s", base + 17);
            CHECK(ui_test_field() == 0 && !strcmp(ui_test_signin(2), want), "typed: %s", ui_test_signin(2));
            snprintf(type_str, sizeof(type_str), "WRONG");
            type_i = 0;
            pc = 812;
            return ev_key(b, w_browser, -1, 9);                 /* Tab: the token */
        case 812:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            CHECK(ui_test_field() == 1 && !strcmp(ui_test_signin(3), "WRONG"), "the token typed");
            pc = 82;
            return ev_button(b, w_browser, S_USE, 0x400);
        case 82:
            CHECK(ui_test_page() == PG_SIGNIN && strstr(ui_test_signin(1), "didn't take the token"),
                  "a wrong token: %s", ui_test_signin(1));
            snprintf(type_str, sizeof(type_str), "SRV-TOKEN");
            type_i = 0;
            pc = 820;
            return ev_key(b, w_browser, -1, 21);                /* Ctrl-U: empty it */
        case 820:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            CHECK(!strcmp(ui_test_signin(3), "SRV-TOKEN"), "the token again");
            pc = 83;
            return ev_key(b, w_browser, -1, 13);                /* Return in the token: Use these */
        case 83:
            CHECK(ui_test_page() == PG_GRID && win(w_browser)->open && ui_test_items() == 9, "by hand: the home page");
            snprintf(want, sizeof(want), "server_base %s\n", base);
            CHECK(strstr(read_file(choices), want) && strstr(read_file(choices), "server_token SRV-TOKEN\n") &&
                  strstr(read_file(choices), "account_token \n"), "Choices:\n%s", read_file(choices));
            pc++;
            return ev_msg(b, 0, 0, 0);                          /* Message_Quit */
        /* ---- the second run: Choices remembered */
        case 100:
            CHECK(!win(w_browser)->open, "second run: nothing open yet");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 101:
            CHECK(win(w_browser)->open && ui_test_items() == 9 && ui_test_page() == PG_GRID,
                  "Select: straight to the browser, from Choices");
            drain_n = 0;
            pc = 1010;
            continue;
        case 1010:                                          /* the posters: from the disc this time */
            DRAIN(20);
            if (pc == 1011)
                continue;
            return NULL_EVENT;
        case 1011:
            pc = 10110;
            return ev_redraw(b, w_browser);
        case 10110:
            save_picture("home-wide.ppm", w_browser);
            CHECK(ui_test_posters(NULL) >= 1 && log_count("/photo/:/transcode", "url", "/library/metadata/101/thumb/1700000000") == prev_count,
                  "the posters from the image cache: not fetched again (%d)",
                  log_count("/photo/:/transcode", "url", "/library/metadata/101/thumb/1700000000") - prev_count);
            pc = 10111;
            return ev_tile(b, 0, 0x100);                        /* Adjust (a PC mouse's right button) on Continue watching */
        case 10111:
            CHECK(menu_open && ui_test_sel() == 0 && !strcmp(menu_text(menu_open, MI_REMOVE), "Remove from Continue watching") &&
                  !menu_shaded(menu_open, MI_REMOVE) && !menu_shaded(menu_open, MI_WATCHED),
                  "Adjust: the poster's menu, with Remove from Continue watching");
            pc++;
            return ev_menu(b, MI_REMOVE, -1);
        case 10112: {
            int rows = 0, pick = 0;
            CHECK(log_count("/actions/removeFromContinueWatching", "ratingKey", "213") == 1, "the server told");
            CHECK(ui_test_home(&rows, &pick) && rows == 2 && ui_test_items() == 8 && strstr(ui_test_status(), "Continue watching"),
                  "gone from the home page (%d rows, %d items): %s", rows, ui_test_items(), ui_test_status());
            pc = 1012;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 1012:
            CHECK(menu_open && !strncmp(menu_text(menu_open, MB_CACHE), "Clear image cache (", 19) &&
                  !menu_shaded(menu_open, MB_CACHE), "Clear image cache, with its size: %s",
                  menu_open ? menu_text(menu_open, MB_CACHE) : "");
            pc++;
            return ev_menu(b, MB_CACHE, -1);
        case 1013: {
            char cmd[400];
            snprintf(cmd, sizeof(cmd), "test $(find %s/choices/Cache -type f | wc -l) = 0", outdir);
            CHECK(system(cmd) == 0 && strstr(ui_test_status(), "Image cache cleared: "), "cleared: %s", ui_test_status());
            pc = 102;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 102:
            CHECK(menu_open && menu_flags(menu_open, MB_QUIT) & 0x80, "Quit is the last item");
            CHECK(menu_open && menu_shaded(menu_open, MB_CACHE) && !strcmp(menu_text(menu_open, MB_CACHE),
                  "Clear image cache (0.0 MB)"), "and now empty: %s", menu_open ? menu_text(menu_open, MB_CACHE) : "");
            {
                const int *pm = (const int *)(intptr_t)menu_sub(menu_open, MB_PLAYER);
                CHECK(pm && (menu_flags(pm, PLAYER_BUILTIN) & 1) && !(menu_flags(pm, PLAYER_REELEGL) & 1),
                      "an old Choices' ReelEGL (no choices_version): the built-in player from now on");
            }
            pc++;
            return ev_menu(b, MB_QUIT, -1);
        /* ---- the third run: a Jellyfin server beside the Plex one */
        case 8000:
            CHECK(!win(w_browser)->open, "third run: nothing open yet");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);             /* Menu on the icon */
        case 8001: {
            const int *sm = menu_open ? (const int *)(intptr_t)menu_sub(menu_open, MB_SERVERS) : NULL;
            CHECK(sm && !menu_shaded(menu_open, MB_SERVERS) && !strcmp(menu_text(sm, 0), "Add a server...") &&
                  (menu_flags(sm, 0) & 0x80), "Servers, not signed in to plex.tv: Add a server... only");
            CHECK(!strcmp(menu_text(menu_open, MB_SIGNIN), "Add a server..."), "and on the icon's menu");
            pc++;
            return ev_menu(b, MB_SERVERS, 0);
        }
        case 8002:
            CHECK(ui_test_signin_mode() == SI_PICK, "Add a server: which kind");
            pc = 8040;
            return ev_button(b, w_browser, S_PICK_JF, 0x400);
        case 8040:
            CHECK(ui_test_page() == PG_SIGNIN && ui_test_signin_jf() && win(w_browser)->open && ui_test_field() == 0,
                  "the Jellyfin sign-in page, the caret in the address");
            snprintf(type_str, sizeof(type_str), "127.0.0.1:%s", base + 17);
            type_i = 0;
            pc = 8003;
            return ev_button(b, w_browser, S_JADDR, 0x400);
        case 8003:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            CHECK(!strcmp(ui_test_signin(5), type_str), "the address typed: %s", ui_test_signin(5));
            pc++;
            return ev_button(b, w_browser, S_QC, 0x400);        /* Quick Connect */
        case 8004:
            CHECK(!strcmp(ui_test_signin(4), "123456") && strstr(ui_test_signin(1), "Quick Connect"),
                  "a Quick Connect code: %s (%s)", ui_test_signin(4), ui_test_signin(1));
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 200, "checked every 2 s");
            pc++;
            return ev_redraw(b, w_browser);
        case 8005:
            CHECK(strstr(plotted_text, "Sign in to a Jellyfin server|") && strstr(plotted_text, "1  2  3  4  5  6|") &&
                  strstr(plotted_text, "New code|") && strstr(plotted_text, "Back|") &&
                  strstr(plotted_text, "Cancel|"), "drawn: %s", plotted_text);
            save_picture("signin-jellyfin.ppm", w_browser);
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 8006:
            CHECK(ui_test_page() == PG_SIGNIN, "not yet allowed");
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 8007: {
            int rows = 0, pick = 0, st = 0, n = 0, vis = 0, cur = -1;
            CHECK(ui_test_page() == PG_GRID && ui_test_home(&rows, &pick) && rows == 3, "allowed: Jellyfin's home page (%d rows)", rows);
            CHECK(ui_test_home_row(0, &st, &n, &vis) && !strcmp(ui_test_home_row(0, &st, &n, &vis), "Continue watching") && n == 2 &&
                  !strcmp(ui_test_item(0, 0), "Jelly Bunny") && !strcmp(ui_test_item(1, 0), "Moon Show") &&
                  !strncmp(ui_test_item(1, 1), "S1 E2", 5), "Continue watching: part watched, then next up (%s, %s)",
                  ui_test_item(0, 0), ui_test_item(1, 0));
            CHECK(!strcmp(ui_test_home_row(1, &st, &n, &vis), "Recently added in Jelly Films") && n == 2, "recently added");
            CHECK(!strcmp(ui_test_path(), "Cellar") && strstr(ui_test_status(), "Signed in to Cellar as andrew"),
                  "where: %s; %s", ui_test_path(), ui_test_status());
            CHECK(ui_test_tab(&cur) == 4 && cur == 0, "Home and three libraries");
            snprintf(want, sizeof(want), "jellyfin Cellar|JFID|%s|JF-TOKEN|u1|andrew\n", base);
            CHECK(strstr(read_file(choices), want) && strstr(read_file(choices), "server_kind 1\n") &&
                  strstr(read_file(choices), "server_user_id u1\n"), "kept in Choices:\n%s", read_file(choices));
            CHECK(log_count("/Items/jm1/Images/Primary", NULL, NULL) >= 0, "posters");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        }
        case 8008: {
            const int *sm = menu_open ? (const int *)(intptr_t)menu_sub(menu_open, MB_SERVERS) : NULL;
            CHECK(sm && !strcmp(menu_text(sm, 0), "Cellar (Jellyfin)") && (menu_flags(sm, 0) & 1) &&
                  !strcmp(menu_text(sm, 1), "Add a server..."), "Servers: Cellar, ticked");
            CHECK(menu_shaded(menu_open, MB_USERS) && !strcmp(menu_text(menu_open, MB_SIGNOUT), "Sign out of this server"),
                  "no Plex Home; Sign out of this server");
            pc++;
            return ev_button(b, w_browser, TB_TAB + 1, 0x400);  /* Jelly Films */
        }
        case 8009: {
            int view = -1, sort = -1, unw = -1;
            CHECK(ui_test_items() == 3 && find_tile("Jelly Bunny") >= 0 && ui_test_lib(&view, &sort, &unw),
                  "a Jellyfin library: %d films, its bar", ui_test_items());
            pc++;
            return ev_tile(b, find_tile("Jelly Bunny"), 4);
        }
        case 8010: {
            int x, y;
            CHECK(ui_test_page() == PG_DETAILS && strstr(ui_test_det(2), "Direct Play"), "its details: %s", ui_test_det(2));
            CHECK(ui_test_button_xy(w_browser, D_STAR, &x, &y) != 0, "no stars: Jellyfin keeps none");
            CHECK(log_count("/Items/jm1/PlaybackInfo", NULL, NULL) == 0, "how it plays: nothing asked of the server");
            fake_rc.len = 5400;
            open0 = fake_rc.opens;
            pc++;
            return ev_redraw(b, w_browser);
        }
        case 8011:
            save_picture("details-jellyfin.ppm", w_browser);
            pc++;
            return ev_button(b, w_browser, D_PLAY, 0x400);
        case 8012:
            snprintf(want, sizeof(want), "%s/Videos/jm1/stream?static=true&mediaSourceId=ms-jm1&PlaySessionId=", base);
            CHECK(ui_test_page() == PG_PLAYER && fake_rc.opens == open0 + 1 && !strncmp(fake_rc.url, want, strlen(want)) &&
                  strstr(fake_rc.url, "&api_key=JF-TOKEN"), "played from Jellyfin, the file itself: %s", fake_rc.url);
            CHECK(strstr(fake_rc.headers, "Authorization: MediaBrowser Client=\"Matinee\"") && strstr(fake_rc.headers, "Token=\"JF-TOKEN\"") &&
                  !strstr(fake_rc.headers, "X-Plex") && !strstr(fake_rc.headers, "Accept:"), "with Jellyfin's header: %s",
                  fake_rc.headers);
            n_null = 0;
            pc++;
            continue;
        case 8013:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            CHECK(player_ready() && fake_rc.seeks >= 1 && NEAR(fake_rc.seek_to, 2530, 0.01), "resumed where it was left (%d %d %.1f)",
                  player_ready(), fake_rc.seeks, fake_rc.seek_to);
            fake_cs += 1100;                                    /* the timeline's 10 s */
            n_null = 0;
            pc++;
            return NULL_EVENT;
        case 8014:
            CHECK(log_count("/Sessions/Playing", NULL, NULL) == 1, "Jellyfin told it's playing (%d)",
                  log_count("/Sessions/Playing", NULL, NULL));
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 8015:
            pc++;
            return ev_menu(b, MP_STOP, -1);
        case 8016:
            CHECK(ui_test_page() == PG_DETAILS && log_count("/Sessions/Playing/Stopped", NULL, NULL) == 1,
                  "stopped: Jellyfin told");
            CHECK(log_count("/Videos/ActiveEncodings", NULL, NULL) == 0, "the file itself: nothing converting to stop");
            pc = 8030;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape: the grid */
        case 8030:
            pc++;
            return ev_tile(b, find_tile("Jelly Hevc"), 4);
        case 8031:
            CHECK(ui_test_page() == PG_DETAILS && strstr(ui_test_det(2), "Transcoded"), "HEVC: converted (%s)", ui_test_det(2));
            CHECK(log_count("/Items/jm2/PlaybackInfo", NULL, NULL) == 0, "the details say so without asking the server");
            open0 = fake_rc.opens;
            setenv("Matinee$NoHEVCBlock", "1", 1);
            pc++;
            return ev_button(b, w_browser, D_PLAY, 0x400);
        case 8032:
            snprintf(want, sizeof(want), "%s/videos/jm2/master.m3u8?", base);
            CHECK(ui_test_page() == PG_PLAYER && fake_rc.opens == open0 + 1 && !strncmp(fake_rc.url, want, strlen(want)) &&
                  strstr(fake_rc.url, "ApiKey=JF-TOKEN") && log_count("/Items/jm2/PlaybackInfo", NULL, NULL) == 1,
                  "converted: the server's HLS playlist, from PlaybackInfo: %s", fake_rc.url);
            CHECK(fake_rc.open_flags & REELCORE_NO_HEVC_BLOCK, "Matinee$NoHEVCBlock: reelcore told");
            unsetenv("Matinee$NoHEVCBlock");
            n_null = 0;
            pc++;
            continue;
        case 8033:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 8034:
            pc++;
            return ev_menu(b, MP_STOP, -1);
        case 8035:
            CHECK(log_count("/Videos/ActiveEncodings", "playSessionId", "0123456789abcdef0123456789abcdef") == 1,
                  "stopped: the conversion ended under Jellyfin's PlaySessionId");
            player_test_hevc_block = 1;                         /* a Pi 4: the HEVC block */
            fake_rc.hevc_block = 1;
            open0 = fake_rc.opens;
            pc = 8036;
            return ev_button(b, w_browser, D_PLAY, 0x400);
        case 8036:
            snprintf(want, sizeof(want), "%s/Videos/jm2/stream?static=true&mediaSourceId=ms-jm2", base);
            CHECK(ui_test_page() == PG_PLAYER && fake_rc.opens == open0 + 1 && !strncmp(fake_rc.url, want, strlen(want)) &&
                  !(fake_rc.open_flags & REELCORE_NO_HEVC_BLOCK), "with the HEVC block: the HEVC file itself (%s)", fake_rc.url);
            n_null = 0;
            pc++;
            continue;
        case 8037:
            if (!player_ready() && n_null++ < 50) {
                fake_cs += 2;
                return NULL_EVENT;
            }
            pc++;
            return ev_click(b, w_browser, -1, (win(w_browser)->vis[0] + win(w_browser)->vis[2]) / 2,
                            win(w_browser)->vis[3] - 100, 2);
        case 8038:
            pc++;
            return ev_menu(b, MP_STOP, -1);
        case 8039:
            CHECK(ui_test_page() == PG_DETAILS && strstr(ui_test_det(2), "HEVC block"), "the details say so: %s", ui_test_det(2));
            player_test_hevc_block = 0;
            fake_rc.hevc_block = 0;
            pc = 8017;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 8017:
            pc = 8050;
            return ev_menu(b, MB_SERVERS, 1);                   /* Add a server... */
        case 8050:
            CHECK(ui_test_signin_mode() == SI_PICK, "the choice");
            pc++;
            return ev_button(b, w_browser, S_PICK_JF, 0x400);
        case 8051:
            CHECK(ui_test_signin_mode() == SI_JF, "Jellyfin's");
            pc++;
            return ev_button(b, w_browser, S_BACK, 0x400);
        case 8052:
            CHECK(ui_test_signin_mode() == SI_PICK, "Back: the choice again");
            pc++;
            return ev_button(b, w_browser, S_PICK_JF, 0x400);
        case 8053:
            pc++;
            return ev_button(b, w_browser, S_CANCEL, 0x400);
        case 8054:
            CHECK(ui_test_page() == PG_DETAILS && win(w_browser)->open && !strcmp(ui_test_path(), "Cellar > Jelly Films > Jelly Hevc") ,
                  "Cancel: back to the details page it was on (%s)", ui_test_path());
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 8055:
            pc++;
            return ev_menu(b, MB_SERVERS, 1);
        case 8056:
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape on the choice: Cancel too */
        case 8057:
            CHECK(ui_test_page() == PG_DETAILS, "Escape: back as well");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 8058:
            pc++;
            return ev_menu(b, MB_SERVERS, 1);
        case 8059:
            pc = 8018;
            return ev_button(b, w_browser, S_PICK_JF, 0x400);   /* the same server again, by password */
        case 8018:
            CHECK(ui_test_signin_jf() && strstr(ui_test_signin(5), "127.0.0.1:"), "the address kept: %s",
                  ui_test_signin(5));
            snprintf(type_str, sizeof(type_str), "andrew");
            type_i = 0;
            pc++;
            return ev_button(b, w_browser, S_JUSER, 0x400);
        case 8019:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            snprintf(type_str, sizeof(type_str), "wrong");
            type_i = 0;
            pc++;
            return ev_key(b, w_browser, -1, 13);                /* Return: the password */
        case 8020:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            CHECK(ui_test_field() == 2, "Return moved to the password");
            pc++;
            return ev_key(b, w_browser, -1, 13);                /* Return: Sign in */
        case 8021:
            CHECK(ui_test_page() == PG_SIGNIN && strstr(ui_test_signin(1), "didn't take that name and password"),
                  "a wrong password: %s", ui_test_signin(1));
            snprintf(type_str, sizeof(type_str), "secret");
            type_i = 0;
            pc++;
            continue;
        case 8022:
            if (type_str[type_i])
                return ev_key(b, w_browser, -1, (unsigned char)type_str[type_i++]);
            pc++;
            return ev_button(b, w_browser, S_LOGIN, 0x400);
        case 8023:
            CHECK(ui_test_page() == PG_GRID && ui_test_jf_servers() == 1, "signed in by password: still one server kept (%d)",
                  ui_test_jf_servers());
            report_answer = 1;
            prev_reports = reports;
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 8024:
            pc++;
            return ev_menu(b, MB_SIGNOUT, -1);
        case 8025:
            CHECK(reports == prev_reports + 1 && strstr(last_report, "Sign out of Cellar?"), "asked: %s", last_report);
            CHECK(!win(w_browser)->open && ui_test_jf_servers() == 0 && !strstr(read_file(choices), "jellyfin ") &&
                  strstr(read_file(choices), "server_kind 0\n") && log_count("/Sessions/Logout", NULL, NULL) == 1,
                  "signed out of it: forgotten, and the server told:\n%s", read_file(choices));
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 8026:
            CHECK(ui_test_signin_mode() == SI_PICK, "Select: Add a server's choice again");
            pc++;
            return ev_msg(b, 0, 0, 0);
        default:
            printf("  script ran out at %d\n", pc);
            fails++;
            return ev_msg(b, 0, 0, 0);
        }
    }
}

int main(int argc, char **argv)
{
    char cmd[400];
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3)
        return 2;
    snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]);
    snprintf(outdir, sizeof(outdir), "%s", argv[2]);
    snprintf(cmd, sizeof(cmd), "rm -rf %s/scrap %s/choices %s/saved; mkdir -p %s/scrap %s/choices %s/saved",
             outdir, outdir, outdir, outdir, outdir, outdir);
    if (system(cmd) != 0)
        return 2;
    snprintf(scrap, sizeof(scrap), "%s/scrap", outdir);
    snprintf(choices, sizeof(choices), "%s/choices/Choices", outdir);
    setenv("Wimp$ScrapDir", scrap, 1);
    snprintf(cmd, sizeof(cmd), "%s/choices", outdir);
    setenv("Matinee$ChoicesDir", cmd, 1);
    setenv("Matinee$PlexTV", base, 1);
    {
        FILE *f = fopen(choices, "w");      /* the hand-off first: ReelEGL chosen */
        if (f) {
            fputs("choices_version 2\nplayer ReelEGL\n", f);
            fclose(f);
        }
    }
    net_init("ui_test");
    {
        net_buf nb;                 /* the fake server's record, from core_test's run, cleared */
        snprintf(cmd, sizeof(cmd), "%s/_reset", base);
        if (net_fetch(cmd, NULL, "", &nb, 5000, NULL, 0) == 0)
            net_buf_free(&nb);
    }

    CHECK(matinee_main(1, argv) == 0, "first run ends cleanly");
    printf("  first run: %d checks so far\n", checks);

    /* second run: the Choices from the first, made to look like test4's
       (ReelEGL, saved as the default then, and no choices_version) */
    {
        char *c = read_file(choices), out[8192] = "", *l;
        for (l = strtok(c, "\n"); l; l = strtok(NULL, "\n")) {
            if (!strncmp(l, "choices_version", 15))
                continue;
            strcat(out, strncmp(l, "player ", 7) ? l : "player ReelEGL");
            strcat(out, "\n");
        }
        FILE *f = fopen(choices, "w");
        if (f) {
            fputs(out, f);
            fclose(f);
        }
    }
    {
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "test $(find %s/choices/Cache -type f | wc -l) -ge 10", outdir);
        CHECK(system(cmd) == 0, "the pictures kept on disc, in the image cache");
    }
    prev_count = log_count("/photo/:/transcode", "url", "/library/metadata/101/thumb/1700000000");    /* a poster seen in the first run */
    nwins = 0; pc = 100; menu_open = NULL; bar_icon_made = 0;
    ntasks = 0;
    CHECK(matinee_main(1, argv) == 0, "second run ends cleanly");

    /* third run: a Jellyfin server added, played from, signed out of */
    nwins = 0; pc = 8000; menu_open = NULL; bar_icon_made = 0;
    ntasks = 0;
    CHECK(matinee_main(1, argv) == 0, "third run ends cleanly");

    /* a second copy: another Matinee task is running */
    nwins = 0; bar_icon_made = 0;
    tasks[0].handle = 0x999; tasks[0].name = "Matinee\r";
    ntasks = 1;
    CHECK(matinee_main(1, argv) == 0 && bar_icon_made == 0, "one copy only");

    {
        /* the image cache held to its size: the oldest go first */
        char dir[400], cmd[800], *d = NULL;
        size_t n;
        static char pic[1000];
        snprintf(dir, sizeof(dir), "%s/cache2", outdir);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0, "a fresh cache");
        imgcache_init(dir, 1 << 20);
        for (int i = 0; i < 4; i++) {
            char key[16];
            snprintf(key, sizeof(key), "pic%d", i);
            memset(pic, 'a' + i, sizeof(pic));
            imgcache_put(key, pic, sizeof(pic));
        }
        CHECK(imgcache_size(NULL) == 4000, "four kept (%lld bytes)", imgcache_size(NULL));
        for (int i = 0; i < 4; i++) {           /* pic0's file the oldest, pic3's the newest */
            snprintf(cmd, sizeof(cmd), "for f in $(find %s -type f); do head -c1 $f | grep -q %c && touch -d @%d $f; done; true",
                     dir, 'a' + i, 1000000000 + i);
            CHECK(system(cmd) == 0, "dated");
        }
        imgcache_init(dir, 3000);                               /* 4000 > 3000: down to 2250 or less */
        CHECK(imgcache_size(NULL) == 2000 && imgcache_get("pic0", &d, &n) != 0 && imgcache_get("pic1", &d, &n) != 0,
              "trimmed: the two oldest gone (%lld)", imgcache_size(NULL));
        CHECK(imgcache_get("pic3", &d, &n) == 0 && n == 1000 && d[0] == 'd', "the newest kept");
        free(d);
    }
    printf("ui_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
