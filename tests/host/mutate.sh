#!/bin/sh
# mutate.sh - do the host tests notice when the code is wrong? Each
# mutation below breaks one thing on purpose, in a copy of the tree; the
# tests (run.sh) must then fail. A mutation the tests pass is a gap.
#   REEL_SRC=<riscos-ffmpeg checkout> [QEMU=...] tests/host/mutate.sh
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/matinee-mutate}
REEL_SRC=${REEL_SRC:-$TOP/../riscos-ffmpeg}     # at reel-0.1.21 (riscos14)
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
  'if (how == PLAY_RESUME)\n        allow = 0;' ''
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
  'set_hover(p[3] == S.browser_w ? tile_at(p[0], p[1]) : -1);' 'set_hover(-1);'
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
mutate "sleeping while buffering" src/player.c \
  'if (reelcore_net(P.v, &ns) && (ns.buffering || ns.opening))\n        return 0;' ''
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
  'if (strlen(headers) + strlen(S.pl.p.session) + 40 < sizeof(headers))' 'if (0)'
mutate "a search on every key" src/ui.c \
  'S.search_due = now_cs() + SEARCH_WAIT;\n    if (!S.search_due)\n        S.search_due = 1;' 'search_now();\n    return 1;'
mutate "search results in the server's order" src/plex.c \
  '{ "movie", "show", "episode" }' '{ "episode", "movie", "show" }'
mutate "Back to a search forgets its words" src/ui.c \
  'latin1(path + 7, S.query, sizeof(S.query));' '(void)0;'
# the image cache
mutate "the image cache not read" src/ui.c \
  'if (imgcache_get(key, jpeg, len) == 0)\n        return 0;' ''
mutate "the cache trimmed in any order" src/imgcache.c \
  'qsort(l.v, l.n, sizeof(*l.v), older);' '(void)older;'
mutate "Clear image cache deletes nothing" src/imgcache.c \
  'if (remove(path) == 0) {\n        (*gone)++;' 'if (0) {\n        (*gone)++;'
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
  'if (ask("Sign out?' 'if (1 || ask("Sign out?'

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
  'if (!*path)                     /* the top: the home page */\n        e = plex_home(&S.px, &l, rows, 8, &nrows);\n    else\n        e' 'e'
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
  'if (!strncasecmp(t, "The ", 4)) t += 4;' ''
mutate "Unwatched not asked for" src/ui.c \
  'unwatched ? (sort ? "&unwatched=1" : "unwatched=1") : ""' '""'
mutate "More like this not fetched" src/ui.c \
  'snprintf(path, sizeof(path), "/library/metadata/%s/%s", S.det.v[0].rating_key, k ? "extras" : "similar");' 'snprintf(path, sizeof(path), "/nowhere");'

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

rm -rf "$WORK"
echo "$n mutations, $survived survived"
[ "$survived" = 0 ]
