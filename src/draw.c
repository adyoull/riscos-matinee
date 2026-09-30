/*
 * draw.c - shapes and text for PlexRO's windows (see draw.h).
 * Part of riscos-plex. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
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

static int font[D_FONTS];           /* Font Manager handles; 0 = the desktop font */

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

void draw_init(void)
{
    draw_done();
    font[D_BOLD] = find("Homerton.Bold", 12);
    font[D_TITLE] = find("Homerton.Bold", 20);
}

void draw_done(void)
{
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

void draw_circle(int x, int y, int r, unsigned c)
{
    colour(c);
    plot(4, x, y);
    plot(157, x + r, y);            /* circle fill: a point on its edge */
}

void draw_round(int x0, int y0, int x1, int y1, int r, unsigned c)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    if (r * 2 > x1 - x0)
        r = (x1 - x0) / 2;
    if (r * 2 > y1 - y0)
        r = (y1 - y0) / 2;
    if (r < 2) {
        draw_rect(x0, y0, x1, y1, c);
        return;
    }
    colour(c);
    plot(4, x0 + r, y0);
    plot(101, x1 - r - 1, y1 - 1);
    plot(4, x0, y0 + r);
    plot(101, x1 - 1, y1 - r - 1);
    plot(4, x0 + r, y0 + r);         plot(157, x0 + 2 * r, y0 + r);
    plot(4, x1 - r - 1, y0 + r);     plot(157, x1 - 1, y0 + r);
    plot(4, x0 + r, y1 - r - 1);     plot(157, x0 + 2 * r, y1 - r - 1);
    plot(4, x1 - r - 1, y1 - r - 1); plot(157, x1 - 1, y1 - r - 1);
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
