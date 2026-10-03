/*
 * fake_reelcore.h - what the fake reelcore (fake_reelcore.c) was asked,
 * for ui_test's checks. Part of riscos-matinee. GPL v2 or later.
 */
#ifndef FAKE_REELCORE_H
#define FAKE_REELCORE_H

#define FAKE_W   1280
#define FAKE_H   720
#define FAKE_LEN 20.0           /* seconds: a direct play file's length */

typedef struct {
    int opens, closes, open_now, async;
    char url[4096], headers[2048];
    int updates, frames, draws, draw_w, draw_h, draw_flags;
    int yuv_draws, yuv_w, yuv_h;
    int seeks;
    double seek_to;
    double volume;
    int track;
    int panel_rows, panels;
    int buffering;              /* set by the test: the network is short */
    double pos_offset;          /* set by the test: the stream counts from here */
    double stream_end;          /* set by the test: a converted stream ends here (0: never) */
    double len;                 /* set by the test: a file's length (0: FAKE_LEN) */
    double yuv_scale;           /* reelcore_set_yuv_scale: the panel drawn that much bigger */
    int sub_track;              /* reelcore_set_subtitle_track (-1 none) */
    int sub_sets;
    int sub_files;              /* files added */
    char sub_file[512];         /* the last one, and what it held */
    char sub_text[256];
    int fast;                   /* reelcore_set_fast */
    double clock_start;         /* set by the test: the clock counts from here (a stream's start_time) */
    int app_page;               /* the physical page at &8000 (the fake OS_Memory 0 gives it) */
    int open_flags;
    double ahead;               /* set by the test: seconds read ahead (0: 8) */             /* reelcore_open_source's flags */
    int hevc_block;             /* set by the test: the video is decoded by the HEVC block */
    int move_page_on_open;      /* set by the test: opening moves it (a decoder claiming contiguous memory) */
} fake_rc_t;

extern fake_rc_t fake_rc;
int fake_rc_cs(void);           /* the fake Wimp's clock (ui_test) */

#endif
