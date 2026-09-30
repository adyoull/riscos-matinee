/*
 * draw.h - PlexRO's drawing: filled shapes in true colour and text in
 * outline fonts, for the dark browser and details windows. Everything is
 * in screen OS units, inside a Wimp redraw loop.
 *
 *   colours: palette entries, &BBGGRR00 (RGB(r, g, b) makes one), set with
 *            ColourTrans, so they come out right in any screen mode
 *   shapes:  OS_Plot rectangle, circle and triangle fills
 *   text:    D_BODY the desktop font (Wimp_TextOp); D_BOLD and D_TITLE
 *            Homerton Bold at 12 and 20 point (Font_Paint), or the desktop
 *            font if the Font Manager can't find it
 * Part of riscos-plex. GPL v2 or later.
 */
#ifndef PLEXRO_DRAW_H
#define PLEXRO_DRAW_H
#include <stddef.h>

#define RGB(r, g, b) ((unsigned)(b) << 24 | (unsigned)(g) << 16 | (unsigned)(r) << 8)

/* The dark look */
#define C_BG      RGB(24, 26, 31)       /* the window */
#define C_HEADER  RGB(15, 16, 20)       /* the bar at the top */
#define C_CARD    RGB(52, 56, 66)       /* buttons, and posters not there yet */
#define C_CARD_HI RGB(72, 78, 90)
#define C_SHADOW  RGB(8, 8, 10)
#define C_TEXT    RGB(240, 241, 244)
#define C_SUB     RGB(150, 156, 168)
#define C_ACCENT  RGB(70, 150, 235)     /* selection, progress, Play */
#define C_HOVER   RGB(120, 128, 142)

enum { D_BODY, D_BOLD, D_TITLE, D_FONTS };

/* Glyphs, drawn smooth in a box */
enum { G_PLAY, G_BACK, G_REFRESH, G_DOWN, G_CIRCLE, G_CORNER_TL, G_CORNER_TR, G_CORNER_BL, G_CORNER_BR };

void draw_init(int xeig, int yeig);     /* the fonts and the screen (again after a mode change) */
void draw_done(void);                   /* lets the fonts and the shape sprites go */

/* The work area origin, on the screen, of the window being redrawn: the
   smooth shapes are sprites, which Wimp_PlotIcon places in work area
   coordinates */
void draw_origin(int ox, int oy);

void draw_rect(int x0, int y0, int x1, int y1, unsigned c);
void draw_tri(int x0, int y0, int x1, int y1, int x2, int y2, unsigned c);
/* A rounded rectangle, its corners smoothed against bg (the colour
   around it) */
void draw_round(int x0, int y0, int x1, int y1, int r, unsigned c, unsigned bg);
/* A glyph filling the box, smoothed against bg */
void draw_glyph(int g, int x0, int y0, int x1, int y1, unsigned c, unsigned bg);

/* Text at x, with its baseline at y; bg: the colour behind it (for the
   anti-aliasing) */
void draw_text(int font, int x, int y, const char *s, unsigned fg, unsigned bg);
int draw_width(int font, const char *s);     /* OS units */
int draw_height(int font);                   /* a line's height, OS units */
/* Cuts s so it fits in width, ending "..." if it was cut */
void draw_fit(int font, char *s, int width);
/* Breaks s into lines of at most width; returns how many (up to max) */
int draw_wrap(int font, const char *s, int width, char lines[][160], int max);

#endif
