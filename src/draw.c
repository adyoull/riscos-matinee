/*
 * draw.c - shapes and text for PlexRO's windows (see draw.h).
 * Part of riscos-plex. GPL v2 or later.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>

#include "draw.h"

#define OS_Plot                    0x45
#define Wimp_TextOp                0x400F9
#define Font_FindFont              0x40081
#define Font_LoseFont              0x40082
#define Font_Paint                 0x40086
#define Font_ScanString            0x400A1
#define ColourTrans_SetGCOL        0x40743
#define ColourTrans_SetFontColours 0x4074F

#define MPT_PER_OS 400              /* millipoints in an OS unit (1/180 inch) */

#define Wimp_PlotIcon              0x400E2

static int font[D_FONTS];           /* Font Manager handles; 0 = the desktop font */
static int xeig = 1, yeig = 1, org_x, org_y;

/* Smooth shapes: small 32bpp sprites, each pixel the shape's colour mixed
   with the background by how much of it the shape covers (4 x 4 samples).
   Made when first wanted, kept (the least used go first). */
#define SHAPES 64
static struct {
    int g, w, h;                    /* glyph, size in pixels */
    unsigned c, bg;
    int *area;
    unsigned used;
} shape[SHAPES];
static unsigned shape_clock;

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

static int find(const char *name, int pt)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)name;
    r.r[2] = pt * 16;
    r.r[3] = pt * 16;
    r.r[4] = 0;
    r.r[5] = 0;
    return swi(Font_FindFont, &r) ? 0 : r.r[0];
}

void draw_init(int xe, int ye)
{
    draw_done();
    xeig = xe;
    yeig = ye;
    font[D_BOLD] = find("Homerton.Bold", 12);
    font[D_TITLE] = find("Homerton.Bold", 20);
}

void draw_origin(int ox, int oy)
{
    org_x = ox;
    org_y = oy;
}

void draw_done(void)
{
    for (int i = 0; i < SHAPES; i++) {
        free(shape[i].area);
        shape[i].area = NULL;
    }
    for (int i = 1; i < D_FONTS; i++)
        if (font[i]) {
            _kernel_swi_regs r;
            r.r[0] = font[i];
            swi(Font_LoseFont, &r);
            font[i] = 0;
        }
}

static void colour(unsigned c)
{
    _kernel_swi_regs r;
    r.r[0] = (int)c;
    r.r[3] = 0;                     /* foreground */
    r.r[4] = 0;                     /* overwrite */
    swi(ColourTrans_SetGCOL, &r);
}

static void plot(int code, int x, int y)
{
    _kernel_swi_regs r;
    r.r[0] = code;
    r.r[1] = x;
    r.r[2] = y;
    swi(OS_Plot, &r);
}

void draw_rect(int x0, int y0, int x1, int y1, unsigned c)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    colour(c);
    plot(4, x0, y0);                /* move */
    plot(101, x1 - 1, y1 - 1);      /* rectangle fill, absolute */
}

/* ---- smooth shapes ------------------------------------------------------ */

static int in_tri(double x, double y, double ax, double ay, double bx, double by, double cx, double cy)
{
    double e0 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    double e1 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    double e2 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

static double seg_dist(double x, double y, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay, t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    dx = ax + t * dx - x;
    dy = ay + t * dy - y;
    return sqrt(dx * dx + dy * dy);
}

/* Is (u, v) in the glyph? u across, v down, both 0..1 over its box */
static int inside(int g, double u, double v)
{
    double x = u - 0.5, y = 0.5 - v, a;     /* from the middle, y up */
    switch (g) {
    case G_PLAY:   return in_tri(u, v, 0.18, 0.08, 0.18, 0.92, 0.92, 0.5);
    case G_DOWN:   return in_tri(u, v, 0.1, 0.28, 0.9, 0.28, 0.5, 0.76);
    case G_CIRCLE: return x * x + y * y <= 0.25;
    case G_BACK:
        return seg_dist(u, v, 0.66, 0.14, 0.3, 0.5) <= 0.09 || seg_dist(u, v, 0.3, 0.5, 0.66, 0.86) <= 0.09;
    case G_REFRESH:
        /* a ring, open between 45 and 110 degrees, with an arrow at one end */
        if (in_tri(x, y, 0.345, 0.345, 0.105, 0.105, 0.07, 0.40))
            return 1;
        a = atan2(y, x) * 180 / 3.14159265;
        if (a > 45 && a < 110)
            return 0;
        a = sqrt(x * x + y * y);
        return a >= 0.24 && a <= 0.36;
    /* a corner of a rounded rectangle: a quarter circle */
    case G_CORNER_TL: return (1 - u) * (1 - u) + (1 - v) * (1 - v) <= 1;
    case G_CORNER_TR: return u * u + (1 - v) * (1 - v) <= 1;
    case G_CORNER_BL: return (1 - u) * (1 - u) + v * v <= 1;
    case G_CORNER_BR: return u * u + v * v <= 1;
    case G_SEARCH:                  /* a magnifying glass: a ring, and a handle to the bottom right */
        a = sqrt((u - 0.42) * (u - 0.42) + (v - 0.42) * (v - 0.42));
        return (a >= 0.2 && a <= 0.3) || seg_dist(u, v, 0.63, 0.63, 0.88, 0.88) <= 0.08;
    }
    return 0;
}

static int *shape_make(int g, int w, int h, unsigned c, unsigned bg)
{
    size_t image = (size_t)w * h * 4;
    int *a = malloc(16 + 44 + image), *s;
    unsigned *px;
    int fr = c >> 8 & 255, fg = c >> 16 & 255, fb = c >> 24 & 255;
    int br = bg >> 8 & 255, bgg = bg >> 16 & 255, bb = bg >> 24 & 255;
    if (!a)
        return NULL;
    a[0] = (int)(16 + 44 + image);
    a[1] = 1;
    a[2] = 16;
    a[3] = a[0];
    s = a + 4;
    s[0] = (int)(44 + image);
    memset(&s[1], 0, 12);
    ((char *)&s[1])[0] = 'g';
    s[4] = w - 1;
    s[5] = h - 1;
    s[6] = 0;
    s[7] = 31;
    s[8] = 44;
    s[9] = 44;                      /* no mask: the edges are mixed with bg */
    s[10] = (int)(1u | ((unsigned)(180 >> xeig) << 1) | ((unsigned)(180 >> yeig) << 14) | (6u << 27));
    px = (unsigned *)(s + 11);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    n += inside(g, (x + (i + 0.5) / 4) / w, (y + (j + 0.5) / 4) / h);
            px[y * w + x] = (unsigned)((fr * n + br * (16 - n)) / 16) |
                            (unsigned)((fg * n + bgg * (16 - n)) / 16) << 8 |
                            (unsigned)((fb * n + bb * (16 - n)) / 16) << 16;
        }
    return a;
}

void draw_glyph(int g, int x0, int y0, int x1, int y1, unsigned c, unsigned bg)
{
    int w = (x1 - x0) >> xeig, h = (y1 - y0) >> yeig, k = -1, old = 0;
    struct { int box[4]; unsigned flags; int data[3]; } ic;
    _kernel_swi_regs r;
    if (w <= 0 || h <= 0)
        return;
    for (int i = 0; i < SHAPES; i++) {
        if (shape[i].area && shape[i].g == g && shape[i].w == w && shape[i].h == h && shape[i].c == c &&
            shape[i].bg == bg) {
            k = i;
            break;
        }
        if (!shape[i].area || shape[i].used < shape[old].used)
            old = i;
    }
    if (k < 0) {                    /* a new one, over the least used */
        k = old;
        free(shape[k].area);
        shape[k].g = g; shape[k].w = w; shape[k].h = h; shape[k].c = c; shape[k].bg = bg;
        if (!(shape[k].area = shape_make(g, w, h, c, bg)))
            return;
    }
    shape[k].used = ++shape_clock;
    ic.box[0] = x0 - org_x;
    ic.box[1] = y0 - org_y;
    ic.box[2] = x0 - org_x + (w << xeig);
    ic.box[3] = y0 - org_y + (h << yeig);
    ic.flags = 0x0000011Au;         /* sprite, centred, indirected */
    ic.data[0] = (int)(intptr_t)"g";
    ic.data[1] = (int)(intptr_t)shape[k].area;
    ic.data[2] = 1;
    r.r[1] = (intptr_t)&ic;
    _kernel_swi(Wimp_PlotIcon, &r, &r);
}

void draw_round(int x0, int y0, int x1, int y1, int r, unsigned c, unsigned bg)
{
    int xm = (1 << xeig) - 1, ym = (1 << yeig) - 1;
    /* on whole pixels, so the corners meet the straight parts */
    x0 &= ~xm; x1 &= ~xm; y0 &= ~ym; y1 &= ~ym;
    r &= ~(xm | ym);
    if (x1 <= x0 || y1 <= y0)
        return;
    if (r * 2 > x1 - x0)
        r = ((x1 - x0) / 2) & ~(xm | ym);
    if (r * 2 > y1 - y0)
        r = ((y1 - y0) / 2) & ~(xm | ym);
    if (r < 4) {
        draw_rect(x0, y0, x1, y1, c);
        return;
    }
    draw_rect(x0 + r, y0, x1 - r, y1, c);
    draw_rect(x0, y0 + r, x0 + r, y1 - r, c);
    draw_rect(x1 - r, y0 + r, x1, y1 - r, c);
    draw_glyph(G_CORNER_TL, x0, y1 - r, x0 + r, y1, c, bg);
    draw_glyph(G_CORNER_TR, x1 - r, y1 - r, x1, y1, c, bg);
    draw_glyph(G_CORNER_BL, x0, y0, x0 + r, y0 + r, c, bg);
    draw_glyph(G_CORNER_BR, x1 - r, y0, x1, y0 + r, c, bg);
}

void draw_tri(int x0, int y0, int x1, int y1, int x2, int y2, unsigned c)
{
    colour(c);
    plot(4, x0, y0);
    plot(4, x1, y1);
    plot(85, x2, y2);               /* triangle fill: the last two points and this */
}

void draw_text(int f, int x, int y, const char *s, unsigned fg, unsigned bg)
{
    _kernel_swi_regs r;
    if (f > 0 && font[f]) {
        r.r[0] = font[f];
        r.r[1] = (int)bg;
        r.r[2] = (int)fg;
        r.r[3] = 14;                /* anti-aliased */
        swi(ColourTrans_SetFontColours, &r);
        r.r[0] = font[f];
        r.r[1] = (intptr_t)s;
        r.r[2] = 0x110;             /* OS units; the handle in R0 */
        r.r[3] = x;
        r.r[4] = y;
        swi(Font_Paint, &r);
        return;
    }
    r.r[0] = 0;                     /* the desktop font's colours */
    r.r[1] = (int)fg;
    r.r[2] = (int)bg;
    swi(Wimp_TextOp, &r);
    r.r[0] = 2;
    r.r[1] = (intptr_t)s;
    r.r[2] = -1;
    r.r[3] = -1;
    r.r[4] = x;
    r.r[5] = y;
    swi(Wimp_TextOp, &r);
}

int draw_width(int f, const char *s)
{
    _kernel_swi_regs r;
    if (f > 0 && font[f]) {
        r.r[0] = font[f];
        r.r[1] = (intptr_t)s;
        r.r[2] = 0x100;
        r.r[3] = 0x7FFFFFFF;
        r.r[4] = 0x7FFFFFFF;
        if (!swi(Font_ScanString, &r))
            return r.r[3] / MPT_PER_OS;
    }
    r.r[0] = 1;
    r.r[1] = (intptr_t)s;
    r.r[2] = 0;
    return swi(Wimp_TextOp, &r) ? (int)strlen(s) * 16 : r.r[0];
}

int draw_height(int f)
{
    return f == D_TITLE && font[f] ? 56 : f == D_BOLD && font[f] ? 36 : 36;
}

void draw_fit(int f, char *s, int width)
{
    size_t n = strlen(s);
    char t[200];
    if (draw_width(f, s) <= width)
        return;
    while (n > 0) {
        s[--n] = 0;
        while (n > 0 && s[n - 1] == ' ')
            s[--n] = 0;
        if (n + 4 > sizeof(t))
            continue;
        snprintf(t, sizeof(t), "%s...", s);
        if (draw_width(f, t) <= width) {
            strcpy(s, t);
            return;
        }
    }
}

int draw_wrap(int f, const char *s, int width, char lines[][160], int max)
{
    int n = 0;
    while (*s && n < max) {
        size_t best = 0, i = 0;
        char t[160];
        while (*s == ' ')
            s++;
        /* the most whole words that fit */
        for (;;) {
            size_t j = i;
            while (s[j] && s[j] != ' ')
                j++;
            if (j >= sizeof(t))
                break;
            memcpy(t, s, j);
            t[j] = 0;
            if (draw_width(f, t) > width)
                break;
            best = j;
            if (!s[j])
                break;
            i = j + 1;
        }
        if (!best) {                /* one word too long for a line: cut it */
            best = strcspn(s, " ");
            if (best >= sizeof(t))
                best = sizeof(t) - 1;
        }
        memcpy(lines[n], s, best);
        lines[n][best] = 0;
        if (n == max - 1 && s[best]) {  /* more than fits: the last line ends "..." */
            snprintf(t, sizeof(t), "%s %s", lines[n], s + best);
            t[sizeof(t) - 1] = 0;
            draw_fit(f, t, width);
            snprintf(lines[n], 160, "%s", t);
        }
        n++;
        s += best;
    }
    return n;
}
