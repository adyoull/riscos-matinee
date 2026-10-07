#!/bin/sh
# mutate.sh - do the host tests notice when the code is wrong? Each
# mutation below breaks one thing on purpose, in a copy of the tree; the
# tests (run.sh) must then fail. A mutation the tests pass is a gap.
#   REEL_SRC=<riscos-ffmpeg checkout> [QEMU=...] tests/host/mutate.sh
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/matinee-mutate}
REEL_SRC=${REEL_SRC:-$TOP/../riscos-ffmpeg}     # at v5.1.10-riscos16
export REEL_SRC
survived=0
n=0

# mutate NAME FILE OLD NEW: OLD (exactly once in FILE) becomes NEW
mutate() {
  n=$((n + 1))
  rm -rf "$WORK"
  mkdir -p "$WORK"
  cp -r "$TOP/src" "$TOP/tests" "$TOP/third_party" "$TOP/app" "$WORK/"
  python3 - "$WORK/$2" "$3" "$4" <<'EOF'
import sys
path, old, new = sys.argv[1], sys.argv[2].encode().decode('unicode_escape'), sys.argv[3].encode().decode('unicode_escape')
s = open(path).read()
if s.count(old) != 1:
    sys.exit("mutation not found once in %s: %r" % (path, old))
open(path, 'w').write(s.replace(old, new))
EOF
  if [ $? != 0 ]; then              # the code has moved on: the mutation needs updating
    echo "STALE: $1"
    survived=$((survived + 1))
    return
  fi
  if OUT=$WORK/out PORT=$((18500 + n)) sh "$WORK/tests/host/run.sh" > "$WORK/log" 2>&1; then
    echo "SURVIVED: $1"
    survived=$((survived + 1))
  else
    echo "caught: $1 ($(grep -c '^FAIL' "$WORK/log") failures)"
  fi
}

# the core (core_test)
mutate "IPv6 addresses kept" src/plex.c \
  'if (jbool(k, "IPv6"))\n                continue;' ''
mutate "connection ranks swapped (https before http on the local network)" src/plex.c \
  'return k->https ? 10 : 0;' 'return k->https ? 0 : 10;'
mutate "Accept: JSON handed to Reel" src/handoff.c \
  'if (strcmp(name, "Accept"))' 'if (1)'
mutate "10-bit H.264 played directly" src/caps.c \
  '(strstr(it->vprofile, "10") || strstr(it->vprofile, "4:"))' '(strstr(it->vprofile, "4:"))'
# the front end (ui_test)
mutate "DataOpen broadcast, not sent to the player" src/ui.c \
  'r.r[2] = task;              /* to that task' 'r.r[2] = 0;              /* to that task'
mutate "\"Reel\" finds \"ReelEGL\"" src/ui.c \
  '!strncmp(t, name, n) && (unsigned char)t[n] < 32' '!strncmp(t, name, n)'
mutate "no 4GB refusal" src/ui.c \
  'if (size <= FILE_MAX)\n        return 0;' 'return 0;'
mutate "hand-off file kept after DataLoadAck" src/ui.c \
  'remove(S.pend[i].path);\n            set_status' 'set_status'
mutate "1MB saved a null event" src/ui.c \
  '#define SAVE_STEP (256 * 1024)' '#define SAVE_STEP (1024 * 1024)'
mutate "Play from start keeps Reel's carry-on key" src/ui.c \
  'p.key[0] = 0;               /* not where' '(void)0;               /* not where'
mutate "Resume doesn't turn direct play off" src/ui.c \
  'if (how == PLAY_RESUME && S.px.kind != SRV_DLNA)\n        allow = 0;' ''
mutate "posters drawn on the screen (output not switched)" src/ui.c \
  'r.r[2] = (intptr_t)(area + 4);\n    r.r[3] = 0;' 'r.r[2] = 0;\n    r.r[3] = 0;'
mutate "subtitles chosen, yet played directly" src/caps.c \
  'if (allow_direct && plex_sub_selected(it) >= 0 &&' 'if (0 &&'
mutate "the subtitle track chosen isn't sent" src/ui.c \
  '(&S.px, it, k ? it->subs[k - 1].id : 0)' '(&S.px, it, 0)'
mutate "Back from the details forgets where the grid was" src/ui.c \
  'open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, S.grid_sy);' 'open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);'
mutate "Right doesn't go to the next video's details" src/ui.c \
  'det_show(j);\n                        break;' 'break;'
mutate "typing ignored on the sign-in page" src/ui.c \
  'f[n] = (char)k;\n            f[n + 1] = 0;' '(void)0;'
mutate "the shapes' edges not smoothed" src/draw.c \
  'n += inside(g, (x + (i + 0.5) / 4) / w, (y + (j + 0.5) / 4) / h);' 'n = 16 * inside(g, (x + 0.5) / w, (y + 0.5) / h);'
mutate "the pointer's poster not found" src/ui.c \
  'set_hover(p[3] == S.browser_w && S.page == PG_GRID ? tile_at(p[0], p[1]) : -1);' 'set_hover(-1);'
mutate "backdrop not faded" src/ui.c \
  '    } else if (art == 1) {\n        sprite_fade(p->area, w, h);\n    }' '    }'
mutate "posters not rounded (no mask)" src/ui.c \
  'art == 1 ? 0 : 12 >> S.xeig' '0'
mutate "the speed test's time limit ignored" src/ui.c \
  'if (now_cs() - S.speed.t0 >= SPEED_TIME || S.speed.done' 'if (S.speed.done'
mutate "the speed in bytes, not bits" src/ui.c \
  'S.speed.done * 8.0 /' 'S.speed.done * 1.0 /'
mutate "no MB/s while saving" src/ui.c \
  'if (cs >= 100)' 'if (0)'
# the built-in player
mutate "the overlay not clipped above the bar (full screen)" src/player.c \
  'if (P.fullscreen && P.bar_shown)\n        c.y0 = P.bar.y1;' ''
mutate "the server not told of a pause" src/ui.c \
  'case PE_PAUSED:\n        builtin_timeline("paused");' 'case PE_PAUSED:'
mutate "no next episode" src/ui.c \
  'if (plex_next_episode(&S.px, it, &S.pl.next) == 0 && S.pl.next.n) {' 'if (0) {'
mutate "Accept: JSON given to reelcore" src/ui.c \
  'if (strncmp(in, "Accept:", 7) && o + n < size) {' 'if (o + n < size) {'
# (test28: "sleeping while buffering" went: net_feed's no-sleep covers buffering too, so
# player_poll_cs's own check can go without a test noticing; both kept)
mutate "Resume ignored" src/player.c \
  'if (P.start > 0)\n        reelcore_seek(P.v, P.start - P.base);' ''
mutate "Stretch not given to the overlay" src/player.c \
  'if (P.pic_mode != PIC_STRETCH) {' 'if (1) {'
mutate "the player page scrolls" src/ui.c \
  '        b[5] = 0;\n        b[6] = 0;\n' ''
mutate "sync counted from the stream's start" src/player.c \
  '(int)((st.position - st.clock - P.sync0) * 1000)' '(int)((st.position - st.clock) * 1000)'
# metadata, search, the dashboard
mutate "the cast's photos not fetched" src/ui.c \
  'if (!th || S.cast[i].photo)\n            continue;' 'continue;'
mutate "cast photos square" src/ui.c \
  'int round = art == 2 ? w / 2 :' 'int round = art == 2 ? 0 :'
mutate "the timeline without the play queue" src/plex.c \
  'if (pl && pl->pq_id && u < sizeof(url))' 'if (0)'
mutate "the stream without the session id" src/ui.c \
  'if (S.px.kind == SRV_PLEX && strlen(headers) + strlen(S.pl.p.session) + 40 < sizeof(headers))' 'if (0)'
mutate "a search on every key" src/ui.c \
  'S.search_due = now_cs() + SEARCH_WAIT;\n    if (!S.search_due)\n        S.search_due = 1;' 'search_now();\n    return 1;'
mutate "search results in the server's order" src/plex.c \
  '{ "movie", "show", "episode" }' '{ "episode", "movie", "show" }'
mutate "Back to a search forgets its words" src/ui.c \
  'latin1(path + 7, S.query, sizeof(S.query));' '(void)0;'
# the image cache
mutate "the image cache not read" src/ui.c \
  'if (imgcache_get(key, jpeg, len) == 0) {\n        if (imgcache_jpeg_whole(*jpeg, *len))' 'if (0) {\n        if (imgcache_jpeg_whole(*jpeg, *len))'
mutate "the cache trimmed in any order" src/imgcache.c \
  'qsort(l.v, l.n, sizeof(*l.v), older);' '(void)older;'
mutate "Clear image cache deletes nothing" src/imgcache.c \
  '        gone += remove(path) == 0;' '        gone += 0;'
# test8: in-stream seeking, the keep-alive, the pointer, the panel, sign out
mutate "a new sound track keeps the old conversion" src/ui.c \
  'if (*S.pl.sid && !S.pl.p.direct)\n            plex_transcode_stop(&S.px, S.pl.sid);' ''
mutate "no keep-alive while paused" src/ui.c \
  'plex_transcode_ping(&S.px, S.pl.sid);' '(void)0;'
mutate "the pointer never hidden" src/player.c \
  '        pointer_show(0);' '        (void)0;'
mutate "the panel not scaled for the overlay" src/player.c \
  'reelcore_set_yuv_scale(P.v, (double)ov.fw / rw);' '(void)rw;'
mutate "Sign out without asking" src/ui.c \
  '            if (ask(q))' '            if (1 || ask(q))'

mutate "the stats say Direct Play for a transcoded stream" src/player.c \
  'P.convert ? "Transcoded" : "Direct Play"' '"Direct Play"'

mutate "the built-in player's subtitles burnt in anyway" src/ui.c \
  'k.own_subs = 1;' 'k.own_subs = 0;'
mutate "a subtitle file beside the video not fetched" src/ui.c \
  'if (t < 0 && sub_fetch(sb, path, sizeof(path)) == 0) {' 'if (0) {'
mutate "subtitle files left in the scrap directory" src/ui.c \
  'remove(S.pl.ext[i].path);' '(void)0;'
mutate "external picture subtitles drawn by the player" src/caps.c \
  'sb->external ? sub_file : sub_in_file' 'sub_in_file'

mutate "the player's subtitle choice isn't sent" src/ui.c \
  'id = k ? it->subs[k - 1].id : 0;' 'id = 0;'
mutate "a new subtitle track in a converted stream keeps the old conversion" src/ui.c \
  'player_note(k ? "Changing the subtitles..." : "Subtitles off...");\n    if (builtin_open_at(player_position(), 1) != 0)' 'if (0)'

# test10: the window's size, the mini player, the backdrop
mutate "the window not centred" src/ui.c \
  'int x0 = (S.scr_w - w - SCROLL_W) / 2,' 'int x0 = 0,'
mutate "the mini player decodes in full" src/player.c \
  'P.mini ? REELCORE_FAST_LIGHT : REELCORE_FAST_OFF' 'REELCORE_FAST_OFF'
mutate "the mini player's grip loses the video's shape" src/player.c \
  'b[2] = b[4] - (mini_pic_h(vw) + MINI_BAR);' '(void)0;'
mutate "Normal doesn't open the window again" src/player.c \
  'open_at(P.win, P.main_st[1], P.main_st[2], P.main_st[3], P.main_st[4], -1);' '(void)0;'
mutate "the backdrop fetched on every resize" src/ui.c \
  'art_due = now_cs() + 50;' 'art_due = now_cs();'

# test11: the new look
mutate "no watched tick" src/ui.c \
  'if (it->watched && it->rating_key && it->kind != PI_OTHER)\n        return 1;' 'if (0)\n        return 1;'
mutate "a show's unwatched count not shown" src/ui.c \
  'if (it->kind == PI_FOLDER && it->unwatched > 0)\n        return 2;' 'if (0)\n        return 2;'
mutate "a show opens as a grid of series" src/ui.c \
  'if (it->kind == PI_FOLDER && it->type && !strcmp(it->type, "show") && it->key && it->rating_key) {' 'if (0) {'
mutate "the show page opens on the first series, not the one to watch" src/ui.c \
  'if (se.v[i].unwatched > 0) {\n            k = i;' 'if (0) {\n            k = i;'
mutate "another series pushes history" src/ui.c \
  'show_list(path, "", 0, 0);\n}\n\n/* Play and Mark watched' 'show_list(path, "", 1, 0);\n}\n\n/* Play and Mark watched'
mutate "the details poster not fetched" src/ui.c \
  'S.det_poster = poster_fetch(det_poster_thumb(it), key, det_pw >> S.xeig, det_ph >> S.yeig, 0);' '(void)0;'

# test12: a real show's children start with "All episodes"
mutate "All episodes taken for a series" src/ui.c \
  'plex_list_keep(&se, "season");  /* not' '(void)0;  /* not'
mutate "age ratings keep their country" src/ui.c \
  "latin1(strchr(it->content_rating, '/') ? strchr(it->content_rating, '/') + 1 : it->content_rating, g,\n               sizeof(g));             /* \"gb/18\": 18 */\n        show_chip" "latin1(it->content_rating, g, sizeof(g));\n        show_chip"

# test13: the home page
mutate "the top as the old list" src/ui.c \
  '    S.home.on = !*path;' '    S.home.on = 0;'
mutate "a row shows all, not what fits" src/ui.c \
  'S.home.lay[r].vis = fit < S.home.row[r].n ? fit : S.home.row[r].n;' 'S.home.lay[r].vis = S.home.row[r].n;'
mutate "the featured backdrop not fetched again after a resize" src/ui.c \
  'if (S.home.art && S.home.fw != w)\n            S.home.due = now_cs() + 50;' ''

# test14: tabs, Adjust, Remove from Continue watching
mutate "no tabs" src/ui.c \
  'return S.page != PG_SIGNIN && S.page != PG_PLAYER && S.libs.n > 0 ? STRIP_H : 0;' 'return 0;'
mutate "Adjust only selects" src/ui.c \
  'if ((buttons & 2) || ((buttons & 0x100) && t >= 0)) {' 'if (buttons & 2) {'
mutate "Remove from Continue watching not sent" src/ui.c \
  'if (plex_remove_continue(&S.px, it) != 0) {' 'if (0) {'

# test15: a library's bar, More like this
mutate "A to Z ignores The" src/ui.c \
  'if (!strncasecmp(t, "The ", 4)) t += 4;' 'if (!strncasecmp(t, "The ", 4)) t += 0;'
mutate "Unwatched not asked for" src/ui.c \
  'unwatched ? (sort ? "&unwatched=1" : "unwatched=1") : ""' '""'
mutate "More like this not fetched" src/ui.c \
  'snprintf(path, sizeof(path), "/library/metadata/%s/%s", v->rating_key, k == REL_EXTRAS ? "extras" : "similar");' 'snprintf(path, sizeof(path), "/nowhere");'

# test16: skip intro and credits, chapters
mutate "a marker's end counts as in it" src/plex.c \
  'if (t >= it->markers[i].start_ms && t < it->markers[i].end_ms)' 'if (t >= it->markers[i].start_ms && t <= it->markers[i].end_ms)'
mutate "Skip credits only skips them" src/ui.c \
  'if (k->type == PM_CREDITS && (k->final' 'if (0 && (k->final'
mutate "Page Up always goes to this chapter's start" src/ui.c \
  'else if (c >= 0 && t - it->chapters[c].start_ms / 1000.0 < 3)' 'else if (0)'

# test19: the overlay's box, your rating, Plex Home
mutate "the box left alone under the overlay" src/player.c \
  '} else if (!update)\n            /* the overlay shows' '} else if (0)\n            /* the overlay shows'
mutate "the same star doesn't take the rating away" src/ui.c \
  'it->user_rating < r + 1 ? -1 : r);' 'it->user_rating < r + 1 ? r : r);'
mutate "the server's token not fetched again after a switch" src/ui.c \
  '    if (use_server(k) != 0)\n        return;\n    if (S.libs.n' '    if (0)\n        return;\n    if (S.libs.n'

# performance (after test19): the bar's parts, hit-testing, lazy fitting, gzip
mutate "the whole bar redrawn as the time changes" src/player.c \
  'update_box(top_line(right - (w > ow ? w : ow) - 8, right + 4));' 'update_box(P.bar);'
mutate "the grid's hit-test always the first row" src/ui.c \
  'row = (top - wy) / (TILE_H + GAP);' 'row = 0;'
mutate "grid lines never cut to fit" src/ui.c \
  '    draw_fit(D_BOLD, d->line[0], TILE_W);\n    draw_fit(D_BODY, d->line[1], TILE_W);\n    d->fitted = 1;' '    d->fitted = 1;'
mutate "lists not asked for compressed" src/plex.c \
  'snprintf(out + n, size - n, "Accept-Encoding: gzip\\r\\n");' '(void)0;'

# test21: crops
mutate "tall pictures cut through the middle" src/ui.c \
  'r.r[2] = (th > h ? (h - th) * 4 / 5 : (h - th) / 2) << S.yeig;' 'r.r[2] = ((h - th) / 2) << S.yeig;'

# test22: the decoder in the stats
mutate "the Decoder row says ARM always" src/player.c \
  'st.decoder == REELCORE_DECODER_VIDEOCORE ? "VideoCore (hardware)" :' '0 ? "VideoCore (hardware)" :'

# test24: the page at &8000
mutate "a moved page at &8000 not noticed" src/player.c \
  'if (p && P.app_page && p != P.app_page) {' 'if (0) {'

# test25: Jellyfin
mutate "Jellyfin's header without the token" src/jellyfin.c \
  'if (token && *token && n > 0 && (size_t)n < size) {' 'if (0) {'
mutate "a JSON body sent as a form" src/net.c \
  '*body == '"'"'{'"'"' || *body == '"'"'['"'"' ? "application/json"' '0 ? "application/json"'
mutate "stream ids are Jellyfin's Index (0 taken as none)" src/jellyfin.c \
  '            sb->id = idx + 1;' '            sb->id = idx;'
mutate "missing episodes listed" src/jellyfin.c \
  'if (!strcmp(ty, "Episode") && loc && !strcmp(loc, "Virtual"))' 'if (0)'
mutate "Next up not in Continue watching" src/jellyfin.c \
  'snprintf(path, sizeof(path), "/Shows/NextUp?UserId=%s&Limit=%d&Fields=" ROW_FIELDS IMAGES, c->user_id, size);\n    if (get_items(c, path, 1, &nx) != 0)' 'if (1)'
mutate "progress always reported as a start" src/jellyfin.c \
  '} else if (strcmp(playing_session, sess)) {' '} else if (1) {'
mutate "the details page asks the server how it plays" src/ui.c \
  '    k.dry = 1;                      /* nothing asked of the server for it */' ''
mutate "the session id stays Matinee's (Jellyfin's PlaySessionId not used)" src/ui.c \
  '    snprintf(S.pl.sid, sizeof(S.pl.sid), "%s", S.pl.p.session);\n' ''
mutate "a Plex server keeps Jellyfin's kind" src/plex.c \
  '    c->kind = SRV_PLEX;\n    c->user_id[0] = c->user_name[0] = 0;\n    {\n        /* the scheme' '    {\n        /* the scheme'
mutate "signing out of Jellyfin forgets nothing" src/ui.c \
  '                memmove(&S.jf[i], &S.jf[i + 1], (S.njf - i - 1) * sizeof(jf_saved));\n                S.njf--;' ''
mutate "stars shown for Jellyfin" src/ui.c \
  'if (it->rating_key && S.px.kind == SRV_PLEX && S.nbtn + 5 <= DET_BTN_MAX)' 'if (it->rating_key && S.nbtn + 5 <= DET_BTN_MAX)'

# test26: Add a server; the HEVC block
mutate "Cancel always goes to the home page" src/ui.c \
  'S.page = S.si_prev == PG_DETAILS ? PG_DETAILS : PG_GRID;' 'S.page = PG_GRID;'
mutate "Escape doesn't cancel" src/ui.c \
  'return 4;                   /* Escape: Cancel */' 'return 0;'
mutate "the HEVC block not used for direct play" src/caps.c \
  'if (hevc && k->hevc) {' 'if (0) {'
mutate "4K HEVC at 60 a second played as it is" src/caps.c \
  'if (it->height > 1088 && it->fps > k->max_fps_1080)' 'if (0)'
mutate "Matinee\$NoHEVCBlock not given to reelcore" src/player.c \
  'REELCORE_NO_HEVC_BLOCK : 0' '0 : 0'

# test27: the reader given the time between pictures
mutate "asleep between pictures with the read-ahead short" src/player.c \
  '    P.idle_cs = 0;                  /* straight back: Wimp_Poll, not PollIdle */' ''
mutate "the reader fed until full, never stopping" src/player.c \
  '} else if (P.feeding && ns.ahead >= FEED_STOP) {' '} else if (0) {'

# test28: the audit
mutate "the PIN id in a 32-bit long" src/ui.c \
  '    long long pin_id;' '    long pin_id;'
mutate "window_state's block left as it was when refused" src/ui.c \
  '    memset(st, 0, 9 * sizeof(int));     /* all callers'"'"' blocks are 9 words; zero if the Wimp refuses */\n    st[0] = w;\n    r.r[1] = (intptr_t)st;\n    if (swi(Wimp_GetWindowState, &r)) {\n        memset(st, 0, 9 * sizeof(int));\n        st[0] = w;\n    }' '    st[0] = w;\n    r.r[1] = (intptr_t)st;\n    swi(Wimp_GetWindowState, &r);'

# test29: posters cut short; the collection
mutate "a poster cut short taken for a whole one" src/imgcache.c \
  '        if (d[n - 2] == 0xFF && d[n - 1] == 0xD9)\n            return 1;\n    return 0;' '        if (d[n - 2] == 0xFF && d[n - 1] == 0xD9)\n            return 1;\n    return 1;'
mutate "the collection's row left out" src/plex.c \
  '            it->collection = dup_s(jstr(col, "tag"));' '            it->collection = NULL;'
mutate "the film itself in its collection's row" src/ui.c \
  '                if (c->v[i].rating_key && !strcmp(c->v[i].rating_key, v->rating_key)) {' '                if (0) {'

# test31: the details page's rows drawn in strips
mutate "a details row left out of a redraw below its heading" src/ui.c \
  '        if (!S.nrel[k] || S.relc[k][0].y0 - 100 > cy1 || S.rel_y[k] + 60 < cy0)' '        if (!S.nrel[k] || S.rel_y[k] - 60 > cy1 || S.relc[k][0].y0 - 100 > cy1 || S.rel_y[k] + 60 < cy0)'
mutate "the grid's posters looked for under the pointer on the details" src/ui.c \
  '            set_hover(p[3] == S.browser_w && S.page == PG_GRID ? tile_at(p[0], p[1]) : -1);' '            set_hover(p[3] == S.browser_w ? tile_at(p[0], p[1]) : -1);'

# test32: a whole library, a page at a time
mutate "a library's later pages never fetched" src/ui.c \
  '    if (S.more_wanted && list_more_step())\n        return;\n' ''
mutate "Plex's later pages asked from the start" src/plex.c \
  'c->base, path, strchr(path, '"'"'?'"'"') ? "&" : "?", start, size);' 'c->base, path, strchr(path, '"'"'?'"'"') ? "&" : "?", 0, size);'
mutate "Jellyfin's later pages asked from the start" src/jellyfin.c \
  '"&Filters=IsUnplayed" : "", fields, IMAGES, start, size);' '"&Filters=IsUnplayed" : "", fields, IMAGES, 0, size);'
mutate "Refresh's selection in a later page forgotten" src/ui.c \
  '    S.sel_want = sel >= l.n && sel < l.total ? sel : -1;' '    S.sel_want = -1;'
mutate "A to Z not made again with a new page" src/ui.c \
  '        lib_layout();\n        force_redraw(S.browser_w, 0, -HEADER_H - libbar_h(), S.scr_w, -HEADER_H);' '        force_redraw(S.browser_w, 0, -HEADER_H - libbar_h(), S.scr_w, -HEADER_H);'
mutate "an empty page leaves more to ask for" src/plex.c \
  '        l->total = l->n;\n        return 0;' '        return 0;'
mutate "a later page's items not shown as fetched" src/ui.c \
  '    S.more_wanted = l.total > l.n && *path && strncmp(path, "search:", 7) && !S.home.on && !S.show.on;' '    S.more_wanted = 0;'

# test36: DLNA servers
mutate "SSDP answers' LOCATION not read" src/dlna.c \
  'if (!strncasecmp(l, "LOCATION:", 9)) {' 'if (!strncasecmp(l, "LOCATIONX:", 10)) {'
mutate "a relative control address taken from the host" src/dlna.c \
  '    if (*rel == '"'"'/'"'"') {\n        snprintf(out, size, "%s%s", o, rel);' '    if (1) {\n        snprintf(out, size, "%s/%s", o, rel + (*rel == '"'"'/'"'"'));'
mutate "a device with no ContentDirectory taken" src/dlna.c \
  'if (!strcmp(s->name, "service") && t && strstr(t, ":service:ContentDirectory:")) {' 'if (!strcmp(s->name, "service") || !t || 1) {'
mutate "object ids not escaped in keys" src/dlna.c \
  '    net_escape(id, eid, sizeof(eid));\n    it->rating_key' '    snprintf(eid, sizeof(eid), "%s", id);\n    it->rating_key'
mutate "Music and Pictures kept as libraries" src/dlna.c \
  '    for (int i = 0; title && no[i]; i++)' '    for (int i = 0; title && no[i] && 0; i++)'
mutate "a DLNA folder not paged" src/dlna.c \
  '    out->total = meta ? out->n : total > start + out->n ? total : start + out->n;' '    out->total = out->n;'
mutate "a converted stream bigger than the Quality chosen" src/dlna.c \
  '        if ((r[i].w <= k->max_w && r[i].h <= k->max_h) && (best < 0' '        if ((1) && (best < 0'
mutate "4K with the HEVC block not taken for HEVC" src/dlna.c \
  '        if (k->hevc && t->width <= CAPS_HEVC_W' '        if (0 && k->hevc && t->width <= CAPS_HEVC_W'
mutate "DLNA watched only at the very end" src/dlna.c \
  '    if (p->dur > 0 && time_ms >= p->dur * 9 / 10) {' '    if (p->dur > 0 && time_ms >= p->dur) {'
mutate "DLNA places not read again" src/dlna.c \
  '    nplaces = places_read = 0;' '    nplaces = 0;'
mutate "SOAPACTION without its quotes" src/dlna.c \
  'snprintf(headers, sizeof(headers), "SOAPACTION: \\"%s#%s\\"\\r\\n", svc, action);' 'snprintf(headers, sizeof(headers), "SOAPACTION: %s#%s\\r\\n", svc, action);'
mutate "XML &amp; not decoded" src/xml.c \
  'else if (len == 4 && !strncmp(s, "&amp", 4)) c = '"'"'&'"'"';' 'else if (len == 4 && !strncmp(s, "&amp", 4)) c = 0;'
mutate "a DLNA server in Choices taken for Plex" src/ui.c \
  'atoi(v) == SRV_DLNA ? SRV_DLNA : SRV_PLEX;' 'SRV_PLEX;'
mutate "Return on the DLNA page not Use" src/ui.c \
  'return S.si_mode == SI_JF ? 3 : S.si_mode == SI_DLNA ? 5 : 2;' 'return S.si_mode == SI_JF ? 3 : 2;'
mutate "a DLNA server's home page without its libraries" src/plex.c \
  '    if (c->kind == SRV_DLNA && secs.n && nr < max) {' '    if (0) {'
mutate "a DLNA poster that isn't a JPEG taken" src/dlna.c \
  '    if (b.len < 4 || (unsigned char)b.data[0] != 0xFF || (unsigned char)b.data[1] != 0xD8) {' '    if (b.len < 4) {'

# test37: Forget a server
mutate "a server forgotten without asking" src/ui.c \
  '    if (!ask(q))\n        return;                     /* not confirmed: nothing forgotten */' '    ask(q);'
mutate "a Jellyfin server forgotten but not signed out of" src/ui.c \
  '        hourglass(1);\n        jf_logout(&c);\n        hourglass(0);\n        snprintf(name, sizeof(name), "%s", S.jf[k].name);' '        snprintf(name, sizeof(name), "%s", S.jf[k].name);'
mutate "the server in use forgotten but still shown" src/ui.c \
  '    if (in_use) {\n        sign_out();\n        return;\n    }' '    if (0) {\n        sign_out();\n        return;\n    }'

rm -rf "$WORK"
echo "$n mutations, $survived survived"
[ "$survived" = 0 ]
