/*
 * player.h - Matinee's built-in player: riscos-ffmpeg's reelcore playing
 * inside Matinee's own window, as its player page.
 *
 * The picture fills the window above a bar of controls (Back, Play/Pause,
 * back and forward 10 s, the title, the time and a position bar, Stats and
 * Full screen). Full screen is a window of its own with no furniture over
 * the whole screen, still in the desktop (other tasks carry on); the bar
 * shows there when the pointer moves.
 *
 * The picture goes through a hardware overlay (the VideoOverlay module,
 * YV12) when one can be had: the display scales it to the window or the
 * screen, so it costs almost nothing to stretch. Otherwise, or while
 * anything covers it, it is converted into a sprite and plotted, as Reel
 * does. Sound goes through SharedSoundBuffer (reelcore).
 *
 * The player knows nothing of Plex: ui.c tells the server where it's got
 * to, and offers the
 * next episode (player_card).
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_PLAYER_H
#define MATINEE_PLAYER_H

/* What player_null, player_click and player_key report */
enum {
    PE_NONE,
    PE_END,             /* played to the end */
    PE_BACK,            /* Back, Escape: the caller leaves the player page */
    PE_PAUSED,          /* paused, or playing again: tell the server */
    PE_PLAYING,
    PE_FAILED,          /* couldn't open it: player_error() says why */
    PE_READY,           /* open, and playing */
    PE_CARD_1,          /* the card's buttons (player_card) */
    PE_CARD_2,
    PE_MENU,            /* Menu over the picture: the caller opens its menu */
    PE_SUBS,            /* Subtitles on the bar: the caller opens its subtitles menu */
    PE_SKIP             /* the skip button (player_skip), or Return while it shows */
};

/* How the picture fills its area */
enum { PIC_FIT, PIC_FILL, PIC_STRETCH, PIC_COUNT };

typedef struct {
    const char *url;
    const char *headers;        /* "Name: value\r\n" lines, or NULL */
    const char *user_agent;     /* or NULL */
    const char *title;          /* Latin-1, for the bar */
    double base;                /* the video's time at the stream's start (a converted stream
                                   started part way); seconds */
    double start;               /* seek here once open (in the video; seconds) */
    double duration;            /* the video's length (seconds), or 0: the stream's */
    int convert;                /* converted by the server */
    int subs;                   /* subtitle tracks to choose from: the bar has Subtitles */
    const double *chapters;     /* where chapters start (seconds, in the video): marks on the position bar */
    int nchapters;
} player_src;

/* The task, the choices (hardware overlay on, picture mode, volume 0..1) */
void player_init(int task, int overlay, int pic_mode, double volume);

/* Starts playing src in window win's work area (its top left at work area
   0,0, not scrolled). Returns at once: opening happens in null events
   (PE_READY or PE_FAILED). 0 = started. */
int player_open(const player_src *src, int win);
/* 1 if HEVC can be decoded by the Pi 4's HEVC block (asked once) */
int player_hevc_block(void);
#ifdef MATINEE_TEST
extern int player_test_hevc_block;      /* the tests' answer */
extern int player_test_feeds;           /* times the reader was given the time between pictures */
#endif
/* Stops and lets everything go (the overlay, the full screen window) */
void player_close(void);
int player_active(void);
int player_ready(void);                 /* open (the size is known) */
const char *player_error(void);

/* After Wimp_Poll: how to poll next. -1: no null events needed (paused);
   0: null events at once; > 0: Wimp_PollIdle for that many cs */
int player_poll_cs(void);
/* A null event's work: a PE_* */
int player_null(void);

/* Redraw_Window_Request for the player's windows: the whole loop */
void player_redraw(int *b);
/* The full screen window (not the caller's) */
int player_owns(int w);
/* The window was opened or resized (after Wimp_OpenWindow) */
void player_layout(void);
void player_mode_change(void);

/* Mouse_Click (the Wimp's block) and Key_Pressed (the key): a PE_*, or -1
   for a key the player doesn't use */
int player_click(const int *b);
int player_key(int k);

/* Where it is (seconds, in the video) */
double player_position(void);
double player_duration(void);
int player_paused(void);
void player_pause(int paused);
int player_ended(void);
void player_seek(double t);             /* a converted stream too: the server converts from there */

int player_fullscreen(void);
void player_set_fullscreen(int on);

/* The mini player (as Reel's): a small window with no furniture above the
   icon bar in place of the caller's window (closed meanwhile, opened again
   where it was): the picture, Play/Pause, the position bar, Normal and a
   grip. It decodes with no deblocking on pictures nothing is predicted
   from. Keep on top brings it back to the front about once a second. */
int player_mini(void);
void player_set_mini(int on);
int player_ontop(void);
void player_set_ontop(int on);
/* Its width, gap from the screen's right edge and bottom (-1: just above
   the icon bar), OS units: kept in the caller's choices */
void player_mini_place(int *w, int *right, int *bottom);
void player_set_mini_place(int w, int right, int bottom);
/* Open_Window_Request for it (player_owns): 1 = moved or resized */
int player_mini_open_request(int *b);

/* The file's sound tracks (a converted stream has one: the server's
   choice), reelcore's numbering */
int player_tracks(void);
int player_track(void);
void player_track_name(int i, char *buf, int size);
void player_set_track(int i);

/* Subtitles drawn by reelcore (direct play): the file's tracks, then
   files added; -1 none */
int player_sub_tracks(void);
int player_sub_track(void);
int player_set_sub(int i);                  /* 0 = done */
int player_add_sub_file(const char *path);  /* the new track, or < 0 */

double player_volume(void);             /* 0..1 */
void player_set_volume(double v);

int player_stats(void);
void player_set_stats(int on);

int player_overlay(void);               /* the choice */
void player_set_overlay(int on);
int player_overlay_shown(void);         /* the picture is in an overlay now */

int player_pic_mode(void);
void player_set_pic_mode(int m);

/* A card over the picture (up next): a heading, a line, a line under it
   (the countdown) and two buttons; heading NULL takes it away */
void player_card(const char *heading, const char *line, const char *line2, const char *b1, const char *b2);
/* A button at the right of the bar's top line ("Skip intro"), or NULL to
   take it away; it reports PE_SKIP. Full screen, the bar stays while it's
   there. */
void player_skip(const char *label);
const char *player_skip_label(void);

/* A short note in the bar instead of the time, for a few seconds */
void player_note(const char *text);

#ifdef MATINEE_TEST
int player_test_button_xy(int id, int *x, int *y);   /* screen OS units; 0 = ok */
const char *player_test_time(void);
int player_test_panel_rows(void);
const char *player_test_panel(int row, int value);
int player_test_sprite_plots(void);
int player_test_idle(void);
int player_test_page_moves(void);   /* times the page at &8000 moved (should be 0) */
#endif

/* The bar's buttons (player_test_button_xy) */
enum { PB_BACK = 1, PB_PLAY, PB_REW, PB_FWD, PB_TRACK, PB_STATS, PB_FULL, PB_SUBS, PB_NORMAL, PB_GRIP, PB_CARD1, PB_CARD2, PB_SKIP, PB_COUNT };

#endif
