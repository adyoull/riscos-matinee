/*
 * ui_test.c - PlexRO's front end (src/ui.c) against a scripted fake Wimp
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
 * Part of riscos-plex. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel.h"
#include "ui.h"
#include "net.h"
#include "cJSON.h"
#include "sources.h"            /* Reel's (riscos-ffmpeg player/sources.c) */
#include "panel_font.h"         /* Reel's bitmap font (riscos-ffmpeg reelcore/), for the pictures */

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
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
static int *output_sprite;              /* where output goes (NULL: the screen) */
static int jpeg_plots, jpeg_scale[4], jpeg_into_sprite = 1;
static int drag_started;
static int pointer_w = -1, pointer_i = -1, pointer_x = 640, pointer_y = 480;
static int poll_null_mask_bad;

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

static void fb_text(int x, int y, const char *s, unsigned fg, int scale)
{
    /* the baseline at y: the font's cell is 18 pixels, its baseline 14 down */
    int top = y + (14 * scale) * 2;
    for (; *s; s++, x += PANEL_FONT_W * 2 * scale) {
        unsigned char c = (unsigned char)*s;
        int g = c >= 32 && c <= 126 ? c - 32 : c >= 160 ? 95 + c - 160 : 0;
        for (int row = 0; row < PANEL_FONT_H * scale; row++)
            for (int col = 0; col < PANEL_FONT_W * scale; col++) {
                int a = panel_font[g][(row / scale) * PANEL_FONT_W + col / scale];
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

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "fake: not handled" };
    switch (swi) {
    case 0x400C0:                                   /* Wimp_Initialise */
        CHECK(!strcmp((const char *)(intptr_t)in->r[2], "PlexRO"), "task name");
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
        out->r[2] = in->r[1] == 4 || in->r[1] == 5 ? 1 : in->r[1] == 11 ? 1919 : in->r[1] == 12 ? 1079 : 0;
        return NULL;
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
        CHECK(!strcmp((const char *)(intptr_t)in->r[1], "Homerton.Bold"), "font name");
        out->r[0] = in->r[2] >= 320 ? 2 : 1;
        return NULL;
    case 0x40082: return NULL;                      /* Font_LoseFont */
    case 0x4074F: font_fg = rgb_of(in->r[2]); return NULL;      /* ColourTrans_SetFontColours */
    case 0x400A1:                                   /* Font_ScanString: millipoints */
        out->r[3] = (long)strlen((const char *)(intptr_t)in->r[1]) * (in->r[0] == 2 ? 32 : 16) * 400;
        return NULL;
    case 0x40086: {                                 /* Font_Paint */
        const char *t = (const char *)(intptr_t)in->r[1];
        CHECK((in->r[2] & 0x110) == 0x110, "Font_Paint in OS units, with a handle");
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
    case 0x400CD: icons_refreshed++; return NULL;   /* Wimp_SetIconState */
    case 0x400D2: case 0x400D3: return NULL;        /* caret */
    case 0x400F9:                                   /* Wimp_TextOp */
        if (in->r[0] == 0) { text_fg = rgb_of(in->r[1]); return NULL; }
        if (in->r[0] == 1) { out->r[0] = (long)strlen((const char *)(intptr_t)in->r[1]) * 18; return NULL; }
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
        if ((in->r[0] & 0xFF) != 60) return &err;
        if (in->r[2] == 0) { output_sprite = NULL; return NULL; }
        CHECK((in->r[0] & 0x200) && in->r[2] == in->r[1] + 16, "output switched to the sprite by pointer");
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

static int pc, speed_nulls, save_nulls, drain_n, prev_nsent, prev_started, prev_reports, prev_count;
static char save_path[300], save_path2[300];

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
    plot_sprites = 0;
    plotted_area = NULL;
    shapes = 0;
    glyphs = 0;
    return 1;                                           /* Redraw_Window_Request */
}

static void save_picture(const char *name, int w)
{
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
            CHECK(bar_icon_made == 1 && !strcmp(bar_sprite, "!plexro"), "icon bar icon '%s'", bar_sprite);
            CHECK(proginfo_made == 1, "Info window");
            CHECK(strstr(read_file(choices), "client_id plexro-") != NULL, "a client id kept from the start");
            CHECK(mask & 1, "no null events while there's nothing to do");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);             /* Select on the icon */
        case 1:
            CHECK(win(w_browser)->open && ui_test_page() == PG_SIGNIN,
                  "Select before signing in: the window, on its sign-in page");
            CHECK(!strcmp(ui_test_signin(0), "ABCD"), "the code: %s", ui_test_signin(0));
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 200, "checked every 2 s (PollIdle %d, now %d)",
                  idle_time, fake_cs);
            pc = 150;
            return ev_redraw(b, w_browser);
        case 150:
            CHECK(strstr(plotted_text, "Sign in|") && strstr(plotted_text, "Sign in with a code|") &&
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
            pc++;
            return NULL_EVENT;
        case 3:
            CHECK(ui_test_page() == PG_GRID, "signed in: the grid");
            CHECK(win(w_browser)->open, "in the same window");
            CHECK(ui_test_items() == 4, "the top list: %d items", ui_test_items());
            CHECK(!strcmp(ui_test_path(), "Attic"), "where: %s", ui_test_path());
            CHECK(!strcmp(ui_test_item(1, 0), "Films") && !strcmp(ui_test_item(3, 0), "Music"), "tiles %s, %s",
                  ui_test_item(1, 0), ui_test_item(3, 0));
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
            CHECK(strstr(plotted_text, "Attic|") && strstr(plotted_text, "4 items.|"), "the header: %s", plotted_text);
            CHECK(strstr(plotted_text, "Continue wa...|") && strstr(plotted_text, "Films|") &&
                  strstr(plotted_text, "TV Programmes|") && strstr(plotted_text, "Folder|"),
                  "the tiles drawn: %s", plotted_text);
            CHECK(shapes > 20 && glyphs > 20 && !plot_sprites, "drawn in shapes and smooth corners, no posters yet (%d, %d)",
                  shapes, glyphs);
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
            CHECK(log_count("/photo/:/transcode", "width", "116") >= 1 && log_count("/photo/:/transcode", "height", "174") >= 1,
                  "posters asked for at the tile's size in pixels");
            pc++;
            return ev_tile(b, 1, 4);                            /* double-click Films */
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
            CHECK(strstr(ui_test_det(2), "Converted by the server") && strstr(ui_test_det(2), "bigger"),
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
            const int *spr = plotted_area ? plotted_area + 4 : NULL;
            int w = spr ? spr[4] + 1 : 0, h = spr ? spr[5] + 1 : 0;
            unsigned bottom = spr ? ((const unsigned *)(spr + 11))[(h - 1) * w + w / 2] : 0;
            CHECK(spr && w == 554 && h == 311 && spr[9] == spr[8], "the backdrop: the window's width, 16:9, no mask (%d x %d)",
                  w, h);
            CHECK(abs((int)(bottom & 255) - 0x18) < 3 && abs((int)(bottom >> 8 & 255) - 0x1A) < 3 &&
                  abs((int)(bottom >> 16) - 0x1F) < 3, "faded to the window's grey at its foot (%06x)", bottom);
            CHECK(strstr(plotted_text, "Big Buck Bunny|") && strstr(plotted_text, "Play|") &&
                  strstr(plotted_text, "Resume from 42:10|") && strstr(plotted_text, "Subtitles: None|"),
                  "the details drawn: %s", plotted_text);
            save_picture("details.ppm", w_browser);
            prev_started = nstarted;
            pc++;
            return ev_button(b, w_browser, D_PLAY, 4);
        }
        case 16: {
            int k = (nstarted - 1) & 7;
            snprintf(want, sizeof(want), "Run <ReelEGL$Dir>.!Run %s/PlexRO/Play0", scrap);
            CHECK(nstarted == prev_started + 1 && !strcmp(started[k], want), "Play: started %s", started[k]);
            CHECK(strstr(started_url(), "/video/:/transcode/universal/start.m3u8?") &&
                  strstr(started_url(), "&offset=2530&") && strstr(started_url(), "videoResolution=1280x720"),
                  "720p (the default): converted, from where it was left: %s", started_url());
            CHECK(started_nsrc[k] == 1 && started_src[k].title && !strcmp(started_src[k].title, "Big Buck Bunny") &&
                  started_src[k].headers && strstr(started_src[k].headers, "X-Plex-Token: SRV-TOKEN"),
                  "title and headers for Reel");
            CHECK(ntyped && typed_type[(ntyped - 1) & 7] == 0xBF4, "typed as video/mp4");
            snprintf(want, sizeof(want), "%s/PlexRO/Play0", scrap);
            CHECK(!file_exists(want), "the file deleted once the player has started");
            CHECK(strstr(ui_test_status(), "Converted") && strstr(ui_test_status(), "bigger"), "status: %s",
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
            CHECK(strstr(ui_test_det(2), "Direct play"), "the details say so: %s", ui_test_det(2));
            tasks[0].handle = 0x777; tasks[0].name = "ReelEGL\r";
            tasks[1].handle = 0x778; tasks[1].name = "Reel\r";
            tasks[2].handle = task; tasks[2].name = "PlexRO\r";
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
            snprintf(want, sizeof(want), "%s/PlexRO/Play1", scrap);
            CHECK(!file_exists(want), "deleted on DataLoadAck");
            CHECK(strstr(ui_test_status(), "in ReelEGL") && strstr(ui_test_status(), "Direct play"),
                  "status: %s", ui_test_status());
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 21:
            pc++;
            return ev_menu(b, MB_PLAYER, 1);                    /* Reel */
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
            tasks[0].handle = task; tasks[0].name = "PlexRO\r";
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
            return ev_menu(b, MB_PLAYER, 0);                    /* back to ReelEGL */
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
            /* ---- Save original file, from the details */
            pc++;
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
            CHECK(ui_test_items() == 4 && !strcmp(ui_test_path(), "Attic") && ui_test_sel() == 1,
                  "Back: the top again, Films selected (%d, %s)", ui_test_items(), ui_test_path());
            pc++;
            return ev_tile(b, 2, 4);
        case 55:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_item(0, 1), "2 seasons"), "a show");
            pc++;
            return ev_key(b, w_browser, -1, 13);                /* Return opens the one selected */
        case 56:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_path(), "Attic > TV Programmes > Space Show"),
                  "seasons: %s", ui_test_path());
            pc++;
            return ev_tile(b, 0, 4);
        case 57:
            CHECK(ui_test_items() == 6 && !strcmp(ui_test_item(2, 0), "Episode '3'") &&
                  !strcmp(ui_test_item(2, 1), "S1 E3"), "episodes, Latin-1: %s / %s", ui_test_item(2, 0),
                  ui_test_item(2, 1));
            pc++;
            return ev_key(b, w_browser, -1, 0x18D);             /* Right */
        case 58:
            CHECK(ui_test_sel() == 1, "Right moves the selection");
            pc++;
            return ev_key(b, w_browser, -1, 0x18E);             /* Down (4 a row) */
        case 59:
            CHECK(ui_test_sel() == 5, "Down: a row on (%d)", ui_test_sel());
            pc++;
            return ev_key(b, w_browser, -1, 0x1CC);             /* F12: not ours */
        case 60:
            CHECK(keys_passed == 1, "other keys passed on");
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape */
        case 61:
            CHECK(!strcmp(ui_test_path(), "Attic > TV Programmes > Space Show"), "Escape goes back: %s",
                  ui_test_path());
            pc++;
            return ev_button(b, w_browser, B_BACK, 0x400);      /* the Back button */
        case 62:
            CHECK(!strcmp(ui_test_path(), "Attic > TV Programmes"), "Back button: %s", ui_test_path());
            pc++;
            return ev_key(b, w_browser, -1, 0x7F);              /* Delete: back too */
        case 63:
            CHECK(!strcmp(ui_test_path(), "Attic"), "the top: %s", ui_test_path());
            prev_count = log_count("/library/sections", NULL, NULL);
            pc++;
            return ev_button(b, w_browser, B_REFRESH, 0x400);   /* Refresh */
        case 64:
            CHECK(log_count("/library/sections", NULL, NULL) == prev_count + 1 && ui_test_items() == 4,
                  "Refresh fetches the list again");
            pc++;
            return ev_tile(b, 3, 4);                            /* Music */
        case 65:
            CHECK(ui_test_items() == 4 && strstr(ui_test_status(), "can't be opened yet"), "music: %s",
                  ui_test_status());
            pc = 650;
            continue;
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
            pc++;
            return ev_menu(b, MB_SIZE, 1);
        /* ---- sign out, then a server typed by hand */
        case 78:
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 79:
            pc++;
            return ev_menu(b, MB_SIGNOUT, -1);
        case 80:
            CHECK(!win(w_browser)->open, "signed out: the browser closed");
            CHECK(strstr(read_file(choices), "account_token \n") && strstr(read_file(choices), "server_token \n"),
                  "and the tokens forgotten:\n%s", read_file(choices));
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 81:
            CHECK(ui_test_page() == PG_SIGNIN && win(w_browser)->open, "Select: the sign-in page again");
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
            CHECK(ui_test_page() == PG_GRID && win(w_browser)->open && ui_test_items() == 4, "by hand: the grid");
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
            CHECK(win(w_browser)->open && ui_test_items() == 4 && ui_test_page() == PG_GRID,
                  "Select: straight to the browser, from Choices");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 102:
            CHECK(menu_open && menu_flags(menu_open, MB_QUIT) & 0x80, "Quit is the last item");
            pc++;
            return ev_menu(b, MB_QUIT, -1);
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
    setenv("PlexRO$ChoicesDir", cmd, 1);
    setenv("PlexRO$PlexTV", base, 1);
    net_init("ui_test");
    {
        net_buf nb;                 /* the fake server's record, from core_test's run, cleared */
        snprintf(cmd, sizeof(cmd), "%s/_reset", base);
        if (net_fetch(cmd, NULL, "", &nb, 5000, NULL, 0) == 0)
            net_buf_free(&nb);
    }

    CHECK(plexro_main(1, argv) == 0, "first run ends cleanly");
    printf("  first run: %d checks so far\n", checks);

    /* second run: the Choices from the first */
    nwins = 0; pc = 100; menu_open = NULL; bar_icon_made = 0;
    ntasks = 0;
    CHECK(plexro_main(1, argv) == 0, "second run ends cleanly");

    /* a second copy: another PlexRO task is running */
    nwins = 0; bar_icon_made = 0;
    tasks[0].handle = 0x999; tasks[0].name = "PlexRO\r";
    ntasks = 1;
    CHECK(plexro_main(1, argv) == 0 && bar_icon_made == 0, "one copy only");

    printf("ui_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
