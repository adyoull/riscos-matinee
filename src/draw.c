/*
 * draw.c - shapes and text for Matinee's windows (see draw.h).
 * Part of riscos-matinee. GPL v2 or later.
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
static int dots_w[D_FONTS];         /* the width of "..." in each (0: not measured yet) */
/* The font colours last set (by font handle): the same again within one
   rectangle of a redraw needn't be set again (draw_origin forgets them,
   as other tasks set theirs between our redraws) */
static int last_font = -1;
static unsigned last_fg, last_bg;
static int xeig = 1, yeig = 1, org_x, org_y;

/* Smooth shapes: small 32bpp sprites, each pixel the shape's colour mixed
   with the background by how much of it the shape covers (4 x 4 samples).
   Made when first wanted, kept (the least used go first). */
#define SHAPES 256                  /* a page has ~40: corners in a few colours and sizes, the glyphs */
static struct {
    int g, w, h;                    /* glyph, size in pixels */
    unsigned c, bg;
    int *area;
    unsigned used;
} shape[SHAPES];
static unsigned shape_clock;
#ifdef MATINEE_TEST
unsigned draw_test_made, draw_test_scans, draw_test_widths, draw_test_fits, draw_test_wraps;
#endif

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
    font[D_HEAD] = find("Homerton.Bold", 16);
    font[D_HERO] = find("Homerton.Bold", 28);
    memset(dots_w, 0, sizeof(dots_w));
    last_font = -1;
}

void draw_origin(int ox, int oy)
{
    org_x = ox;
    org_y = oy;
    last_font = -1;
}

/* ColourTrans_SetFontColours, unless they're already those */
static void font_colours(int handle, unsigned fg, unsigned bg, int over)
{
    _kernel_swi_regs r;
    if (handle == last_font && fg == last_fg && bg == last_bg && !over)
        return;
    r.r[0] = handle;
    r.r[1] = over ? 0 : (int)bg;
    r.r[2] = (int)fg;
    r.r[3] = 14;
    swi(ColourTrans_SetFontColours, &r);
    last_font = over ? -1 : handle;     /* blended: set again next time (the background is the screen's) */
    last_fg = fg;
    last_bg = bg;
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
    case G_NEXT:                    /* a chevron pointing right */
        return seg_dist(u, v, 0.34, 0.14, 0.7, 0.5) <= 0.09 || seg_dist(u, v, 0.7, 0.5, 0.34, 0.86) <= 0.09;
    case G_TICK:                    /* a tick */
        return seg_dist(u, v, 0.22, 0.52, 0.42, 0.72) <= 0.08 || seg_dist(u, v, 0.42, 0.72, 0.8, 0.3) <= 0.08;
    case G_STAR: {                  /* five points: in the polygon of its ten corners (even-odd) */
        double px[10], py[10];
        int in = 0;
        for (int k = 0; k < 10; k++) {
            double r = k & 1 ? 0.2 : 0.5, t = -1.5707963 + k * 0.6283185;
            px[k] = r * cos(t);
            py[k] = r * sin(t) + 0.04;
        }
        for (int k = 0, j = 9; k < 10; j = k++)
            if (((py[k] > -y) != (py[j] > -y)) && (x < (px[j] - px[k]) * (-y - py[k]) / (py[j] - py[k]) + px[k]))
                in = !in;
        return in;
    }
    case G_SEARCH:                  /* a magnifying glass: a ring, and a handle to the bottom right */
        a = sqrt((u - 0.42) * (u - 0.42) + (v - 0.42) * (v - 0.42));
        return (a >= 0.2 && a <= 0.3) || seg_dist(u, v, 0.63, 0.63, 0.88, 0.88) <= 0.08;
    }
    return 0;
}

static int *shape_make(int g, int w, int h, unsigned c, unsigned bg)
{
    size_t image = (size_t)w * h * 4, words = (size_t)(w + 31) / 32, mask = bg == DRAW_NONE ? words * 4 * h : 0;
    int *a = malloc(16 + 44 + image + mask), *s;
    unsigned *px, *m;
    int fr = c >> 8 & 255, fg = c >> 16 & 255, fb = c >> 24 & 255;
    int br = bg >> 8 & 255, bgg = bg >> 16 & 255, bb = bg >> 24 & 255;
    if (!a)
        return NULL;
    a[0] = (int)(16 + 44 + image + mask);
    a[1] = 1;
    a[2] = 16;
    a[3] = a[0];
    s = a + 4;
    s[0] = (int)(44 + image + mask);
    memset(&s[1], 0, 12);
    ((char *)&s[1])[0] = 'g';
    s[4] = w - 1;
    s[5] = h - 1;
    s[6] = 0;
    s[7] = 31;
    s[8] = 44;
    s[9] = (int)(44 + (mask ? image : 0));  /* no mask: the edges are mixed with bg */
    s[10] = (int)(1u | ((unsigned)(180 >> xeig) << 1) | ((unsigned)(180 >> yeig) << 14) | (6u << 27));
    px = (unsigned *)(s + 11);
    m = (unsigned *)((char *)s + 44 + image);
    if (mask)
        memset(m, 0, mask);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    n += inside(g, (x + (i + 0.5) / 4) / w, (y + (j + 0.5) / 4) / h);
            if (mask) {             /* in or out, by more than half */
                if (n >= 8)
                    m[y * words + x / 32] |= 1u << (x & 31);
                n = 16;
            }
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
#ifdef MATINEE_TEST
        draw_test_made++;
#endif
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

void draw_text_over(int f, int x, int y, const char *s, unsigned fg)
{
    _kernel_swi_regs r;
    if (f > 0 && font[f]) {
        font_colours(font[f], fg, 0, 1);
        r.r[0] = font[f];
        r.r[1] = (intptr_t)s;
        r.r[2] = 0x910;             /* OS units; the handle in R0; blended with the screen */
        r.r[3] = x;
        r.r[4] = y;
        if (!swi(Font_Paint, &r))
            return;
    }
    draw_text(f, x, y, s, fg, RGB(24, 26, 31));
}

void draw_text(int f, int x, int y, const char *s, unsigned fg, unsigned bg)
{
    _kernel_swi_regs r;
    if (f > 0 && font[f]) {
        font_colours(font[f], fg, bg, 0);  /* anti-aliased, over bg */
        r.r[0] = font[f];
        r.r[1] = (intptr_t)s;
        r.r[2] = 0x110;             /* OS units; the handle in R0 */
        r.r[3] = x;
        r.r[4] = y;
        swi(Font_Paint, &r);
        return;
    }
    last_font = -1;                 /* the desktop font sets the font manager's colours too */
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

/* The width of the first n characters of s (n < 0: all of it), OS units */
static int width_n(int f, const char *s, int n)
{
    _kernel_swi_regs r;
#ifdef MATINEE_TEST
    draw_test_scans++;
#endif
    if (n < 0)
        n = (int)strlen(s);
    if (f > 0 && font[f]) {
        r.r[0] = font[f];
        r.r[1] = (intptr_t)s;
        r.r[2] = 0x180;             /* the handle in R0; R7 is the length */
        r.r[3] = 0x7FFFFFFF;
        r.r[4] = 0x7FFFFFFF;
        r.r[7] = n;
        if (!swi(Font_ScanString, &r))
            return r.r[3] / MPT_PER_OS;
    }
    if (!n)
        return 0;
    r.r[0] = 1;
    r.r[1] = (intptr_t)s;
    r.r[2] = n;
    return swi(Wimp_TextOp, &r) ? n * 16 : r.r[0];
}

int draw_width(int f, const char *s)
{
#ifdef MATINEE_TEST
    draw_test_widths++;
#endif
    return width_n(f, s, -1);
}

/* How many of s's first len characters fit in width (OS units). The font
   manager says where a scan limited to the width stops, in one call; that
   is checked (it and one more character), and a binary search is the
   fallback, so it's a few calls rather than one a character. */
static int fit_len(int f, const char *s, int len, int width)
{
    int lo = 0, hi = len, guess = -1;
    _kernel_swi_regs r;
    if (width <= 0)
        return 0;
    if (f > 0 && font[f]) {
#ifdef MATINEE_TEST
        draw_test_scans++;
#endif
        r.r[0] = font[f];
        r.r[1] = (intptr_t)s;
        r.r[2] = 0x180;
        r.r[3] = width * MPT_PER_OS;
        r.r[4] = 0x7FFFFFFF;
        r.r[7] = len;
        if (!swi(Font_ScanString, &r) && r.r[1] >= (intptr_t)s && r.r[1] <= (intptr_t)(s + len))
            guess = (int)(r.r[1] - (intptr_t)s);
    }
    if (guess >= 0 && width_n(f, s, guess) <= width)
        return guess;
    while (lo < hi) {               /* the most that fit: lo fits, hi + 1 doesn't */
        int mid = (lo + hi + 1) / 2;
        if (width_n(f, s, mid) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

int draw_height(int f)
{
    return f == D_HERO && font[f] ? 80 : f == D_TITLE && font[f] ? 56 : f == D_HEAD && font[f] ? 46 : 36;
}

void draw_fit(int f, char *s, int width)
{
    int n = (int)strlen(s), k;
#ifdef MATINEE_TEST
    draw_test_fits++;
#endif
    if (width_n(f, s, n) <= width)
        return;
    if (f >= 0 && f < D_FONTS && !dots_w[f])
        dots_w[f] = width_n(f, "...", 3);
    if (dots_w[f] > width) {
        s[0] = 0;                   /* not even "..." fits */
        return;
    }
    k = fit_len(f, s, n, width - dots_w[f]);
    while (k > 0 && s[k - 1] == ' ')
        k--;
    if (k + 4 > n + 1) {            /* (can't happen: "..." is wider than what it replaces) */
        s[0] = 0;
        return;
    }
    strcpy(s + k, "...");
}

int draw_wrap(int f, const char *s, int width, char lines[][160], int max)
{
    int n = 0;
#ifdef MATINEE_TEST
    draw_test_wraps++;
#endif
    while (*s && n < max) {
        size_t best, len;
        char t[160];
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        /* the most whole words that fit (at most a line's worth of characters) */
        len = strlen(s);
        if (len > sizeof(t) - 1)
            len = sizeof(t) - 1;
        best = (size_t)fit_len(f, s, (int)len, width);
        if (best < strlen(s) && s[best] != ' ') {   /* back to the end of the last whole word */
            size_t b = best;
            while (b > 0 && s[b] != ' ')
                b--;
            while (b > 0 && s[b - 1] == ' ')
                b--;
            best = b;
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
