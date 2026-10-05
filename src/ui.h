/*
 * ui.h - Matinee's desktop front end (ui.c): the icon numbers of its
 * windows, and what the host test (tests/host/ui_test.c) looks at.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_UI_H
#define MATINEE_UI_H

/* What the window shows */
enum { PG_GRID, PG_DETAILS, PG_SIGNIN, PG_PLAYER };

/* The sign-in page's buttons and fields (drawn, not icons): Plex's, then
   Jellyfin's (its address, Quick Connect, name, password, Sign in); the
   first page's choice of kind (S_PICK_*); Back (to that choice) and Cancel */
enum { S_NEWCODE = 200, S_USE, S_ADDR, S_TOK, S_QC, S_JADDR, S_JUSER, S_JPW, S_LOGIN, S_PICK_PLEX, S_PICK_JF,
       S_BACK, S_CANCEL };
/* What the sign-in page shows: the choice of kind, Plex's sign-in, Jellyfin's */
enum { SI_PICK, SI_PLEX, SI_JF };

/* The browser's and details window's buttons (drawn, not icons) */
enum { B_BACK = 100, B_REFRESH, D_PLAY, D_RESUME, D_START, D_SAVE, D_WATCHED, D_SUBS, B_SEARCH };
/* the details page's stars (your rating): D_STAR + 0..4 */
enum { D_STAR = 140 };
/* the show page's buttons, and its series' tabs (SH_TAB + n) */
enum { SH_PLAY = 120, SH_WATCHED, SH_TAB = 200 };
/* the home page's featured part (Resume or Play, Details, its dots: HB_DOT + n)
   and its rows' titles (HB_ROW + n: the whole list) */
enum { HB_PLAY = 130, HB_DETAILS, HB_DOT = 210, HB_ROW = 220 };
/* the tabs under the bar: TB_TAB (Home), TB_TAB + n (library n) */
enum { TB_TAB = 300 };
/* a library's bar: Library, Collections, Playlists (LB_VIEW + n), Sort, Unwatched; A-Z (LB_AZ + k, 0 #) */
enum { LB_VIEW = 400, LB_SORT = 410, LB_UNWATCHED = 411, LB_AZ = 420 };

/* The save box's icons */
enum { SV_FILE, SV_NAME, SV_OK, SV_COUNT };

/* The icon bar menu's items, and the item menu's */
enum { MB_INFO, MB_SIGNIN, MB_SERVERS, MB_USERS, MB_PLAYER, MB_QUALITY, MB_SIZE, MB_DIRECT, MB_CACHE, MB_SIGNOUT, MB_QUIT,
       MB_COUNT };
enum { MI_PLAY, MI_DETAILS, MI_RESUME, MI_START, MI_SUBS, MI_SAVE, MI_SPEED, MI_WATCHED, MI_UNWATCHED, MI_RATE, MI_REMOVE,
       MI_BACK, MI_REFRESH, MI_COUNT };

/* Players */
enum { PLAYER_BUILTIN, PLAYER_REELEGL, PLAYER_REEL, PLAYER_COUNT };

/* The built-in player's menu (Menu over the picture) */
enum { MP_AUDIO, MP_SUBS, MP_CHAPTERS, MP_VOLUME, MP_PICTURE, MP_STATS, MP_FULL, MP_MINI, MP_ONTOP, MP_OVERLAY, MP_STOP, MP_COUNT };

int matinee_main(int argc, char **argv);

#ifdef MATINEE_TEST
/* The host test's view of the state */
int ui_test_tile_xy(int i, int *x, int *y);     /* a tile's middle, screen OS units; 0 = ok */
int ui_test_items(void);                        /* items in the list shown */
const char *ui_test_item(int i, int line);      /* a tile's text (line 0 or 1), as shown */
const char *ui_test_status(void);
const char *ui_test_path(void);
int ui_test_saving(void);
int ui_test_speed(void);                       /* a speed test running */
int ui_test_sel(void);
int ui_test_posters(int *failed);               /* posters made (and failed) */
int ui_test_windows(int *browser, int *save);   /* 1 if the browser window is open */
int ui_test_page(void);
const char *ui_test_signin(int what);           /* 0 the code, 1 status, 2 address, 3 token; Jellyfin's:
                                                   4 its code, 5 address, 6 name, 7 the server's name */
const char *ui_test_rel(int k, int *n);         /* a details page row's heading (0 similar, 1 extras,
                                                   2 the collection) and its videos shown */
int ui_test_signin_jf(void);                    /* the sign-in page is Jellyfin's */
int ui_test_signin_mode(void);                  /* SI_*, or -1 if it isn't the sign-in page */
int ui_test_jf_servers(void);                   /* Jellyfin servers saved */
int ui_test_field(void);
int ui_test_button_xy(int w, int id, int *x, int *y);   /* a drawn button's middle */
const char *ui_test_button(int id);             /* a details button's label, or NULL */
int ui_test_hover(void);
int ui_test_player(void);                      /* the built-in player is playing (or has the card up) */
int ui_test_upnext(void);
const char *ui_test_query(void);               /* the search field's text ("": not searching) */                      /* the Up next card is counting down */
const char *ui_test_det(int what);              /* 0 title, 1 year etc., 2 how it plays, 3 summary */
int ui_test_psize(void);
int ui_test_badge(int i);                       /* item i's badge: 1 a tick, 2 a count, 0 none */
int ui_test_show(int *season);
int ui_test_tab(int *current);
int ui_test_az_letter(const char *title);       /* the A-Z strip's letter for a title (1 A .. 26 Z, 0 #) */
int ui_test_lib(int *view, int *sort, int *unwatched);   /* a library's bar shows; what it says */                  /* how many tabs (Home and the libraries); the current one */
int ui_test_home(int *rows, int *pick);         /* the home page is shown: its rows, the one featured */
const char *ui_test_home_row(int r, int *start, int *n, int *vis);   /* a row's title, items, shown */                  /* the show page is shown, and which series */
const char *ui_test_show_text(int what);
const char *ui_test_user(int *nusers);          /* who's watching (Plex Home), and how many there are */        /* 0 the title, 1 the episodes' count, 2 the buttons */
#endif

#endif
