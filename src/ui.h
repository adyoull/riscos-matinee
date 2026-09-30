/*
 * ui.h - PlexRO's desktop front end (ui.c): the icon numbers of its
 * windows, and what the host test (tests/host/ui_test.c) looks at.
 * Part of riscos-plex. GPL v2 or later.
 */
#ifndef PLEXRO_UI_H
#define PLEXRO_UI_H

/* The sign-in window's icons */
enum {
    SI_CODE, SI_LINK, SI_STATUS, SI_NEWCODE, SI_OR,
    SI_ADDRL, SI_ADDR, SI_TOKL, SI_TOK, SI_USE, SI_COUNT
};

/* The browser window's header icons (the posters are plotted, not icons) */
enum { BR_BACK, BR_PATH, BR_STATUS, BR_COUNT };

/* The save box's icons */
enum { SV_FILE, SV_NAME, SV_OK, SV_COUNT };

/* The icon bar menu's items, and the item menu's */
enum { MB_INFO, MB_SIGNIN, MB_SERVERS, MB_PLAYER, MB_QUALITY, MB_DIRECT, MB_SIGNOUT, MB_QUIT, MB_COUNT };
enum { MI_PLAY, MI_RESUME, MI_START, MI_SAVE, MI_WATCHED, MI_UNWATCHED, MI_BACK, MI_REFRESH, MI_COUNT };

/* Players */
enum { PLAYER_REELEGL, PLAYER_REEL };

int plexro_main(int argc, char **argv);

#ifdef PLEXRO_TEST
/* The host test's view of the state */
int ui_test_tile_xy(int i, int *x, int *y);     /* a tile's middle, screen OS units; 0 = ok */
int ui_test_items(void);                        /* items in the list shown */
const char *ui_test_item(int i, int line);      /* a tile's text (line 0 or 1), as shown */
const char *ui_test_status(void);
const char *ui_test_path(void);
int ui_test_saving(void);
int ui_test_sel(void);
int ui_test_posters(int *failed);               /* posters made (and failed) */
int ui_test_windows(int *signin, int *browser, int *save);
#endif

#endif
