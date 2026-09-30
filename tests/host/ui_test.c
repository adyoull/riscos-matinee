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
static int pointer_w = -1, pointer_i = -1;
static int poll_null_mask_bad;

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

static char *icon_buffer(int w, int i) { return (char *)(intptr_t)win(w)->icon[i].data[0]; }

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
        out->r[0] = 1;
        return NULL;
    }
    case 0x400CA: out->r[0] = 0; return NULL;       /* Wimp_GetRectangle */
    case 0x400E2: {                                 /* Wimp_PlotIcon */
        const icon_t *ic = (const icon_t *)(intptr_t)in->r[1];
        plots++;
        if ((ic->flags & 3) == 2 && (ic->flags & 0x100)) {
            plot_sprites++;
            plotted_area = (int *)(intptr_t)ic->data[1];
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
    case 0x400F9: out->r[0] = (long)strlen((const char *)(intptr_t)in->r[1]) * 16; return NULL;  /* Wimp_TextOp 1 */
    case 0x406C0: hourglass_depth++; return NULL;
    case 0x406C1: if (--hourglass_depth < 0) hourglass_bad = 1; return NULL;
    case 0x400D4:                                   /* Wimp_CreateMenu */
        if (in->r[1] == -1) { menu_open = NULL; menus_closed++; }
        else menu_open = (int *)(intptr_t)in->r[1];
        if (menu_open && !strncmp(menu_text(menu_open, 0), "Info", 4))
            info_sub = menu_sub(menu_open, 0);
        return NULL;
    case 0x400CF: {                                 /* Wimp_GetPointerInfo */
        int *b = (int *)(intptr_t)in->r[1];
        b[0] = 640; b[1] = 480; b[2] = 4; b[3] = pointer_w; b[4] = pointer_i;
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
    case 0x49982: {                                 /* JPEG_PlotScaled: marks the sprite */
        int *sc = (int *)(intptr_t)in->r[3];
        jpeg_plots++;
        if (!output_sprite) { jpeg_into_sprite = 0; return NULL; }
        memcpy(jpeg_scale, sc, sizeof(jpeg_scale));
        output_sprite[11] = 0x00123456;             /* the first pixel */
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

static int w_signin, w_browser, w_save;

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

static int find_tile(const char *text)
{
    for (int i = 0; i < ui_test_items(); i++)
        if (!strcmp(ui_test_item(i, 0), text))
            return i;
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

static int pc, save_nulls, drain_n, prev_nsent, prev_started, prev_reports;
static char save_path[300], save_path2[300];

#define NULL_EVENT 0
/* hands out null events while the program wants them (at most n), then
   moves on */
#define DRAIN(n) do { if (!(mask & 1) && drain_n++ < (n)) return NULL_EVENT; drain_n = 0; pc++; } while (0)

static int script(int *b, int mask)
{
    static int wait_save;
    char want[400];
    for (;;) {
        switch (pc) {
        /* ---- start-up: sign in with a code */
        case 0:
            ui_test_windows(&w_signin, &w_browser, &w_save);
            CHECK(bar_icon_made == 1 && !strcmp(bar_sprite, "!plexro"), "icon bar icon '%s'", bar_sprite);
            CHECK(proginfo_made == 1, "Info window");
            CHECK(strstr(read_file(choices), "client_id plexro-") != NULL, "a client id kept from the start");
            CHECK(mask & 1, "no null events while there's nothing to do");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);             /* Select on the icon */
        case 1:
            CHECK(win(w_signin)->open, "Select before signing in opens the sign-in window");
            CHECK(!strcmp(icon_text(w_signin, SI_CODE), "ABCD"), "the code: %s", icon_text(w_signin, SI_CODE));
            CHECK(last_poll == 0x400E1 && idle_time == fake_cs + 200, "checked every 2 s (PollIdle %d, now %d)",
                  idle_time, fake_cs);
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 2:
            CHECK(win(w_signin)->open && !win(w_browser)->open, "not signed in after the first check");
            CHECK(strstr(icon_text(w_signin, SI_STATUS), "Waiting"), "status: %s", icon_text(w_signin, SI_STATUS));
            fake_cs = idle_time;
            pc++;
            return NULL_EVENT;
        case 3:
            CHECK(!win(w_signin)->open, "the sign-in window closes when signed in");
            CHECK(win(w_browser)->open, "and the browser opens");
            CHECK(ui_test_items() == 4, "the top list: %d items", ui_test_items());
            CHECK(!strcmp(ui_test_path(), "Attic"), "where: %s", ui_test_path());
            CHECK(!strcmp(ui_test_item(1, 0), "Films") && !strcmp(ui_test_item(3, 0), "Music"), "tiles %s, %s",
                  ui_test_item(1, 0), ui_test_item(3, 0));
            snprintf(want, sizeof(want), "server_base %s\n", base);
            CHECK(strstr(read_file(choices), "account_token ACCT-TOKEN\n") && strstr(read_file(choices), want) &&
                  strstr(read_file(choices), "server_token SRV-TOKEN\n") &&
                  strstr(read_file(choices), "server_name Attic\n") &&
                  strstr(read_file(choices), "player ReelEGL\n"), "Choices after signing in:\n%s", read_file(choices));
            CHECK(!hourglass_depth && !hourglass_bad, "hourglass on and off in pairs");
            pc++;
            memset(b, 0, 64);
            b[0] = w_browser;
            return 1;                                           /* Redraw_Window_Request */
        case 4:
            CHECK(strstr(plotted_text, "Continue wa...|") && strstr(plotted_text, "Films|") &&
                  strstr(plotted_text, "TV Programmes|") && strstr(plotted_text, "Folder|"),
                  "the tiles drawn: %s", plotted_text);
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
            plot_sprites = 0; plotted_area = NULL; plotted_text[0] = 0;
            pc++;
            memset(b, 0, 64);
            b[0] = w_browser;
            return 1;
        }
        case 10:
            CHECK(plot_sprites >= 6 && plotted_area && plotted_area[1] == 1 && plotted_area[4 + 11] == 0x00123456,
                  "posters plotted from their sprites (%d)", plot_sprites);
            CHECK(plotted_area && plotted_area[4 + 4] == 115 && plotted_area[4 + 5] == 173 &&
                  plotted_area[4 + 10] == (int)(1u | 90u << 1 | 90u << 14 | 6u << 27),
                  "a 116 x 174 32bpp sprite at 90 dpi");
            CHECK(strstr(plotted_text, "Big Buck Bunny|2008|"), "titles and years: %s", plotted_text);
            pc++;
            return ev_tile(b, find_tile("Dvd Rip"), 0x400);     /* one click: selects */
        case 11:
            CHECK(ui_test_sel() == find_tile("Dvd Rip"), "a click selects");
            pc++;
            return ev_tile(b, find_tile("Dvd Rip"), 2);         /* Menu */
        case 12:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_SAVE), "Save original file") &&
                  menu_sub(menu_open, MI_SAVE) == -1 && !menu_shaded(menu_open, MI_SAVE),
                  "4.7GB: no save box");
            CHECK(menu_open && menu_shaded(menu_open, MI_RESUME), "no Resume without a place to resume from");
            prev_reports = reports;
            pc++;
            return ev_menu(b, MI_SAVE, -1);
        case 13:
            CHECK(reports == prev_reports + 1 && strstr(last_report, "4.7GB") && strstr(last_report, "can't be saved"),
                  "the 4.7GB file refused: %s", last_report);
            CHECK(!ui_test_saving(), "and not saved");
            /* ---- playing: nothing running, so ReelEGL is started */
            setenv("ReelEGL$Dir", "SDFS::Pi.$.Apps.!ReelEGL", 1);
            setenv("Reel$Dir", "SDFS::Pi.$.Apps.!Reel", 1);
            prev_started = nstarted;
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 4);
        case 14: {
            int k = (nstarted - 1) & 7;
            snprintf(want, sizeof(want), "Run <ReelEGL$Dir>.!Run %s/PlexRO/Play0", scrap);
            CHECK(nstarted == prev_started + 1 && !strcmp(started[k], want), "started: %s", started[k]);
            CHECK(started_nsrc[k] == 1 && strstr(started_src[k].url, "/video/:/transcode/universal/start.m3u8?") &&
                  strstr(started_src[k].url, "&offset=2530&") && strstr(started_src[k].url, "videoResolution=1280x720"),
                  "720p (the default): converted, from where it was left: %s", started_nsrc[k] == 1 ? started_src[k].url : "");
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
        case 15:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MB_QUALITY), "Quality") &&
                  menu_flags(menu_open, MB_DIRECT) & 1, "icon bar menu; Direct play ticked");
            CHECK(info_sub == 0x7000, "Info leads to the Info window");
            pc++;
            return ev_menu(b, MB_QUALITY, 0);                   /* 1080p */
        case 16:
            CHECK(strstr(read_file(choices), "quality 0\n") != NULL, "quality kept");
            /* ---- ReelEGL running: DataOpen to it */
            tasks[0].handle = 0x777; tasks[0].name = "ReelEGL\r";
            tasks[1].handle = 0x778; tasks[1].name = "Reel\r";
            tasks[2].handle = task; tasks[2].name = "PlexRO\r";
            ntasks = 3;
            prev_nsent = nsent;
            prev_started = nstarted;
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 4);
        case 17: {
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
        case 18:
            snprintf(want, sizeof(want), "%s/PlexRO/Play1", scrap);
            CHECK(!file_exists(want), "deleted on DataLoadAck");
            CHECK(strstr(ui_test_status(), "in ReelEGL") && strstr(ui_test_status(), "Direct play"),
                  "status: %s", ui_test_status());
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 19:
            pc++;
            return ev_menu(b, MB_PLAYER, 1);                    /* Reel */
        case 20:
            CHECK(strstr(read_file(choices), "player Reel\n") != NULL, "player kept");
            prev_nsent = nsent;
            prev_started = nstarted;
            pc++;
            return ev_tile(b, find_tile("Hevc Film"), 4);
        case 21: {
            const sent_t *s = last_sent(5);
            CHECK(s && nsent == prev_nsent + 1 && s->to == 0x778, "DataOpen to Reel, not ReelEGL");
            CHECK(s && s->nsrc == 1 && strstr(s->src.url, "start.m3u8") && !s->src.key, "HEVC converted, no key");
            pc++;
            memcpy(b, s ? s->b : b, 256);
            return 19;                                          /* nobody claimed it: it comes back */
        }
        case 22: {
            int k = (nstarted - 1) & 7;
            CHECK(nstarted == prev_started + 1 && !strncmp(started[k], "Run <Reel$Dir>.!Run ", 20),
                  "unclaimed: Reel started: %s", started[k]);
            CHECK(started_nsrc[k] == 1 && strstr(started_src[k].url, "start.m3u8"), "Reel reads the same file");
            /* not running, and never seen by the Filer */
            ntasks = 1;
            tasks[0].handle = task; tasks[0].name = "PlexRO\r";
            unsetenv("Reel$Dir");
            prev_reports = reports;
            prev_started = nstarted;
            pc++;
            return ev_tile(b, find_tile("Hevc Film"), 4);
        }
        case 23:
            CHECK(reports == prev_reports + 1 && strstr(last_report, "Reel hasn't been seen by the Filer"),
                  "no Reel$Dir: %s", last_report);
            CHECK(nstarted == prev_started, "nothing started");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 24:
            pc++;
            return ev_menu(b, MB_PLAYER, 0);                    /* back to ReelEGL */
        case 25:
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);  /* Menu on it */
        case 26:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_RESUME), "Resume from 42:10") &&
                  !menu_shaded(menu_open, MI_RESUME), "Resume from: %s", menu_open ? menu_text(menu_open, MI_RESUME) : "");
            CHECK(menu_open && menu_sub(menu_open, MI_SAVE) == w_save, "the save box is Save's submenu");
            prev_started = nstarted;
            pc++;
            return ev_menu(b, MI_RESUME, -1);
        case 27: {
            int k = (nstarted - 1) & 7;
            CHECK(nstarted == prev_started + 1 && started_nsrc[k] == 1 && strstr(started_src[k].url, "start.m3u8") &&
                  strstr(started_src[k].url, "&offset=2530&") && strstr(started_src[k].url, "directStream=1"),
                  "Resume: the server streams from 42:10 (direct play off)");
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        }
        case 28:
            prev_started = nstarted;
            pc++;
            return ev_menu(b, MI_START, -1);
        case 29: {
            int k = (nstarted - 1) & 7;
            snprintf(want, sizeof(want), "%s/library/parts/11/101/file.mp4", base);
            CHECK(nstarted == prev_started + 1 && started_nsrc[k] == 1 && !strcmp(started_src[k].url, want) &&
                  !started_src[k].key, "Play from start: direct, without the carry-on key");
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        }
        case 30:
            pc++;
            return ev_menu(b, MI_WATCHED, -1);
        case 31:
            CHECK(log_count("/:/scrobble", "key", "101") == 1, "marked watched on the server");
            CHECK(strstr(ui_test_status(), "watched"), "status: %s", ui_test_status());
            /* ---- Save original file */
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        case 32:
            CHECK(!strcmp(icon_text(w_save, SV_NAME), "Big\xa0" "Buck\xa0" "Bunny\xa0(2008)/mp4"), "save as: %s",
                  icon_text(w_save, SV_NAME));
            CHECK(!strcmp(icon_text(w_save, SV_FILE), "file_bf4"), "file icon %s", icon_text(w_save, SV_FILE));
            win(w_save)->open = 1;          /* the Wimp opens the submenu */
            pc++;
            return ev_click(b, w_save, SV_FILE, 700, 500, 0x40);      /* drag the icon */
        case 33:
            CHECK(drag_started == 1, "DragASprite");
            pointer_w = 0x9000; pointer_i = 3;                       /* dropped on a Filer window */
            prev_nsent = nsent;
            pc++;
            return 7;                                                /* User_Drag_Box */
        case 34: {
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
        case 35:
            CHECK(ui_test_saving(), "saving");
            CHECK(menus_closed == 1, "the menu closed");
            CHECK(!(mask & 1), "null events while saving");
            save_nulls = 0;
            pc++;
            continue;
        case 36:
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
                return NULL_EVENT;
            }
            pc++;
            continue;
        case 37: {
            long len;
            unsigned sum = file_sum(save_path, &len);
            CHECK(!poll_null_mask_bad, "nulls all the way");
            CHECK(save_nulls == 20, "256KB a null event: %d nulls for 5MB", save_nulls);
            CHECK(len == 5 * 1024 * 1024 && sum == 0x2d800000u, "the file saved whole: %ld bytes, %08x", len, sum);
            CHECK(ntyped && !strcmp(typed_path[(ntyped - 1) & 7], save_path) && typed_type[(ntyped - 1) & 7] == 0xBF4,
                  "typed from its extension");
            CHECK(strstr(ui_test_status(), "Saved Big Buck Bunny (5 MB)"), "status: %s", ui_test_status());
            /* again, stopped part way */
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        }
        case 38:
            pc++;
            return ev_click(b, w_save, SV_FILE, 700, 500, 0x40);
        case 39:
            pointer_w = 0x9000; pointer_i = 3;
            pc++;
            return 7;
        case 40: {
            const sent_t *s = last_sent(1);
            snprintf(save_path2, sizeof(save_path2), "%s/saved/bbb2.mp4", outdir);
            ev_msg(b, 2, s ? s->b[2] : 0, 0x400);
            b[9] = 1234;
            snprintf((char *)&b[11], 200, "%s", save_path2);
            wait_save = 0;
            pc++;
            return 17;
        }
        case 41:
            if (wait_save++ < 2)
                return NULL_EVENT;
            CHECK(ui_test_saving() && file_exists(save_path2), "saving the second");
            pc++;
            return ev_tile(b, find_tile("Big Buck Bunny"), 2);
        case 42:
            CHECK(menu_open && !strcmp(menu_text(menu_open, MI_SAVE), "Stop saving"), "Stop saving on the menu");
            pc++;
            return ev_menu(b, MI_SAVE, -1);
        case 43:
            CHECK(!ui_test_saving() && !file_exists(save_path2), "stopped, the half file deleted");
            CHECK(strstr(ui_test_status(), "Stopped saving"), "status: %s", ui_test_status());
            CHECK(mask & 1, "no nulls once stopped");
            /* ---- TV: show, season, episodes, Back */
            pc++;
            return ev_key(b, w_browser, -1, 8);                 /* Backspace */
        case 44:
            CHECK(ui_test_items() == 4 && !strcmp(ui_test_path(), "Attic") && ui_test_sel() == 1,
                  "Back: the top again, Films selected (%d, %s)", ui_test_items(), ui_test_path());
            pc++;
            return ev_tile(b, 2, 4);
        case 45:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_item(0, 1), "2 seasons"), "a show");
            pc++;
            return ev_key(b, w_browser, -1, 13);                /* Return opens the one selected */
        case 46:
            CHECK(ui_test_items() == 1 && !strcmp(ui_test_path(), "Attic > TV Programmes > Space Show"),
                  "seasons: %s", ui_test_path());
            pc++;
            return ev_tile(b, 0, 4);
        case 47:
            CHECK(ui_test_items() == 6 && !strcmp(ui_test_item(2, 0), "Episode '3'") &&
                  !strcmp(ui_test_item(2, 1), "S1 E3"), "episodes, Latin-1: %s / %s", ui_test_item(2, 0),
                  ui_test_item(2, 1));
            pc++;
            return ev_key(b, w_browser, -1, 0x18D);             /* Right */
        case 48:
            CHECK(ui_test_sel() == 1, "Right moves the selection");
            pc++;
            return ev_key(b, w_browser, -1, 0x18E);             /* Down (4 a row) */
        case 49:
            CHECK(ui_test_sel() == 5, "Down: a row on (%d)", ui_test_sel());
            pc++;
            return ev_key(b, w_browser, -1, 0x1CC);             /* F12: not ours */
        case 50:
            CHECK(keys_passed == 1, "other keys passed on");
            pc++;
            return ev_key(b, w_browser, -1, 0x1B);              /* Escape */
        case 51:
            CHECK(!strcmp(ui_test_path(), "Attic > TV Programmes > Space Show"), "Escape goes back: %s",
                  ui_test_path());
            pc++;
            return ev_click(b, w_browser, BR_BACK, 0, 0, 4);    /* the Back button */
        case 52:
            CHECK(!strcmp(ui_test_path(), "Attic > TV Programmes"), "Back button: %s", ui_test_path());
            pc = 520;
            return ev_key(b, w_browser, -1, 0x7F);              /* Delete: back too */
        case 520:
            CHECK(!strcmp(ui_test_path(), "Attic"), "the top: %s", ui_test_path());
            pc = 53;
            return ev_tile(b, 3, 4);                            /* Music */
        case 53:
            CHECK(ui_test_items() == 4 && strstr(ui_test_status(), "can't be opened yet"), "music: %s",
                  ui_test_status());
            /* ---- a narrower window: fewer columns */
            memset(b, 0, 32);
            b[0] = w_browser; b[1] = 100; b[2] = 100; b[3] = 100 + 2 * 256 + 24 + 10; b[4] = 1100;
            b[5] = 0; b[6] = 0; b[7] = -1;
            pc++;
            return 2;                                           /* Open_Window_Request */
        case 54: {
            int x0, y0, x1, y1;
            ui_test_tile_xy(0, &x0, &y0);
            ui_test_tile_xy(2, &x1, &y1);
            CHECK(x0 == x1 && y1 < y0, "two columns: the third tile on the second row");
            /* ---- mode change: posters made again for the new mode */
            ev_msg(b, 0x400C1, 0, 0);
            b[0] = 20;
            pc++;
            return 17;
        }
        case 55: {
            int failed;
            CHECK(ui_test_posters(&failed) == 0, "posters dropped on a mode change");
            pc++;
            continue;
        }
        case 56:
            DRAIN(20);
            continue;
        case 57:
            CHECK(ui_test_posters(NULL) >= 1, "and fetched again");
            /* ---- sign out, then a server typed by hand */
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 2);
        case 58:
            pc++;
            return ev_menu(b, MB_SIGNOUT, -1);
        case 59:
            CHECK(!win(w_browser)->open, "signed out: the browser closed");
            CHECK(strstr(read_file(choices), "account_token \n") && strstr(read_file(choices), "server_token \n"),
                  "and the tokens forgotten:\n%s", read_file(choices));
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 60:
            CHECK(win(w_signin)->open, "Select: the sign-in window again");
            snprintf(icon_buffer(w_signin, SI_ADDR), 256, "127.0.0.1:%s", base + 17);
            snprintf(icon_buffer(w_signin, SI_TOK), 256, "WRONG");
            pc++;
            return ev_click(b, w_signin, SI_USE, 0, 0, 4);
        case 61:
            CHECK(win(w_signin)->open && strstr(icon_text(w_signin, SI_STATUS), "didn't take the token"),
                  "a wrong token: %s", icon_text(w_signin, SI_STATUS));
            snprintf(icon_buffer(w_signin, SI_TOK), 256, "SRV-TOKEN");
            pc++;
            return ev_key(b, w_signin, SI_TOK, 13);             /* Return in the token */
        case 62:
            CHECK(!win(w_signin)->open && win(w_browser)->open && ui_test_items() == 4, "by hand: the browser");
            snprintf(want, sizeof(want), "server_base %s\n", base);
            CHECK(strstr(read_file(choices), want) && strstr(read_file(choices), "server_token SRV-TOKEN\n") &&
                  strstr(read_file(choices), "account_token \n"), "Choices:\n%s", read_file(choices));
            pc++;
            return ev_msg(b, 0, 0, 0);                          /* Message_Quit */
        /* ---- the second run: Choices remembered */
        case 100:
            CHECK(!win(w_signin)->open && !win(w_browser)->open, "second run: nothing open yet");
            pc++;
            return ev_click(b, -2, 3, 1000, 20, 4);
        case 101:
            CHECK(win(w_browser)->open && ui_test_items() == 4 && !win(w_signin)->open,
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
