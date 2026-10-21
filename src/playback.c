/*
 * playback.c - playing a video: handing it to Reel (Message_DataOpen), or playing
 * it in the built-in player (player.c), and telling the server where it's
 * got to.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- handing a video to Reel ------------------------------------------------------ */

/* Wimp_StartTask with our VFP context switched off meanwhile (UnixLib keeps
   it on our stack; the new task's UnixLib makes its own and VFPSupport
   would otherwise save ours lazily into the wrong place), as !FFmpeg's
   front end does. One asm block, so no VFP instruction comes between. */
static _kernel_oserror *start_task(const char *cmd)
{
#if defined(__arm__) && defined(__riscos__)
    _kernel_oserror *err;
    __asm__ volatile(
        "mov    r0, #0\n\t"
        "mov    r1, #0\n\t"
        "swi    0x78EC3\n\t"          /* XVFPSupport_ChangeContext 0: off */
        "movvs  r0, #0\n\t"
        "mov    r4, r0\n\t"
        "mov    r0, %1\n\t"
        "swi    0x600DE\n\t"          /* XWimp_StartTask */
        "movvs  %0, r0\n\t"
        "movvc  %0, #0\n\t"
        "movs   r0, r4\n\t"
        "mov    r1, #0\n\t"
        "swine  0x78EC3\n\t"          /* back on */
        : "=&r"(err) : "r"(cmd) : "r0", "r1", "r2", "r3", "r4", "r14", "cc", "memory");
    return err;
#else
    _kernel_swi_regs r;
    r.r[0] = (intptr_t)cmd;
    return swi(Wimp_StartTask, &r);
#endif
}

/* The task handle of a running task called name, or 0 */
int find_task(const char *name)
{
    int buf[16 * 4];
    _kernel_swi_regs r;
    size_t n = strlen(name);
    r.r[0] = 0;
    do {
        r.r[1] = (intptr_t)buf;
        r.r[2] = sizeof(buf);
        if (swi(TaskManager_EnumerateTasks, &r))
            return 0;
        for (int *p = buf; (char *)p < (char *)(intptr_t)r.r[1]; p += 4) {
            const char *t = (const char *)(intptr_t)p[1];
            /* the name exactly: "Reel" mustn't find "ReelEGL" */
            if (p[0] != S.task && !strncmp(t, name, n) && (unsigned char)t[n] < 32)
                return p[0];
        }
    } while (r.r[0] >= 0);
    return 0;
}

void start_player(int player, const char *file)
{
    char var[32], cmd[400];
    const char *dir;
    _kernel_oserror *e;
    snprintf(var, sizeof(var), "%s$Dir", player_names[player]);
    dir = getenv(var);
    if (!dir || !*dir) {
        report("%s hasn't been seen by the Filer yet: open the directory that holds !%s, "
               "then try again (or choose the other player on the Player menu).",
               player_names[player], player_names[player]);
        remove(file);
        return;
    }
    snprintf(cmd, sizeof(cmd), "Run <%s>." "!Run %s", var, file);
    if ((e = start_task(cmd)) != NULL)
        report("Can't start %s: %s", player_names[player], e->errmess);
    else
        set_status("%s (started %s)", S.play_why, player_names[player]);
    /* the player reads the file before its first Wimp_Poll, and StartTask
       returns at that poll */
    remove(file);
}

/* The hand-off file's name: a few in turn, so one still waiting for its
   DataLoadAck isn't overwritten */
static int handoff_path(char *out, size_t size)
{
    const char *scrap = getenv("Wimp$ScrapDir");
    char dir[256];
    if (!scrap || !*scrap)
        return -1;
    snprintf(dir, sizeof(dir), "%s" SEP APP, scrap);
    make_dir(dir);
    snprintf(out, size, "%s" SEP "Play%d", dir, S.play_n);
    S.play_n = (S.play_n + 1) % 8;
    return 0;
}

static void send_to_player(const char *file, int type)
{
    int task = find_task(player_names[S.player]);
    int b[64];
    _kernel_swi_regs r;
    if (task && strlen(file) < 212) {
        memset(b, 0, sizeof(b));
        b[3] = 0;
        b[4] = MSG_DATAOPEN;
        b[5] = 0;                   /* no directory window */
        b[10] = type;
        snprintf((char *)&b[11], 256 - 44, "%s", file);
        b[0] = (44 + (int)strlen(file) + 1 + 3) & ~3;
        r.r[0] = 18;                /* recorded: it comes back if nobody claims it */
        r.r[1] = (intptr_t)b;
        r.r[2] = task;              /* to that task, not to everyone (KinoAmp would take it) */
        if (!swi(Wimp_SendMessage, &r)) {
            for (int i = 0; i < PEND_MAX; i++)
                if (!S.pend[i].ref || i == PEND_MAX - 1) {
                    if (S.pend[i].ref)
                        remove(S.pend[i].path);
                    S.pend[i].ref = b[2];
                    S.pend[i].player = S.player;
                    snprintf(S.pend[i].path, sizeof(S.pend[i].path), "%s", file);
                    break;
                }
            return;
        }
    }
    start_player(S.player, file);
}

/* it: the video as the list shows it (its title); what's played and how
   comes from its full details (the subtitles chosen, where it was left) */
static void builtin_play(const plex_item *it, int how);

void play_item(const plex_item *it, int how)
{
    static play_t p;
    caps_t k;
    char file[300], title[300];
    int allow = S.direct, resume = 1, type, e;
    plex_list tmp;
    const plex_item *full = it;
    if (S.player == PLAYER_BUILTIN) {
        builtin_play(it, how);
        return;
    }
    tmp.n = 0;
    if (det_is(it)) {
        full = det_item();
    } else {
        hourglass(1);
        if (plex_details(&S.px, it, &tmp) == 0)
            full = &tmp.v[0];
        hourglass(0);
    }
    /* Resume from: the server starts its stream there (copying the video
       when it can); Reel can't be told where to start a file of its own */
    if (how == PLAY_RESUME && S.px.kind != SRV_DLNA)
        allow = 0;                  /* (a DLNA server's converted stream can't start part way: Reel's
                                       own "carry on" for the file itself instead) */
    if (how == PLAY_START)
        resume = 0;
    caps_for(S.quality, &k);
    e = caps_play(&S.px, full, &k, allow, resume, &p);
    if (tmp.n)
        plex_list_free(&tmp);
    if (e != 0) {
        set_status("%s", p.why);
        return;
    }
    if (how == PLAY_START)
        p.key[0] = 0;               /* not where Reel last stopped, either */
    if (it->subtitle && *it->subtitle && it->type && !strcmp(it->type, "episode"))
        snprintf(title, sizeof(title), "%s (%s)", it->title, it->subtitle);
    else
        snprintf(title, sizeof(title), "%s", it->title);
    if (handoff_path(file, sizeof(file)) != 0) {
        report("<Wimp$ScrapDir> isn't set: is !Scrap missing?");
        return;
    }
    if (handoff_write(file, &p, title, S.agent) != 0) {
        report("Can't write %s", file);
        return;
    }
    /* typed as a video, so Reel claims the DataOpen; Reel looks inside and
       finds JSON */
    type = mime_type(2, "video/mp4", 0xBF4);
    set_type(file, type);
    snprintf(S.play_why, sizeof(S.play_why), "%s", p.why);
    set_status("%s", p.why);
    send_to_player(file, type);
}

/* ---- the built-in player ----------------------------------------------------------

   The video plays in the window itself (the player page), in player.c.
   Here is what Plex needs from it: the server told where playing has got
   to (Continue watching, Resume) every 10 s and when it pauses or stops; a
   converted stream started again at the new place to seek in it; the
   sound track chosen on the server for a converted stream; and the next
   episode offered at the end of one. */

#define TIMELINE_CS 1000
#define PING_CS     3000            /* paused: the server's converting kept alive */
#define UPNEXT_CS   1000

/* The reports every 10 s while a video plays go from a thread of their
   own. Each is a new connection to the server and its answer: 50 ms to a
   few hundred, and on the desktop's own time that held up the pictures (a
   stutter every 10 s). The thread has its own copies of what the request
   needs (the server, the video's ids and its chosen tracks, the play
   queue), so nothing it reads can change or be freed under it. One at a
   time; the other reports (playing at the start, paused, stopped) are sent
   as before, after waiting for it, so the server hears them in order. */
static struct {
    pthread_t th;
    int running;                /* a thread to join */
    volatile int done;
    plex_ctx c;
    plex_item it;
    plex_playing pl;
    int64_t t, d;
    int cs, result;             /* how long it took; plex_timeline's answer */
} TL;

static void *timeline_thread(void *arg)
{
    int t0 = now_cs();
    (void)arg;
    TL.result = plex_timeline(&TL.c, &TL.it, "playing", TL.t, TL.d, &TL.pl);
    TL.cs = now_cs() - t0;
    TL.done = 1;
    return NULL;
}

/* Waits for the thread's report (if any) and frees its copies */
static void timeline_wait(void)
{
    if (!TL.running)
        return;
    pthread_join(TL.th, NULL);
    TL.running = 0;
    player_log("timeline: playing at %.1f s, told the server in %d cs%s%s", TL.t / 1000.0, TL.cs,
               TL.result ? ": " : "", TL.result ? TL.c.err : "");
    free(TL.it.rating_key);
    free(TL.it.guid);
    free(TL.it.source_id);
    free(TL.it.subs);
    free(TL.it.auds);
    memset(&TL.it, 0, sizeof(TL.it));
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* 0 = the thread is sending it */
static int timeline_start(const plex_item *it, int64_t t, int64_t d)
{
    TL.c = S.px;
    TL.pl = S.pl.pq;
    TL.t = t;
    TL.d = d;
    memset(&TL.it, 0, sizeof(TL.it));
    TL.it.rating_key = dup_or_null(it->rating_key);
    TL.it.guid = dup_or_null(it->guid);
    TL.it.source_id = dup_or_null(it->source_id);
    if (it->nsubs && (TL.it.subs = malloc((size_t)it->nsubs * sizeof(*it->subs))) != NULL) {
        memcpy(TL.it.subs, it->subs, (size_t)it->nsubs * sizeof(*it->subs));
        TL.it.nsubs = it->nsubs;
    }
    if (it->nauds && (TL.it.auds = malloc((size_t)it->nauds * sizeof(*it->auds))) != NULL) {
        memcpy(TL.it.auds, it->auds, (size_t)it->nauds * sizeof(*it->auds));
        TL.it.nauds = it->nauds;
    }
    TL.done = 0;
    {
        pthread_attr_t at;
        int bad = it->rating_key && !TL.it.rating_key;
        /* its own stack: FFmpeg's HTTP and the server's answer, as the player's reader */
        if (!bad) {
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 256 << 10);
            bad = pthread_create(&TL.th, &at, timeline_thread, NULL) != 0;
            pthread_attr_destroy(&at);
        }
        if (bad) {
            free(TL.it.rating_key);
            free(TL.it.guid);
            free(TL.it.source_id);
            free(TL.it.subs);
            free(TL.it.auds);
            memset(&TL.it, 0, sizeof(TL.it));
            return -1;
        }
    }
    TL.running = 1;
    return 0;
}

/* Better still, for a server at a plain http address (on the local
   network, the usual case): the report sent and its answer read a piece at
   a time on each null event, with no thread and never waiting (bgreq.c, as
   the posters are fetched). From the thread (test1 and test2) each report
   still cost a few pictures on a Pi. Plex's reports and Jellyfin's progress
   go this way; the thread is kept for https servers. */
#ifdef MATINEE_TEST
int ui_test_free_reports;
#endif
static struct {
    bgreq r;
    int cs0;
    int64_t t;
} TR = { .r = { .fd = -1 } };

#ifdef MATINEE_TEST
/* A report on its way (without waiting)? */
int timeline_going(void)
{
    return bgreq_active(&TR.r);
}
#endif

/* Moves the report on, if one is going; called on every null event while playing */
void timeline_step(void)
{
    int e;
    if (!bgreq_active(&TR.r))
        return;
    e = bgreq_step(&TR.r);
    if (e != 0)
        player_log("timeline: playing at %.1f s, told the server in %d cs, without waiting%s", TR.t / 1000.0,
                   now_cs() - TR.cs0, e > 0 && TR.r.status == 200 ? "" : e > 0 ? " (not 200)" : " (failed)");
    else if (now_cs() - TR.cs0 > 1000) {
        bgreq_close(&TR.r);         /* no answer in 10 s: the next one will be sent anyway */
        player_log("timeline: playing at %.1f s: no answer in 10 s", TR.t / 1000.0);
    }
}

/* 0 = the report is on its way */
static int timeline_send_free(const plex_item *it, int64_t t, int64_t d)
{
    plex_request rq;
    if (plex_timeline_request(&S.px, it, "playing", t, d, &S.pl.pq, &rq) ||
        bgreq_send(&TR.r, rq.method, rq.url, rq.headers, rq.body, 0))
        return -1;
    TR.cs0 = now_cs();
    TR.t = t;
#ifdef MATINEE_TEST
    ui_test_free_reports++;
#endif
    return 0;
}

static void builtin_timeline(const char *state)
{
    const plex_item *it;
    int64_t t, d;
    if (!S.pl.on || !S.pl.det.n)
        return;
    it = &S.pl.det.v[0];
    t = (int64_t)(player_position() * 1000);
    d = (int64_t)(player_duration() * 1000);
    S.pl.tl_cs = now_cs() + TIMELINE_CS;
    if (!strcmp(state, "playing") && S.pl.tl_async && S.px.kind != SRV_DLNA) {
        if (bgreq_active(&TR.r) || (TL.running && !TL.done))
            return;                 /* the last one is still going: this one is left out */
        timeline_wait();
        if (timeline_send_free(it, t, d) == 0 || timeline_start(it, t, d) == 0)
            return;
    }
    bgreq_close(&TR.r);             /* (sent: the server has it, or will) */
    timeline_wait();
    plex_timeline(&S.px, it, state, t, d, &S.pl.pq);
}

/* The X-Plex headers, without Accept: JSON (the video isn't) */
void media_headers(const char *in, char *out, size_t size)
{
    size_t o = 0;
    while (*in && o + 1 < size) {
        const char *e = strstr(in, "\r\n");
        size_t n = e ? (size_t)(e - in) + 2 : strlen(in);
        if (strncmp(in, "Accept:", 7) && o + n < size) {
            memcpy(out + o, in, n);
            o += n;
        }
        in += n;
    }
    out[o] = 0;
}

/* The title shown in the bar: "Show · S1 E3 · Episode" or the film's */
static void builtin_title(const plex_item *it, char *out, size_t size)
{
    char t[400];
    if (it->type && !strcmp(it->type, "episode") && it->grandparent_title)
        snprintf(t, sizeof(t), "%s \xc2\xb7 S%d E%d \xc2\xb7 %s", it->grandparent_title, it->parent_index, it->index,
                 it->title);
    else
        snprintf(t, sizeof(t), "%s", it->title);
    latin1(t, out, size);
}

/* Plays S.pl.det's video from t seconds. A converted stream always starts
   at the beginning and the player seeks in it (the server's playlist
   covers the whole video, and converting starts from the segment asked
   for), under one session id for the whole playback. new_session: a new
   conversion (another sound track), the one before stopped. */
int builtin_open_at(double t, int new_session)
{
    const plex_item *it = &S.pl.det.v[0];
    caps_t k;
    player_src src;
    char title[200], headers[1400];
    int e;
    if (new_session || !*S.pl.sid) {
        if (*S.pl.sid && !S.pl.p.direct)
            plex_transcode_stop(&S.px, S.pl.sid);
        caps_session_id(&S.px, S.pl.sid, sizeof(S.pl.sid));
    }
    caps_for(S.quality, &k);
    k.own_subs = 1;                 /* reelcore draws them, playing the file itself */
    k.hevc = player_hevc_block();   /* and HEVC on the Pi 4's HEVC block */
    if (caps_play_at(&S.px, it, &k, S.direct, 0, S.pl.sid, &S.pl.p) != 0) {
        set_status("%s", S.pl.p.why);
        return -1;
    }
    media_headers(S.pl.p.headers, headers, sizeof(headers));
    /* the stream and the timeline under one session id: how the server
       ties them together (the dashboard's Now Playing); Jellyfin's is the
       one its PlaybackInfo gave */
    snprintf(S.pl.sid, sizeof(S.pl.sid), "%s", S.pl.p.session);
    snprintf(S.pl.pq.session, sizeof(S.pl.pq.session), "%s", S.pl.p.session);
    S.pl.pq.direct = S.pl.p.direct;
    if (S.px.kind == SRV_PLEX && strlen(headers) + strlen(S.pl.p.session) + 40 < sizeof(headers))
        snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers), "X-Plex-Session-Identifier: %s\r\n",
                 S.pl.p.session);
    builtin_title(it, title, sizeof(title));
    memset(&src, 0, sizeof(src));
    src.url = S.pl.p.url;
    src.headers = headers;
    src.user_agent = S.agent;
    src.title = title;
    src.convert = !S.pl.p.direct;
    src.base = 0;
    src.start = t;
    src.duration = it->duration_ms / 1000.0;
    src.subs = it->nsubs;
    {
        static double ch[100];
        int n = 0;
        for (int i = 0; i < it->nchapters && n < 100; i++)
            ch[n++] = it->chapters[i].start_ms / 1000.0;
        src.chapters = ch;
        src.nchapters = n;
    }
    snprintf(S.play_why, sizeof(S.play_why), "%s", S.pl.p.why);
    set_status("%s", S.pl.p.why);
    e = player_open(&src, S.browser_w);
    builtin_subs_forget();          /* the last reelcore's files: this one is given its own */
    if (e != 0) {
        report("Can't play %s: %s", title, player_error());
        return -1;
    }
    S.pl.tl_cs = now_cs() + TIMELINE_CS;
    S.pl.ping_cs = now_cs() + PING_CS;
    return 0;
}

/* Back to the page it was played from */
static void builtin_leave(void)
{
    int st[9];
    if (S.page != PG_PLAYER)
        return;
    S.page = S.pl.prev_page;
    set_extent();
    set_where();
    if (S.browser_open) {
        window_state(S.browser_w, st);
        open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, S.page == PG_GRID ? S.grid_sy : 0);
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    }
    if (S.page == PG_DETAILS)
        det_refresh();              /* where it stopped: Resume from */
    S.posters_wanted = S.page == PG_GRID;
}

/* Stops playing: the server is told, a converted stream stopped */
void builtin_stop(int leave)
{
    int was_mini = player_mini();
    if (!S.pl.on)
        return;
    if (player_ready() && !player_ended())
        builtin_timeline("stopped");
    if (!S.pl.p.direct && *S.pl.sid)
        plex_transcode_stop(&S.px, S.pl.sid);
    S.pl.sid[0] = 0;
    player_close();
    builtin_subs_forget();
    if (was_mini)
        choices_save();             /* where the mini player was left */
    S.pl.on = 0;
    S.pl.upnext_cs = 0;
    if (S.pl.det.n)
        plex_list_free(&S.pl.det);
    if (S.pl.next.n)
        plex_list_free(&S.pl.next);
    if (leave)
        builtin_leave();
}

/* it: the video (a list's item); what's played comes from its details */
static void builtin_play(const plex_item *it, int how)
{
    plex_list d;
    int st[9], prev = S.page == PG_PLAYER ? S.pl.prev_page : S.page;
    int was_mini = player_mini();   /* the next episode stays in the mini player */
    double t;
    hourglass(1);
    if (plex_details(&S.px, it, &d) != 0) {
        hourglass(0);
        report("Can't get %s from the server: %s", it->title, S.px.err);
        return;
    }
    builtin_stop(0);
    S.pl.det = d;
    S.pl.on = 1;
    /* a play queue of one, as the Plex apps play from (without one it still plays) */
    plex_play_queue(&S.px, &d.v[0], &S.pl.pq);
    hourglass(0);
    S.pl.prev_page = prev == PG_PLAYER ? PG_GRID : prev;
    if (!S.browser_open)
        browser_open();
    window_state(S.browser_w, st);
    if (S.page == PG_GRID)
        S.grid_sy = st[6];
    S.page = PG_PLAYER;
    S.hover = -1;
    S.posters_wanted = 0;
    set_extent();
    set_where();
    /* the window keeps its size: the picture fits in it */
    if (!was_mini) {
        open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
        set_caret(S.browser_w, -1, NULL);
    }
    t = how != PLAY_START && d.v[0].view_offset_ms > 0 ? d.v[0].view_offset_ms / 1000.0 : 0;
    S.pl.sid[0] = 0;                /* a new playback */
    if (builtin_open_at(t, 1) != 0) {
        builtin_stop(1);
        if (was_mini)
            browser_open();
        return;
    }
    if (was_mini)
        player_set_mini(1);
}

/* The card at the end of an episode: the next one, in so many seconds */
static void upnext_card(void)
{
    const plex_item *n = &S.pl.next.v[0];
    char head[200], line[300], line2[60], t[400];
    int s = (S.pl.upnext_cs - now_cs() + 99) / 100;
    snprintf(t, sizeof(t), "Up next \xc2\xb7 %s", n->grandparent_title ? n->grandparent_title : "");
    latin1(t, head, sizeof(head));
    snprintf(t, sizeof(t), "S%d E%d \xc2\xb7 %s", n->parent_index, n->index, n->title);
    latin1(t, line, sizeof(line));
    snprintf(line2, sizeof(line2), "Playing in %d s", s < 0 ? 0 : s);
    player_card(head, line, line2, "Play now", "Back");
}

static void upnext_play(void)
{
    plex_item *n;
    int prev = S.pl.prev_page;
    if (!S.pl.next.n)
        return;
    n = &S.pl.next.v[0];
    player_card(NULL, NULL, NULL, NULL, NULL);
    S.pl.upnext_cs = 0;
    {
        plex_list keep = S.pl.next;         /* builtin_play frees it (builtin_stop) */
        memset(&S.pl.next, 0, sizeof(S.pl.next));
        S.page = PG_PLAYER;
        builtin_play(n, PLAY_START);
        S.pl.prev_page = prev;
        plex_list_free(&keep);
    }
}

/* The end: watched; the next episode offered, or back */
static void builtin_ended(void)
{
    const plex_item *it = &S.pl.det.v[0];
    if (S.pl.upnext_cs)             /* already offered (Skip credits), then the end */
        return;
    player_skip(NULL);
    builtin_timeline("stopped");
    plex_mark(&S.px, it, 1);
    if (S.pl.next.n)
        plex_list_free(&S.pl.next);
    if (plex_next_episode(&S.px, it, &S.pl.next) == 0 && S.pl.next.n) {
        S.pl.upnext_cs = now_cs() + UPNEXT_CS;
        upnext_card();
        return;
    }
    builtin_stop(1);
}

/* Plex's markers: Skip intro or Skip credits while one is playing (not in
   its last second, so a skip to its end doesn't show it again) */
static void builtin_skip_update(void)
{
    const plex_item *it = &S.pl.det.v[0];
    int64_t t = (int64_t)(player_position() * 1000);
    int m = plex_marker_at(it, t);
    if (m >= 0 && t >= it->markers[m].end_ms - 1000)
        m = -1;
    player_skip(m < 0 ? NULL : it->markers[m].type == PM_INTRO ? "Skip intro" : "Skip credits");
}

/* Skip: to the intro's end; past credits that run to the end, the end
   (watched, and Up next for an episode) */
static void builtin_skip(void)
{
    const plex_item *it = &S.pl.det.v[0];
    const plex_marker *k;
    int m = plex_marker_at(it, (int64_t)(player_position() * 1000));
    if (m < 0)
        return;
    k = &it->markers[m];
    player_skip(NULL);
    if (k->type == PM_CREDITS && (k->final || (it->duration_ms > 0 && k->end_ms >= it->duration_ms - 5000))) {
        builtin_ended();
        return;
    }
    player_seek(k->end_ms / 1000.0);
}

/* Page Up and Page Down: the chapter before (or this one's start, if it's
   more than 3 s in) and the next */
void builtin_chapter_step(int d)
{
    const plex_item *it = &S.pl.det.v[0];
    double t = player_position();
    int c = plex_chapter_at(it, (int64_t)(t * 1000));
    if (!it->nchapters)
        return;
    if (d > 0)
        c = c + 1;
    else if (c >= 0 && t - it->chapters[c].start_ms / 1000.0 < 3)
        c = c - 1;
    if (c < 0)
        c = 0;
    if (c >= it->nchapters)
        return;
    player_seek(it->chapters[c].start_ms / 1000.0);
}

/* What the player said */
void builtin_event(int e)
{
    switch (e) {
    case PE_READY:
        builtin_subs_apply();
        builtin_timeline("playing");
        break;
    case PE_PAUSED:
        builtin_timeline("paused");
        break;
    case PE_PLAYING:
        builtin_timeline("playing");
        break;
    case PE_END:
        builtin_ended();
        break;
    case PE_FAILED:
        report("%s", player_error());
        builtin_stop(1);
        break;
    case PE_BACK:
        builtin_stop(1);
        break;
    case PE_CARD_1:
        if (S.pl.upnext_cs)
            upnext_play();
        break;
    case PE_CARD_2:
        builtin_stop(1);
        break;
    case PE_MENU:
        player_menu_open();
        break;
    case PE_SUBS:
        psubs_menu_open();
        break;
    case PE_SKIP:
        builtin_skip();
        break;
    }
}

void builtin_nulls(void)
{
    int e = player_null();
    if (e != PE_NONE) {
        builtin_event(e);
        return;
    }
    if (!S.pl.on)
        return;
    if (S.pl.upnext_cs) {
        if (now_cs() - S.pl.upnext_cs >= 0)
            upnext_play();
        else {
            static int last;
            if (now_cs() - last >= 50) {
                last = now_cs();
                upnext_card();
            }
        }
        return;
    }
    if (player_ready() && !player_ended())
        builtin_skip_update();
    timeline_step();
    if (player_ready() && !player_paused() && !player_ended() && now_cs() - S.pl.tl_cs >= 0) {
        S.pl.tl_async = 1;          /* the reports every 10 s: from a thread */
        builtin_timeline("playing");
        S.pl.tl_async = 0;
    }
    /* paused in a converted stream: the server stops converting for a
       client it hasn't heard from, so it's told the session is wanted */
    if (player_paused() && !S.pl.p.direct && now_cs() - S.pl.ping_cs >= 0) {
        plex_transcode_ping(&S.px, S.pl.sid);
        S.pl.ping_cs = now_cs() + PING_CS;
    }
}
