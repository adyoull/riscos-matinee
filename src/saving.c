/*
 * saving.c - saving the original file (the save box, dragging its icon, a piece
 * each null event), and the speed test.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- saving the original file ---------------------------------------------------- */

void save_prepare(const plex_item *it)
{
    const char *leaf, *dot;
    char ext[16] = "";
    size_t n = 0;
    S.sv_ok = it && it->kind == PI_VIDEO && it->part_key && *it->part_key;
    if (!S.sv_ok)
        return;
    leaf = it->part_file ? strrchr(it->part_file, '/') : NULL;
    {
        const char *bs = it->part_file ? strrchr(it->part_file, '\\') : NULL;
        if (bs && (!leaf || bs > leaf))         /* a Windows server's path (Jellyfin's) */
            leaf = bs;
    }
    leaf = leaf ? leaf + 1 : it->part_file ? it->part_file : "Video";
    if ((dot = strrchr(leaf, '.')) != NULL)
        snprintf(ext, sizeof(ext), "%s", dot + 1);
    {
        char l1[300];
        latin1(leaf, l1, sizeof(l1));
        for (const char *s = l1; *s && n < sizeof(S.sv_leaf) - 1; s++) {
            char c = *s;
            if (c == '.')
                c = '/';            /* "film.mkv" is "film/mkv" on RISC OS */
            else if (c == ' ')
                c = (char)0xA0;     /* hard space */
            else if (strchr(":*#$&@^%\\|\"<>", c))
                c = '_';
            S.sv_leaf[n++] = c;
        }
        S.sv_leaf[n] = 0;
    }
    snprintf(S.sv_name, sizeof(S.sv_name), "%s", S.sv_leaf);
    snprintf(S.sv_url_key, sizeof(S.sv_url_key), "%s", it->part_key);
    latin1(it->title, S.sv_title, sizeof(S.sv_title));
    S.sv_size = it->part_size;
    S.sv_type = *ext ? mime_type(3, ext, 0xFFD) : 0xFFD;
    snprintf(sv_sprite, sizeof(sv_sprite), "file_%03x", S.sv_type);
}

int too_big(int64_t size, const char *title)
{
    if (size <= FILE_MAX)
        return 0;
    report("%s is %.1fGB. RISC OS's filing systems hold files of up to 4GB, so it can't be saved.",
           title, size / 1e9);
    return 1;
}

void save_stop(const char *why)
{
    if (!S.save.active)
        return;
    net_close(S.save.ns);
    if (S.save.f)
        fclose(S.save.f);
    remove(S.save.path);            /* no half files left behind */
    S.save.active = 0;
    S.save.ns = NULL;
    S.save.f = NULL;
    if (why)
        set_status("%s", why);
}

/* A file's address: a key (a path on the server), or an address of its
   own (a DLNA server's files) */
void media_url(const char *key, char *url, size_t size)
{
    if (!strncmp(key, "http://", 7) || !strncmp(key, "https://", 8))
        snprintf(url, size, "%s", key);
    else
        snprintf(url, size, "%s%s", S.px.base, key);
}

void save_start(const char *path)
{
    char url[1100], headers[1024], err[256];
    int64_t size;
    if (S.save.active) {
        report("Already saving %s: stop that first (on the menu).", S.save.title);
        return;
    }
    if (S.speed.active) {
        report("Wait until the speed test has finished.");
        return;
    }
    if (too_big(S.sv_size, S.sv_title))
        return;
    media_url(S.sv_url_key, url, sizeof(url));
    plex_headers(&S.px, S.px.token, headers, sizeof(headers));
    hourglass(1);
    S.save.ns = net_open(url, headers, 30000, err, sizeof(err));
    hourglass(0);
    if (!S.save.ns) {
        report("Can't get %s: %s", S.sv_title, err);
        return;
    }
    size = net_size(S.save.ns);
    if (too_big(size, S.sv_title)) {
        net_close(S.save.ns);
        S.save.ns = NULL;
        return;
    }
    if (!(S.save.f = fopen(path, "wb"))) {
        report("Can't write %s: %s", path, strerror(errno));
        net_close(S.save.ns);
        S.save.ns = NULL;
        return;
    }
    S.save.active = 1;
    S.save.size = size > 0 ? size : S.sv_size;
    S.save.done = 0;
    S.save.t0 = now_cs();
    S.save.type = S.sv_type;
    snprintf(S.save.path, sizeof(S.save.path), "%s", path);
    snprintf(S.save.title, sizeof(S.save.title), "%s", S.sv_title);
    set_status("Saving %s...", S.save.title);
}

static void save_fail(const char *fmt, const char *arg)
{
    char why[400];
    snprintf(why, sizeof(why), fmt, S.save.title, arg ? arg : "");
    save_stop(NULL);
    report("%s", why);
}

/* A piece of the file (SAVE_STEP bytes): one null event's work */
void save_step(void)
{
    static char buf[64 * 1024];
    int total = 0, end = 0;
    while (total < SAVE_STEP) {
        int want = (int)sizeof(buf), got;
        if (S.save.size > 0 && S.save.size - S.save.done < want)
            want = (int)(S.save.size - S.save.done);
        if (want <= 0) {            /* all of it */
            end = 1;
            break;
        }
        got = net_read(S.save.ns, buf, want);
        if (got < 0) {
            save_fail("Saving %s stopped: the server stopped sending.%s", NULL);
            return;
        }
        if (got == 0) {
            end = 1;
            break;
        }
        if (fwrite(buf, 1, (size_t)got, S.save.f) != (size_t)got) {
            save_fail("Can't write %s: %s", strerror(errno));
            return;
        }
        S.save.done += got;
        total += got;
    }
    if (S.save.size > 0 && S.save.done >= S.save.size)
        end = 1;
    if (!end) {
        char rate[40] = "";
        int cs = now_cs() - S.save.t0;
        if (cs >= 100)              /* how fast, once there's a second to go on */
            snprintf(rate, sizeof(rate), ", %.1f MB/s", S.save.done / 1048576.0 * 100 / cs);
        if (S.save.size > 0)
            set_status("Saving %s: %.0f of %.0f MB (%d%%)%s", S.save.title, S.save.done / 1048576.0,
                       S.save.size / 1048576.0, (int)(S.save.done * 100 / S.save.size), rate);
        else
            set_status("Saving %s: %.0f MB%s", S.save.title, S.save.done / 1048576.0, rate);
        return;
    }
    if (S.save.size > 0 && S.save.done != S.save.size) {
        save_fail("Saving %s stopped: the file ended early.%s", NULL);
        return;
    }
    net_close(S.save.ns);
    S.save.ns = NULL;
    if (fclose(S.save.f) != 0) {
        S.save.f = NULL;
        save_fail("Couldn't finish writing %s: is the disc full?%s", NULL);
        return;
    }
    S.save.f = NULL;
    set_type(S.save.path, S.save.type);
    S.save.active = 0;
    set_status("Saved %s (%.0f MB).", S.save.title, S.save.done / 1048576.0);
}

/* ---- the speed test ------------------------------------------------------------ */

/* Reads the start of a video's file from the server for up to SPEED_TIME,
   a piece each null event (as saving does), and says how fast it came and
   whether that's enough to play the file directly. It tells a slow server
   or network from a player that doesn't read fast enough. */
#define SPEED_TIME 800              /* centiseconds */
#define SPEED_MAX (64 * 1024 * 1024)

void speed_close(void)
{
    net_close(S.speed.ns);
    S.speed.ns = NULL;
    S.speed.active = 0;
}

void speed_start(const plex_item *it)
{
    char url[1100], headers[1024], err[256];
    int t;
    if (S.save.active || S.speed.active) {
        report("Wait until the file being saved, or the speed test, has finished.");
        return;
    }
    if (!it || it->kind != PI_VIDEO || !it->part_key || !*it->part_key)
        return;
    media_url(it->part_key, url, sizeof(url));
    plex_headers(&S.px, S.px.token, headers, sizeof(headers));
    latin1(it->title, S.speed.title, sizeof(S.speed.title));
    hourglass(1);
    t = now_cs();
    S.speed.ns = net_open(url, headers, 30000, err, sizeof(err));
    S.speed.start_cs = now_cs() - t;
    hourglass(0);
    if (!S.speed.ns) {
        report("Speed test: can't get %s from the server: %s", S.speed.title, err);
        return;
    }
    S.speed.active = 1;
    S.speed.done = 0;
    S.speed.kbps = it->bitrate_kbps;
    S.speed.t0 = now_cs();
    set_status("Testing the speed: reading %s from the server...", S.speed.title);
}

static void speed_done(const char *broke)
{
    int cs = now_cs() - S.speed.t0;
    double mbit = S.speed.done * 8.0 / (cs > 0 ? cs : 1) * 100 / 1e6;
    int https = !strncmp(S.px.base, "https:", 6);
    char how[160], verdict[120] = "";
    speed_close();
    /* all of it must fit in an error block (252 characters) */
    snprintf(how, sizeof(how), "%.1f MB in %.1f s: %.1f Mbit/s, over %s (%s; answered in %.1f s).",
             S.speed.done / 1048576.0, cs / 100.0, mbit, https ? "https" : "http",
             S.px.local ? "local" : "not local", S.speed.start_cs / 100.0);
    if (S.speed.kbps > 0) {
        double need = S.speed.kbps / 1000.0, times = mbit / need;
        if (times >= 2)
            snprintf(verdict, sizeof(verdict), " It needs %.1f Mbit/s: plenty. If the player runs short, it isn't reading "
                     "fast enough.", need);
        else if (times >= 1.2)
            snprintf(verdict, sizeof(verdict), " It needs %.1f Mbit/s: just enough to play directly.", need);
        else
            snprintf(verdict, sizeof(verdict), " It needs %.1f Mbit/s: too slow to play directly. Untick Direct play, "
                     "or save the file first.", need);
    }
    set_status("Speed test: %.1f Mbit/s from the server (%s)", mbit, https ? "https" : "http");
    report("Speed test, %.40s: %s%s%s", S.speed.title, how, verdict, broke ? " It stopped part way." : "");
}

/* A piece of the file (SAVE_STEP bytes): one null event's work */
void speed_step(void)
{
    static char buf[64 * 1024];
    int total = 0;
    while (total < SAVE_STEP) {
        int got = net_read(S.speed.ns, buf, sizeof(buf));
        if (got <= 0) {
            speed_done(got < 0 ? "broke" : NULL);
            return;
        }
        S.speed.done += got;
        total += got;
    }
    if (now_cs() - S.speed.t0 >= SPEED_TIME || S.speed.done >= SPEED_MAX) {
        speed_done(NULL);
        return;
    }
    set_status("Testing the speed: %.1f MB so far...", S.speed.done / 1048576.0);
}

void close_menus(void)
{
    _kernel_swi_regs r;
    r.r[1] = -1;
    swi(Wimp_CreateMenu, &r);
    S.menu_kind = MK_NONE;
}

/* OK in the save box (or Return): a full path typed in saves there */
void save_ok(void)
{
    S.sv_name[strcspn(S.sv_name, "\r\n")] = 0;
    if (!strchr(S.sv_name, '.') && !strchr(S.sv_name, ':')) {
        report("To save, drag the icon to a directory display.");
        return;
    }
    close_menus();
    save_start(S.sv_name);
}

void drag_start(void)
{
    int st[9], box[10];
    _kernel_swi_regs r;
    window_state(S.save_w, st);
    /* the file icon's box on the screen */
    box[0] = st[1] - st[5] + 136; box[1] = st[4] - st[6] - 124;
    box[2] = st[1] - st[5] + 264; box[3] = st[4] - st[6] - 20;
    r.r[0] = 0x85;                  /* centred, bounded by the screen, a shadow */
    r.r[1] = 1;
    r.r[2] = (intptr_t)sv_sprite;
    r.r[3] = (intptr_t)box;
    r.r[4] = 0;
    S.dragging = 1;
    S.drag_sprite = !swi(DragASprite_Start, &r);
    if (!S.drag_sprite) {
        int b[10];
        b[0] = S.save_w; b[1] = 5;  /* a fixed-size box */
        b[2] = box[0]; b[3] = box[1]; b[4] = box[2]; b[5] = box[3];
        b[6] = -0x10000; b[7] = -0x10000; b[8] = 0x10000; b[9] = 0x10000;
        r.r[1] = (intptr_t)b;
        swi(Wimp_DragBox, &r);
    }
}

/* The icon was dropped: offer the file there (Message_DataSave) */
void drag_end(void)
{
    int p[5], b[64];
    _kernel_swi_regs r;
    const char *leaf;
    if (!S.dragging)
        return;
    S.dragging = 0;
    if (S.drag_sprite)
        swi(DragASprite_Stop, &r);
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r) || p[3] == S.save_w || p[3] < 0)
        return;
    leaf = strrchr(S.sv_name, '.');
    leaf = leaf ? leaf + 1 : S.sv_name;
    memset(b, 0, sizeof(b));
    b[4] = MSG_DATASAVE;
    b[5] = p[3]; b[6] = p[4]; b[7] = p[0]; b[8] = p[1];
    b[9] = S.sv_size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)S.sv_size;
    b[10] = S.sv_type;
    snprintf((char *)&b[11], 212, "%s", *leaf ? leaf : "Video");
    b[0] = (44 + (int)strlen((char *)&b[11]) + 1 + 3) & ~3;
    r.r[0] = 17;
    r.r[1] = (intptr_t)b;
    r.r[2] = p[3];
    r.r[3] = p[4];
    if (!swi(Wimp_SendMessage, &r))
        S.datasave_ref = b[2];
}
