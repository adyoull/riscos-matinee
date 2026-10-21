/*
 * menus.c - the menus (the icon bar's, a poster's, the player's and their
 * submenus), and Plex Home: who's watching.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- menus ------------------------------------------------------------------------ */

static menu_t m_bar, m_servers, m_forget, m_player, m_quality, m_size, m_item;
menu_t m_subs;
static menu_t m_play, m_audio, m_vol, m_pic, m_psubs, m_chap, m_users, m_pin, m_rate;

static void menu_begin(menu_t *m, const char *title)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->m.title, sizeof(m->m.title), "%.11s", title);
    m->m.tfg = 7; m->m.tbg = 2; m->m.wfg = 7; m->m.wbg = 0;
    m->m.height = 44;
    m->m.width = (int)strlen(m->m.title) * 16 + 16;
}

static void menu_add(menu_t *m, const char *text, int tick, int shade, int sub, int dots)
{
    mitem_t *i = &m->m.item[m->n];
    int w;
    if (m->n >= 16)
        return;
    latin1(text, m->text[m->n], sizeof(m->text[0]));
    i->flags = (tick ? 1 : 0) | (dots ? 2 : 0);
    i->sub = sub;
    i->iflags = 0x07000121u | (shade ? 0x400000u : 0);  /* text, filled, indirected */
    i->data[0] = (int)(intptr_t)m->text[m->n];
    i->data[1] = -1;
    i->data[2] = (int)sizeof(m->text[0]);
    w = (int)strlen(m->text[m->n]) * 16 + 32;
    if (w > m->m.width)
        m->m.width = w;
    m->n++;
}

static void menu_end(menu_t *m)
{
    if (m->n)
        m->m.item[m->n - 1].flags |= 0x80;          /* the last */
}

void open_menu(menu_t *m, int x, int y)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)&m->m;
    r.r[2] = x;
    r.r[3] = y;
    swi(Wimp_CreateMenu, &r);
}

static void bar_menu_build(void)
{
    int signed_in = *S.px.account_token != 0;
    /* the Plex account's servers, then the Jellyfin ones signed in to, then
       Add a server (S.srv_map says which an item is) */
    menu_begin(&m_servers, "Servers");
    for (int i = 0; i < S.nservers && m_servers.n < 9; i++) {
        S.srv_map[m_servers.n] = i;
        menu_add(&m_servers, S.servers[i].name, S.px.kind == SRV_PLEX && !strcmp(S.servers[i].id, S.px.server_id),
                 0, -1, 0);
    }
    if (signed_in && !S.nservers) {
        S.srv_map[m_servers.n] = SRV_ADD - 1;
        menu_add(&m_servers, "(no Plex servers found)", 0, 1, -1, 0);
    }
    if (m_servers.n)
        m_servers.m.item[m_servers.n - 1].flags |= 2;   /* a line under Plex's */
    for (int k = 0; k < S.njf && m_servers.n < 15; k++) {
        char t[80];
        snprintf(t, sizeof(t), "%s (Jellyfin)", S.jf[k].name);
        S.srv_map[m_servers.n] = -1 - k;
        menu_add(&m_servers, t, S.px.kind == SRV_JELLYFIN && !strcmp(S.jf[k].id, S.px.server_id) &&
                 !strcmp(S.jf[k].uid, S.px.user_id), 0, -1, 0);
    }
    if (S.njf && m_servers.n)
        m_servers.m.item[m_servers.n - 1].flags |= 2;
    for (int k = 0; k < S.ndl && m_servers.n < 15; k++) {
        char t[80];
        snprintf(t, sizeof(t), "%s (DLNA)", S.dl[k].name);
        S.srv_map[m_servers.n] = SRV_DL(k);
        menu_add(&m_servers, t, S.px.kind == SRV_DLNA && !strcmp(S.dl[k].udn, S.px.server_id), 0, -1, 0);
    }
    if (S.ndl && m_servers.n)
        m_servers.m.item[m_servers.n - 1].flags |= 2;
    S.srv_map[m_servers.n] = SRV_ADD;
    menu_add(&m_servers, "Add a server...", 0, 0, -1, 0);
    /* Forget a server: each that Matinee keeps (asked first when chosen) */
    menu_begin(&m_forget, "Forget");
    if (signed_in || (S.px.kind == SRV_PLEX && *S.px.base)) {
        S.fg_map[m_forget.n] = FG_PLEX;
        menu_add(&m_forget, signed_in ? "Plex (your sign-in)" : "Plex (the server given by hand)", 0, 0, -1, 0);
    }
    for (int k = 0; k < S.njf && m_forget.n < 15; k++) {
        char t[140];
        snprintf(t, sizeof(t), "%s (Jellyfin, %s)", S.jf[k].name, S.jf[k].uname);
        S.fg_map[m_forget.n] = FG_JF(k);
        menu_add(&m_forget, t, 0, 0, -1, 0);
    }
    for (int k = 0; k < S.ndl && m_forget.n < 16; k++) {
        char t[80];
        snprintf(t, sizeof(t), "%s (DLNA)", S.dl[k].name);
        S.fg_map[m_forget.n] = FG_DL(k);
        menu_add(&m_forget, t, 0, 0, -1, 0);
    }
    menu_end(&m_forget);
    S.srv_map[m_servers.n] = SRV_FORGET;
    menu_add(&m_servers, "Forget a server", 0, !m_forget.n, m_forget.n ? (int)(intptr_t)&m_forget.m : -1, 0);
    menu_end(&m_servers);
    /* Plex Home: its people; one with a PIN has a writable PIN item to its right */
    menu_begin(&m_pin, "PIN");
    menu_add(&m_pin, "", 0, 0, -1, 0);
    m_pin.m.item[0].flags |= 4;                 /* writable */
    m_pin.m.item[0].data[1] = (int)(intptr_t)"A0-9;D*";
    m_pin.m.item[0].iflags |= 0x07000000u;
    m_pin.m.width = 200;
    m_pin.text[0][0] = 0;
    menu_end(&m_pin);
    menu_begin(&m_users, "Who's watching");
    for (int i = 0; i < S.nusers && i < 15; i++) {
        char t[80];
        int cur = !strcmp(S.users[i].uuid, S.user_uuid);
        snprintf(t, sizeof(t), "%s%s", S.users[i].title, S.users[i].protected_ && !cur ? " (PIN)" : "");
        menu_add(&m_users, t, cur, 0, S.users[i].protected_ && !cur ? (int)(intptr_t)&m_pin.m : -1, 0);
    }
    if (S.nusers <= 0)
        menu_add(&m_users, "(no Plex Home)", 0, 1, -1, 0);
    menu_end(&m_users);
    menu_begin(&m_player, "Player");
    for (int i = 0; i < PLAYER_COUNT; i++)
        menu_add(&m_player, player_names[i], S.player == i, 0, -1, i == PLAYER_BUILTIN);
    menu_end(&m_player);
    menu_begin(&m_quality, "Quality");
    for (int i = 0; i < Q_COUNT; i++)
        menu_add(&m_quality, caps_quality_name(i), S.quality == i, 0, -1, 0);
    menu_end(&m_quality);
    menu_begin(&m_size, "Poster size");
    for (int i = 0; i < 3; i++)
        menu_add(&m_size, sizes[i].name, S.psize == i, 0, -1, 0);
    menu_end(&m_size);

    menu_begin(&m_bar, APP);
    menu_add(&m_bar, "Info", 0, 0, S.proginfo, 0);
    menu_add(&m_bar, "Add a server...", 0, 0, -1, 0);
    menu_add(&m_bar, "Servers", 0, 0, (int)(intptr_t)&m_servers.m, 0);
    {   /* Plex Home's people: for a Plex server */
        int users = S.nusers >= 2 && S.px.kind == SRV_PLEX;
        menu_add(&m_bar, "Switch user", 0, !users, users ? (int)(intptr_t)&m_users.m : -1, 0);
    }
    menu_add(&m_bar, "Player", 0, 0, (int)(intptr_t)&m_player.m, 0);
    menu_add(&m_bar, "Quality", 0, 0, (int)(intptr_t)&m_quality.m, 0);
    menu_add(&m_bar, "Poster size", 0, 0, (int)(intptr_t)&m_size.m, 0);
    menu_add(&m_bar, "Direct play when possible", S.direct, 0, -1, 1);
    {
        char t[64];
        int n;
        long long b = imgcache_size(&n);
        snprintf(t, sizeof(t), "Clear image cache (%.1f MB)", b / 1048576.0);
        menu_add(&m_bar, t, 0, n == 0, -1, 0);
    }
    menu_add(&m_bar, S.px.kind == SRV_JELLYFIN ? "Sign out of this server" :
                     S.px.kind == SRV_DLNA ? "Forget this server" : "Sign out", 0,
             !signed_in && !*S.px.base, -1, 0);
    menu_add(&m_bar, "Quit", 0, 0, -1, 0);
    menu_end(&m_bar);
}

void bar_menu_open(int x)
{
    if (*S.px.account_token && !S.nservers)
        servers_get();              /* for the Servers submenu */
    if (*S.px.account_token && S.nusers < 0)
        users_get();                /* and Switch user */
    bar_menu_build();
    S.menu_kind = 1;
    S.menu_x = x;
    open_menu(&m_bar, x - 64, 96 + MB_COUNT * 44);
}

static const plex_item *sel_item(void)
{
    return S.have_list && S.sel >= 0 && S.sel < S.list.n ? &S.list.v[S.sel] : NULL;
}

/* What the item menu is for: the selected poster, or on a details page
   opened from More like this (not one of the list's), that video */
static int menu_rel(void)
{
    return S.page == PG_DETAILS && S.det_i < 0 && det_item() != NULL;
}

static const plex_item *menu_item(void)
{
    return menu_rel() ? det_item() : sel_item();
}

/* The subtitles of the video in S.det: None, then its tracks */
void subs_menu_build(void)
{
    const plex_item *it = det_item();
    char t[160];
    menu_begin(&m_subs, "Subtitles");
    menu_add(&m_subs, "None", !it || plex_sub_selected(it) < 0, 0, -1, it && it->nsubs);
    for (int i = 0; it && i < it->nsubs && i < 15; i++) {
        const plex_sub *sb = &it->subs[i];
        snprintf(t, sizeof(t), "%s%s", sb->title,
                 sb->forced && !strstr(sb->title, "orced") ? " (forced)" : "");
        menu_add(&m_subs, t, sb->selected, 0, -1, 0);
    }
    menu_end(&m_subs);
}

static menu_t m_sort;

/* The library bar's buttons and letters */
void lib_action(int id, int x, int y)
{
    char path[256];
    if (id >= LB_VIEW && id < LB_VIEW + 3 && id - LB_VIEW != S.lib.view) {
        lib_path(id - LB_VIEW, 0, 0, path, sizeof(path));
        show_list(path, "", 0, 0);
    } else if (id == LB_UNWATCHED) {
        lib_path(0, S.lib.sort, !S.lib.unwatched, path, sizeof(path));
        show_list(path, "", 0, 0);
    } else if (id == LB_SORT) {
        menu_begin(&m_sort, "Sort by");
        for (int k = 0; k < SORTS; k++)
            menu_add(&m_sort, sort_names[k], k == S.lib.sort, 0, -1, 0);
        menu_end(&m_sort);
        S.menu_kind = 6;
        S.menu_x = x;
        S.menu_y = y;
        open_menu(&m_sort, x - 64, y);
    } else if (id >= LB_AZ && id < LB_AZ + 27 && S.lib.az_first[id - LB_AZ] >= 0) {
        select_tile(S.lib.az_first[id - LB_AZ]);    /* (scrolled into view) */
    }
}

/* Is it in Continue watching (part watched, or the list is Continue watching)? */
static int in_continue(const plex_item *it)
{
    int c;
    if (it->view_offset_ms > 0 || !strncmp(S.path, "/library/onDeck", 15))
        return 1;
    return S.home.on && S.sel >= 0 && home_row_of(S.sel, &c) >= 0 &&
           S.home.row[home_row_of(S.sel, &c)].kind == PR_CONTINUE;
}

static void item_menu_build(void)
{
    const plex_item *it = menu_item();
    int video = it && it->kind == PI_VIDEO, folder = it && it->kind == PI_FOLDER, rel = menu_rel();
    char t[80], when[32], head[80];
    save_prepare(it);
    /* a video's subtitle tracks are only in its details */
    if (video && !det_is(it))
        det_fetch(it, 0);
    if (video && det_is(it))
        subs_menu_build();
    if (it && !rel && S.disp)
        fit_disp(S.sel);
    if (rel)
        latin1(it->title, head, sizeof(head));
    else
        snprintf(head, sizeof(head), "%s", it && S.disp ? S.disp[S.sel].line[0] : APP);
    menu_begin(&m_item, head);
    menu_add(&m_item, folder ? "Open" : "Play", 0, !video && !folder, -1, 0);
    menu_add(&m_item, "Details...", 0, !video, -1, 0);
    if (video && it->view_offset_ms > 0) {
        hms(it->view_offset_ms, when, sizeof(when));
        snprintf(t, sizeof(t), "Resume from %s", when);
        menu_add(&m_item, t, 0, 0, -1, 0);
    } else {
        menu_add(&m_item, "Resume", 0, 1, -1, 0);
    }
    menu_add(&m_item, "Play from start", 0, !video, -1, 0);
    menu_add(&m_item, "Subtitles", 0, !video || !det_is(it) || !det_item()->nsubs,
             video && det_is(it) && det_item()->nsubs ? (int)(intptr_t)&m_subs.m : -1, 0);
    if (S.save.active)
        menu_add(&m_item, "Stop saving", 0, 0, -1, 0);
    else
        menu_add(&m_item, "Save original file", 0, !S.sv_ok,
                 S.sv_ok && S.sv_size <= FILE_MAX ? S.save_w : -1, 0);
    menu_add(&m_item, "Test speed", 0, !S.sv_ok || S.save.active || S.speed.active, -1, 1);
    menu_add(&m_item, "Mark watched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 0);
    menu_add(&m_item, "Mark unwatched", 0, !it || !it->rating_key || it->kind == PI_OTHER, -1, 0);
    {
        /* yours: the details' if they're this one's (lists may leave it out) */
        double ur = it && det_is(it) ? det_item()->user_rating : it ? it->user_rating : 0;
        int can = it && it->rating_key && it->type && S.px.kind == SRV_PLEX &&
                  (!strcmp(it->type, "movie") || !strcmp(it->type, "episode") || !strcmp(it->type, "show") ||
                   !strcmp(it->type, "season"));
        menu_begin(&m_rate, "Rate");
        menu_add(&m_rate, "No rating", ur <= 0, 0, -1, 1);
        for (int k = 1; k <= 5; k++) {
            char t[16];
            snprintf(t, sizeof(t), "%d star%s", k, k == 1 ? "" : "s");
            menu_add(&m_rate, t, ur > 2 * k - 1 && ur < 2 * k + 1, 0, -1, 0);
        }
        menu_end(&m_rate);
        menu_add(&m_item, "Rate", 0, !can, can ? (int)(intptr_t)&m_rate.m : -1, 0);
    }
    menu_add(&m_item, "Remove from Continue watching", 0, !it || !it->rating_key || it->kind != PI_VIDEO || !in_continue(it),
             -1, 1);
    menu_add(&m_item, "Back", 0, S.nhist == 0, -1, 0);
    menu_add(&m_item, "Refresh", 0, 0, -1, 0);
    menu_end(&m_item);
}

/* The playing video's subtitles: None, then its tracks (the server's
   list: the names are Plex's) */
static void psubs_menu_build(void)
{
    const plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    char t[160];
    menu_begin(&m_psubs, "Subtitles");
    menu_add(&m_psubs, "None", !it || plex_sub_selected(it) < 0, 0, -1, it && it->nsubs);
    for (int i = 0; it && i < it->nsubs && i < 15; i++) {
        const plex_sub *sb = &it->subs[i];
        snprintf(t, sizeof(t), "%s%s", sb->title, sb->forced && !strstr(sb->title, "orced") ? " (forced)" : "");
        menu_add(&m_psubs, t, sb->selected, 0, -1, 0);
    }
    menu_end(&m_psubs);
}

/* Subtitles on the bar: the menu on its own, by the pointer */
void psubs_menu_open(void)
{
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r))
        p[0] = S.scr_w / 2, p[1] = S.scr_h / 2;
    psubs_menu_build();
    S.menu_kind = 5;
    S.menu_x = p[0];
    S.menu_y = p[1];
    open_menu(&m_psubs, p[0] - 64, p[1] + 44 * (m_psubs.n + 1));
}

/* Menu over the built-in player */
static void player_menu_build(void)
{
    const plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    char t[120];
    int n;
    menu_begin(&m_audio, "Sound track");
    /* a converted stream has the one the server was asked for: choose on the server */
    S.pl.menu_audio = !S.pl.p.direct;
    if (S.pl.menu_audio) {
        n = it ? it->nauds : 0;
        for (int i = 0; i < n && i < 15; i++)
            menu_add(&m_audio, it->auds[i].title, i == plex_audio_selected(it), 0, -1, 0);
    } else {
        n = player_tracks();
        for (int i = 0; i < n && i < 15; i++) {
            player_track_name(i, t, sizeof(t));
            menu_add(&m_audio, t, i == player_track(), 0, -1, 0);
        }
    }
    if (!n)
        menu_add(&m_audio, "(none)", 0, 1, -1, 0);
    menu_end(&m_audio);
    psubs_menu_build();
    /* chapters: 16 at most, around the one playing */
    menu_begin(&m_chap, "Chapters");
    {
        int nc = it ? it->nchapters : 0, cur = it ? plex_chapter_at(it, (int64_t)(player_position() * 1000)) : -1;
        int first = cur - 7;
        if (first > nc - 16)
            first = nc - 16;
        if (first < 0)
            first = 0;
        S.pl.chap_first = first;
        for (int i = first; i < nc && i < first + 16; i++) {
            char tm[16], u[200];
            int s2 = (int)(it->chapters[i].start_ms / 1000);
            if (s2 >= 3600)
                snprintf(tm, sizeof(tm), "%d:%02d:%02d", s2 / 3600, s2 / 60 % 60, s2 % 60);
            else
                snprintf(tm, sizeof(tm), "%d:%02d", s2 / 60, s2 % 60);
            snprintf(u, sizeof(u), "%s  %s", tm, it->chapters[i].title);
            latin1(u, t, 60);
            menu_add(&m_chap, t, i == cur, 0, -1, 0);
        }
        if (!nc)
            menu_add(&m_chap, "(none)", 0, 1, -1, 0);
    }
    menu_end(&m_chap);
    menu_begin(&m_vol, "Volume");
    for (int i = 0; i < 5; i++) {
        snprintf(t, sizeof(t), "%d%%", 100 - i * 25);
        menu_add(&m_vol, t, (int)(player_volume() * 100 + 0.5) == 100 - i * 25, 0, -1, 0);
    }
    menu_end(&m_vol);
    menu_begin(&m_pic, "Picture");
    for (int i = 0; i < PIC_COUNT; i++)
        menu_add(&m_pic, pic_names[i], player_pic_mode() == i, 0, -1, 0);
    menu_end(&m_pic);
    menu_begin(&m_play, "Player");
    menu_add(&m_play, "Sound track", 0, n < 2, n >= 2 ? (int)(intptr_t)&m_audio.m : -1, 0);
    menu_add(&m_play, "Subtitles", 0, !it || !it->nsubs, it && it->nsubs ? (int)(intptr_t)&m_psubs.m : -1, 0);
    menu_add(&m_play, "Chapters", 0, !it || !it->nchapters, it && it->nchapters ? (int)(intptr_t)&m_chap.m : -1, 0);
    menu_add(&m_play, "Volume", 0, 0, (int)(intptr_t)&m_vol.m, 0);
    menu_add(&m_play, "Picture", 0, 0, (int)(intptr_t)&m_pic.m, 1);
    menu_add(&m_play, "Stats", player_stats(), 0, -1, 0);
    menu_add(&m_play, "Full screen", player_fullscreen(), 0, -1, 0);
    menu_add(&m_play, "Mini player", player_mini(), 0, -1, 0);
    menu_add(&m_play, "Keep on top", player_ontop(), 0, -1, 1);
    menu_add(&m_play, "Hardware overlay", player_overlay(), 0, -1, 1);
    menu_add(&m_play, "Stop", 0, 0, -1, 0);
    menu_end(&m_play);
}

void player_menu_open(void)
{
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)p;
    if (swi(Wimp_GetPointerInfo, &r))
        p[0] = S.scr_w / 2, p[1] = S.scr_h / 2;
    player_menu_build();
    S.menu_kind = 4;
    S.menu_x = p[0];
    S.menu_y = p[1];
    open_menu(&m_play, p[0] - 64, p[1]);
}

/* A sound track: reelcore's for the file itself; the server's for a converted stream */
static void choose_audio(int k)
{
    plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    if (k < 0)
        return;
    if (!S.pl.menu_audio) {
        player_set_track(k);
        return;
    }
    if (!it || k >= it->nauds || k == plex_audio_selected(it))
        return;
    hourglass(1);
    if (plex_set_audio(&S.px, it, it->auds[k].id) != 0) {
        hourglass(0);
        report("Can't choose that sound track: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; i < it->nauds; i++)
        it->auds[i].selected = i == k;
    player_note("Changing the sound track...");
    if (builtin_open_at(player_position(), 1) != 0)
        builtin_stop(1);
}

/* A subtitle file beside the video (Plex's /library/streams/<id>): fetched
   into the scrap directory for the player. 0 = done, path set. */
static int sub_fetch(const plex_sub *sb, char *path, size_t size)
{
    const char *scrap = getenv("Wimp$ScrapDir");
    char dir[256], url[1024], hd[1400], h2[1400];
    net_buf b;
    FILE *f;
    int ok;
    if (!scrap || !*scrap || !sb->key)
        return -1;
    snprintf(dir, sizeof(dir), "%s" SEP APP, scrap);
    make_dir(dir);
    snprintf(path, size, "%s" SEP "Sub%ld", dir, sb->id);
    media_url(sb->key, url, sizeof(url));
    plex_headers(&S.px, S.px.token, hd, sizeof(hd));
    media_headers(hd, h2, sizeof(h2));
    hourglass(1);
    ok = net_fetch(url, h2, NULL, &b, 15000, S.px.err, sizeof(S.px.err)) == 0;
    hourglass(0);
    if (!ok)
        return -1;
    ok = (f = fopen(path, "wb")) != NULL;
    if (ok) {
        ok = fwrite(b.data, 1, b.len, f) == b.len;
        ok &= fclose(f) == 0;
    }
    net_buf_free(&b);
    if (!ok)
        snprintf(S.px.err, sizeof(S.px.err), "can't write %s", path);
    return ok ? 0 : -1;
}

/* The files given to the player, gone with it */
void builtin_subs_forget(void)
{
    for (int i = 0; i < S.pl.n_ext; i++)
        remove(S.pl.ext[i].path);
    S.pl.n_ext = 0;
}

/* Playing the file itself: the player shows the subtitles chosen on the
   server (or none), as Plex's apps do. A track in the file is reelcore's
   track of the same place among the file's tracks (Plex lists them in the
   file's order, then the files beside it); a file beside it is fetched
   and added (once). */
void builtin_subs_apply(void)
{
    const plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    const plex_sub *sb;
    int sel, j = 0, t = -1;
    char path[300];
    if (!it || !S.pl.p.direct || !player_ready())
        return;
    sel = plex_sub_selected(it);
    if (sel < 0) {
        player_set_sub(-1);
        return;
    }
    sb = &it->subs[sel];
    if (!sb->external) {
        for (int i = 0; i < sel; i++)
            j += !it->subs[i].external;
        if (j < player_sub_tracks())
            t = j;
    } else {
        for (int i = 0; i < S.pl.n_ext; i++)
            if (S.pl.ext[i].id == sb->id)
                t = S.pl.ext[i].track;
        if (t < 0 && sub_fetch(sb, path, sizeof(path)) == 0) {
            t = player_add_sub_file(path);
            if (t >= 0 && S.pl.n_ext < 8) {
                S.pl.ext[S.pl.n_ext].id = sb->id;
                S.pl.ext[S.pl.n_ext].track = t;
                snprintf(S.pl.ext[S.pl.n_ext].path, sizeof(S.pl.ext[0].path), "%s", path);
                S.pl.n_ext++;
            } else {
                remove(path);
            }
        }
    }
    if (t < 0 || player_set_sub(t) != 0)
        player_note("Can't show those subtitles");
}

/* Subtitles menu choice k (the player's): 0 None, else track k-1. Kept on
   the server; the file itself: the player shows them; a converted stream
   (or subtitles the player can't read): started again from here, the
   server burning them in */
static void choose_psub(int k)
{
    plex_item *it = S.pl.det.n ? &S.pl.det.v[0] : NULL;
    char name[120], t[160];
    long id;
    if (!it || k < 0 || k > it->nsubs || k - 1 == plex_sub_selected(it))
        return;
    id = k ? it->subs[k - 1].id : 0;
    hourglass(1);
    if (plex_set_subtitle(&S.px, it, id) != 0) {
        hourglass(0);
        report("Can't choose the subtitles: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; i < it->nsubs; i++)
        it->subs[i].selected = i == k - 1;
    latin1(k ? it->subs[k - 1].title : "", name, sizeof(name));
    if (S.pl.p.direct && (!k || caps_sub_own(&it->subs[k - 1]))) {
        builtin_subs_apply();
        snprintf(t, sizeof(t), k ? "Subtitles: %s" : "Subtitles off", name);
        player_note(t);
        return;
    }
    player_note(k ? "Changing the subtitles..." : "Subtitles off...");
    if (builtin_open_at(player_position(), 1) != 0)
        builtin_stop(1);
}

void item_menu_open(int x, int y)
{
    item_menu_build();
    S.menu_kind = 2;
    S.menu_x = x;
    S.menu_y = y;
    open_menu(&m_item, x - 64, y);
}

void quit(void)
{
    _kernel_swi_regs r;
    save_stop(NULL);
    if (S.speed.active)
        speed_close();
    builtin_stop(0);
    draw_done();
    for (int i = 0; i < PEND_MAX; i++)
        if (S.pend[i].ref)
            remove(S.pend[i].path);
    r.r[0] = S.task;
    swi(Wimp_CloseDown, &r);
}

static void set_psize(int k)
{
    if (k < 0 || k > 2 || k == S.psize)
        return;
    S.psize = k;
    choices_save();
    cache_free_all();               /* posters are made at the tile's size */
    if (S.have_list)
        make_disp();
    if (S.browser_open) {
        int st[9];
        window_state(S.browser_w, st);
        S.cols = layout_cols(st[3] - st[1]);
        set_extent();
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    }
    if (S.page == PG_DETAILS)
        det_refresh();              /* its backdrop went with the cache */
    S.posters_wanted = 1;
}

/* Subtitles menu choice k: 0 None, else track k-1 of the video in S.det */
static void choose_sub(int k)
{
    const plex_item *it = det_item();
    char name[120];
    if (!it || k < 0 || k > it->nsubs)
        return;
    latin1(k ? it->subs[k - 1].title : "", name, sizeof(name));
    hourglass(1);
    if (plex_set_subtitle(&S.px, it, k ? it->subs[k - 1].id : 0) != 0) {
        hourglass(0);
        report("Can't choose the subtitles: %s", S.px.err);
        return;
    }
    hourglass(0);
    {
        int own = k && caps_sub_own(&it->subs[k - 1]);      /* before det_refresh lets it go */
        det_refresh();
        it = NULL;
        if (own && S.player == PLAYER_BUILTIN)
            set_status("Subtitles: %s.", name);
        else if (k)
            set_status("Subtitles: %s. The server burns them into the picture.", name);
        else
            set_status("Subtitles off.");
    }
}

/* Watched (1) or not (0), on the server and here. it: the list's item or
   the details' */
void mark(const plex_item *it, int watched)
{
    hourglass(1);
    if (plex_mark(&S.px, it, watched) != 0) {
        hourglass(0);
        report("Can't mark it: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; S.have_list && i < S.list.n; i++)
        if (S.list.v[i].rating_key && it->rating_key && !strcmp(S.list.v[i].rating_key, it->rating_key)) {
            S.list.v[i].watched = watched;
            S.list.v[i].view_offset_ms = 0;
            redraw_tile(i);
        }
    if (det_item() && det_item()->rating_key && it->rating_key &&
        !strcmp(det_item()->rating_key, it->rating_key)) {
        S.det.v[0].watched = watched;
        S.det.v[0].view_offset_ms = 0;
        det_repaint();
    }
    set_status(watched ? "Marked as watched." : "Marked as not watched.");
}

/* Your rating (0..10, 2 a star; -1 none), on the server, and wherever it's shown */
void rate(const plex_item *it, int r)
{
    char key[64];
    if (!it->rating_key)
        return;
    snprintf(key, sizeof(key), "%s", it->rating_key);     /* it may be in a list redrawn below */
    hourglass(1);
    if (plex_rate(&S.px, it, r) != 0) {
        hourglass(0);
        report("Can't rate it: %s", S.px.err);
        return;
    }
    hourglass(0);
    for (int i = 0; S.have_list && i < S.list.n; i++)
        if (S.list.v[i].rating_key && !strcmp(S.list.v[i].rating_key, key))
            S.list.v[i].user_rating = r > 0 ? r : 0;
    if (S.have_det && S.det.n && S.det.v[0].rating_key && !strcmp(S.det.v[0].rating_key, key)) {
        S.det.v[0].user_rating = r > 0 ? r : 0;
        det_repaint();
    }
    if (r > 0)
        set_status("Rated %d star%s.", r / 2, r == 2 ? "" : "s");
    else
        set_status("Rating taken away.");
}

/* ---- Plex Home: who's watching ------------------------------------------------- */

/* The account's people (once; again after a switch). -1: not a Plex Home, or plex.tv didn't answer */
int users_get(void)
{
    int n;
    if (!*S.px.account_token)
        return S.nusers = 0;
    hourglass(1);
    n = plex_home_users(&S.px, S.users, PLEX_USERS);
    hourglass(0);
    S.nusers = n < 0 ? 0 : n;
    if (n > 0 && !*S.user_uuid)                 /* signed in as the account's owner */
        for (int i = 0; i < n; i++)
            if (S.users[i].admin) {
                snprintf(S.user_uuid, sizeof(S.user_uuid), "%s", S.users[i].uuid);
                snprintf(S.user_title, sizeof(S.user_title), "%s", S.users[i].title);
            }
    return n;
}

/* Switches to user i (pin: theirs, or ""): their account token, then the
   same server with their token for it (or another of theirs), and the
   home page again */
static void switch_user(int i, const char *pin)
{
    plex_ctx c = S.px;
    char srv[64];
    int e, k;
    if (i < 0 || i >= S.nusers)
        return;
    if (S.users[i].protected_ && !*pin) {
        report("%s has a PIN: move to the right of the name and type it, then press Return.", S.users[i].title);
        return;
    }
    builtin_stop(1);
    hourglass(1);
    e = plex_switch_user(&c, S.users[i].uuid, pin);
    hourglass(0);
    if (e == -2) {
        report("That isn't %s's PIN.", S.users[i].title);
        return;
    }
    if (e != 0) {
        report("Can't switch to %s: %s", S.users[i].title, c.err);
        return;
    }
    snprintf(S.px.account_token, sizeof(S.px.account_token), "%s", c.account_token);
    snprintf(S.user_uuid, sizeof(S.user_uuid), "%s", S.users[i].uuid);
    snprintf(S.user_title, sizeof(S.user_title), "%s", S.users[i].title);
    snprintf(srv, sizeof(srv), "%s", S.px.server_id);
    choices_save();
    /* the server's token is per user */
    if (servers_get() <= 0)
        return;
    for (k = 0; k < S.nservers && strcmp(S.servers[k].id, srv); k++)
        ;
    if (k == S.nservers) {
        report("%s can't use %s: choose one of their servers on the Servers menu.", S.user_title,
               S.px.server_name);
        S.px.base[0] = S.px.token[0] = 0;
        choose_server();
        return;
    }
    if (use_server(k) != 0)
        return;
    if (S.libs.n || S.libs.v)
        plex_list_free(&S.libs);    /* their libraries */
    cache_free_all();
    S.nhist = 0;
    browser_top();
    set_status("Now watching as %s.", S.user_title);
}

/* 1 if it's time to stop */
int menu_select(const int *sel)
{
    int b[5], kind = S.menu_kind;
    _kernel_swi_regs r;
    if (kind == 1) {
        switch (sel[0]) {
        case MB_SIGNIN:
            signin_open();
            break;
        case MB_SERVERS:
            if (sel[1] >= 0 && sel[1] < m_servers.n) {
                int w = S.srv_map[sel[1]];
                if (w == SRV_ADD)
                    signin_open();
                else if (w == SRV_FORGET) {
                    if (sel[2] >= 0 && sel[2] < m_forget.n)
                        forget_server(S.fg_map[sel[2]]);
                } else if (w <= -1 && w > -1 - S.njf)
                    use_jf(-1 - w);
                else if (w <= SRV_DL(0) && w > SRV_DL(S.ndl))
                    use_dl(SRV_DL(0) - w);
                else if (w >= 0 && w < S.nservers && use_server(w) == 0)
                    browser_top();
            }
            break;
        case MB_USERS:
            if (S.px.kind == SRV_PLEX && sel[1] >= 0 && sel[1] < S.nusers && strcmp(S.users[sel[1]].uuid, S.user_uuid)) {
                char pin[16];
                snprintf(pin, sizeof(pin), "%s", sel[2] == 0 ? m_pin.text[0] : "");
                pin[strcspn(pin, "\r\n")] = 0;
                m_pin.text[0][0] = 0;
                switch_user(sel[1], pin);
            }
            break;
        case MB_PLAYER:
            if (sel[1] >= 0 && sel[1] < PLAYER_COUNT) {
                S.player = sel[1];
                choices_save();
            }
            break;
        case MB_QUALITY:
            if (sel[1] >= 0 && sel[1] < Q_COUNT) {
                S.quality = sel[1];
                choices_save();
                det_repaint();
            }
            break;
        case MB_SIZE:
            set_psize(sel[1]);
            break;
        case MB_DIRECT:
            S.direct = !S.direct;
            choices_save();
            det_repaint();          /* how it will play */
            break;
        case MB_CACHE: {
            long long b = imgcache_size(NULL);
            int n = imgcache_clear();
            set_status("Image cache cleared: %d picture%s (%.1f MB).", n, n == 1 ? "" : "s", b / 1048576.0);
            break;
        }
        case MB_SIGNOUT: {          /* easily chosen by mistake: asked first */
            char q[300];
            if (S.px.kind == SRV_DLNA)
                snprintf(q, sizeof(q), "Forget %s? Matinee forgets this DLNA server; add it again with Servers, Add "
                         "a server. (Where you got to in its videos is kept.)", S.px.server_name);
            else if (S.px.kind == SRV_JELLYFIN)
                snprintf(q, sizeof(q), "Sign out of %s? Matinee forgets this Jellyfin server, and you'll need to "
                         "sign in to it again (Servers, Add a server).", S.px.server_name);
            else
                snprintf(q, sizeof(q), "Sign out? Matinee forgets the Plex server and your sign-in, and you'll need "
                         "a new code from plex.tv/link (or the server's token) to sign in again.");
            if (ask(q))
                sign_out();
            break;
        }
        case MB_QUIT:
            if (S.save.active && !ask("A file is being saved. Stop saving it and quit?"))
                break;
            quit();
            return 1;
        }
    } else if (kind == 2) {
        const plex_item *it = menu_item();
        int rel = menu_rel();
        switch (sel[0]) {
        case MI_PLAY:
        case MI_RESUME:
        case MI_START: {
            int how = sel[0] == MI_RESUME ? PLAY_RESUME : sel[0] == MI_START ? PLAY_START : PLAY_DEFAULT;
            if (it && rel)
                play_item(it, how);
            else if (it)
                open_item(S.sel, how);
            break;
        }
        case MI_DETAILS:
            if (it && !rel)
                det_show(S.sel);
            break;
        case MI_SUBS:
            choose_sub(sel[1]);
            break;
        case MI_SAVE:
            if (S.save.active)
                save_stop("Stopped saving.");
            else if (it && S.sv_ok && !too_big(S.sv_size, S.sv_title))
                report("To save it, move to the right of Save original file, then drag the icon "
                       "to a directory display.");
            break;
        case MI_SPEED:
            speed_start(it);
            break;
        case MI_REMOVE:
            if (it) {
                hourglass(1);
                if (plex_remove_continue(&S.px, it) != 0) {
                    hourglass(0);
                    report("Can't take it off Continue watching: %s", S.px.err);
                    break;
                }
                hourglass(0);
                if (S.home.on || !strncmp(S.path, "/library/onDeck", 15))
                    refresh_list(); /* it's gone from the row */
                set_status("Taken off Continue watching.");
            }
            break;
        case MI_WATCHED:
        case MI_UNWATCHED:
            if (it)
                mark(it, sel[0] == MI_WATCHED);
            break;
        case MI_RATE:
            if (it && sel[1] >= 0 && sel[1] <= 5)
                rate(det_is(it) ? det_item() : it, sel[1] ? 2 * sel[1] : -1);
            break;
        case MI_BACK:
            go_back();
            break;
        case MI_REFRESH:
            refresh_list();
            break;
        }
    } else if (kind == 3) {
        choose_sub(sel[0]);
    } else if (kind == 6 && S.lib.on) {
        if (sel[0] >= 0 && sel[0] < SORTS && sel[0] != S.lib.sort) {
            char path[256];
            lib_path(0, sel[0], S.lib.unwatched, path, sizeof(path));
            show_list(path, "", 0, 0);
        }
        return 0;
    } else if (kind == 5 && S.pl.on) {
        choose_psub(sel[0]);
    } else if (kind == 4 && S.pl.on) {
        switch (sel[0]) {
        case MP_AUDIO:
            choose_audio(sel[1]);
            break;
        case MP_SUBS:
            choose_psub(sel[1]);
            break;
        case MP_CHAPTERS:
            if (sel[1] >= 0 && S.pl.det.n && S.pl.chap_first + sel[1] < S.pl.det.v[0].nchapters)
                player_seek(S.pl.det.v[0].chapters[S.pl.chap_first + sel[1]].start_ms / 1000.0);
            break;
        case MP_VOLUME:
            if (sel[1] >= 0 && sel[1] < 5) {
                player_set_volume((100 - sel[1] * 25) / 100.0);
                S.volume = player_volume();
                choices_save();
            }
            break;
        case MP_PICTURE:
            if (sel[1] >= 0 && sel[1] < PIC_COUNT) {
                player_set_pic_mode(sel[1]);
                S.pic_mode = sel[1];
                choices_save();
            }
            break;
        case MP_STATS:
            player_set_stats(!player_stats());
            break;
        case MP_FULL:
            player_set_fullscreen(!player_fullscreen());
            break;
        case MP_MINI:
            player_set_mini(!player_mini());
            choices_save();
            break;
        case MP_ONTOP:
            player_set_ontop(!player_ontop());
            choices_save();
            break;
        case MP_OVERLAY:
            player_set_overlay(!player_overlay());
            S.overlay = player_overlay();
            choices_save();
            break;
        case MP_STOP:
            builtin_stop(1);
            return 0;
        }
    }
    /* Adjust keeps the menu open */
    r.r[1] = (intptr_t)b;
    if (!swi(Wimp_GetPointerInfo, &r) && (b[2] & 1)) {
        if (kind == 1)
            bar_menu_open(S.menu_x);
        else if (kind == 2 && S.browser_open)
            item_menu_open(S.menu_x, S.menu_y);
        else if (kind == 4 && S.pl.on) {
            player_menu_build();
            S.menu_kind = 4;
            open_menu(&m_play, S.menu_x - 64, S.menu_y);
        } else if (kind == 5 && S.pl.on) {
            psubs_menu_build();
            S.menu_kind = 5;
            open_menu(&m_psubs, S.menu_x - 64, S.menu_y + 44 * (m_psubs.n + 1));
        }
    }
    return 0;
}

/* The home page's featured part and rows' titles */
void home_action(int id)
{
    const plex_item *h = home_hero();
    if (id == HB_PLAY && h) {
        select_tile(S.home.row[0].start + S.home.pick);
        play_item(h, PLAY_DEFAULT);
    } else if (id == HB_DETAILS && h) {
        select_tile(S.home.row[0].start + S.home.pick);
        det_show(S.home.row[0].start + S.home.pick);
    } else if (id >= HB_DOT && id < HB_DOT + S.home.npick && id - HB_DOT != S.home.pick) {
        S.home.pick = id - HB_DOT;      /* another one featured */
        home_layout();
        hourglass(1);
        home_fetch_art();
        hourglass(0);
        force_redraw(S.browser_w, 0, -HEADER_H - S.home.hero_h, S.scr_w, -HEADER_H);
    } else if (id >= HB_ROW && id < HB_ROW + S.home.nrows && S.home.row[id - HB_ROW].path[0]) {
        char path[200], back[64];
        snprintf(path, sizeof(path), "%s", S.home.row[id - HB_ROW].path);
        snprintf(back, sizeof(back), "%s", S.list.title);
        show_list(path, back, 1, 0);
    }
}

/* A show: its page, with the series that has episodes not seen yet (or
   the first) */
static void show_open(const plex_item *it)
{
    plex_list d, se;
    char back[64], path[256];
    int k = 0, sel = 0;
    hourglass(1);
    if (plex_details(&S.px, it, &d) != 0 || !d.n) {
        hourglass(0);
        report("Can't get %s from the server: %s", it->title, S.px.err);
        return;
    }
    if (plex_list_get(&S.px, it->key, &se) != 0) {
        plex_list_free(&d);
        hourglass(0);
        report("Can't get its series: %s", S.px.err);
        return;
    }
    hourglass(0);
    plex_list_keep(&se, "season");  /* not Plex's "All episodes" */
    if (!se.n) {                    /* no series (Plex left them out) */
        plex_list_free(&d);
        plex_list_free(&se);
        snprintf(back, sizeof(back), "%s", S.list.title);
        snprintf(path, sizeof(path), "%s", it->key);
        show_list(path, back, 1, 0);
        return;
    }
    if (S.show.have) {
        plex_list_free(&S.show.det);
        plex_list_free(&S.show.seasons);
    }
    S.show.det = d;
    S.show.seasons = se;
    S.show.have = 1;
    S.show.art = S.show.poster = NULL;
    S.show.fw = 0;
    for (int i = 0; i < se.n; i++)
        if (se.v[i].unwatched > 0) {
            k = i;
            break;
        }
    snprintf(back, sizeof(back), "%s", S.list.title);
    snprintf(path, sizeof(path), "%s", se.v[k].key ? se.v[k].key : "");
    if (show_list(path, back, 1, 0) == 0 && S.show.on) {
        for (int i = 0; i < S.list.n; i++)  /* the episode to carry on with, selected */
            if (S.list.v[i].view_offset_ms > 0 || !S.list.v[i].watched) {
                sel = i;
                break;
            }
        S.sel = sel;
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    }
}

/* Another series of the show: its episodes, in place (Back still goes to
   where the show was opened from) */
void show_season(int k)
{
    char path[256];
    if (!S.show.on || k < 0 || k >= S.show.seasons.n || k == S.show.season)
        return;
    snprintf(path, sizeof(path), "%s", S.show.seasons.v[k].key ? S.show.seasons.v[k].key : "");
    show_list(path, "", 0, 0);
}

/* Play and Mark watched on the show page */
void show_action(int id)
{
    plex_item *it = &S.show.det.v[0];
    if (id == SH_PLAY && S.show.next < S.list.n) {
        select_tile(S.show.next);
        play_item(&S.list.v[S.show.next], PLAY_DEFAULT);
    } else if (id == SH_WATCHED) {
        int w = !it->watched;
        plex_list se;
        mark(it, w);
        it->watched = w;
        memset(&se, 0, sizeof(se));
        if (it->key && plex_list_get(&S.px, it->key, &se) == 0 &&
            (plex_list_keep(&se, "season"), se.n == S.show.seasons.n)) {
            plex_list_free(&S.show.seasons);    /* their unwatched counts */
            S.show.seasons = se;
        } else if (se.n || se.v)
            plex_list_free(&se);
        refresh_list();             /* the episodes' ticks */
    }
}

void open_item(int i, int how)
{
    const plex_item *it;
    if (i < 0 || i >= S.list.n)
        return;
    it = &S.list.v[i];
    if (it->kind == PI_FOLDER && it->type && !strcmp(it->type, "show") && it->key && it->rating_key) {
        show_open(it);
    } else if (it->kind == PI_FOLDER && it->type && !strcmp(it->type, "season") && it->grandparent_key) {
        plex_item show;                 /* a series (recently added): its show's page */
        char key[160];
        memset(&show, 0, sizeof(show));
        snprintf(key, sizeof(key), "/library/metadata/%s/children", it->grandparent_key);
        show.kind = PI_FOLDER;
        show.type = (char *)"show";
        show.rating_key = it->grandparent_key;
        show.key = key;
        show.title = it->grandparent_title ? it->grandparent_title : it->title;
        show_open(&show);
    } else if (it->kind == PI_FOLDER) {
        char path[256], back[64];
        snprintf(path, sizeof(path), "%s", it->key ? it->key : "");
        snprintf(back, sizeof(back), "%s", S.list.title);
        show_list(path, back, 1, 0);
    } else if (it->kind == PI_VIDEO) {
        play_item(it, how);
    } else if (S.px.kind == SRV_DLNA) {     /* a song or a picture on a DLNA server */
        set_status("%s isn't played: films and TV only.", it->subtitle && *it->subtitle ? it->subtitle : "That");
    } else {
        set_status("%s libraries can't be opened yet: films and TV only.", it->subtitle ? it->subtitle : "These");
    }
}
