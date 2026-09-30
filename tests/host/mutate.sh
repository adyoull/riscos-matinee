#!/bin/sh
# mutate.sh - do the host tests notice when the code is wrong? Each
# mutation below breaks one thing on purpose, in a copy of the tree; the
# tests (run.sh) must then fail. A mutation the tests pass is a gap.
#   REEL_SRC=<riscos-ffmpeg checkout> [QEMU=...] tests/host/mutate.sh
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/plexro-mutate}
REEL_SRC=${REEL_SRC:-$TOP/../riscos-ffmpeg}
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
  'if (allow_direct && plex_sub_selected(it) >= 0) {' 'if (0) {'
mutate "the subtitle track chosen isn't sent" src/ui.c \
  'k ? it->subs[k - 1].id : 0' '0'
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
  'art ? 0 : 12 >> S.xeig' '0'
mutate "the speed test's time limit ignored" src/ui.c \
  'if (now_cs() - S.speed.t0 >= SPEED_TIME || S.speed.done' 'if (S.speed.done'
mutate "the speed in bytes, not bits" src/ui.c \
  'S.speed.done * 8.0 /' 'S.speed.done * 1.0 /'
mutate "no MB/s while saving" src/ui.c \
  'if (cs >= 100)' 'if (0)'
# the built-in player
mutate "the overlay not clipped above the bar (full screen)" src/player.c \
  'if (P.fullscreen && P.bar_shown)\n        c.y0 = P.bar.y1;' ''
mutate "a converted stream seeked in place" src/player.c \
  'if (P.convert) {                /* the caller' 'if (0) {                /* the caller'
mutate "the server not told of a pause" src/ui.c \
  'case PE_PAUSED:\n        builtin_timeline("paused");' 'case PE_PAUSED:'
mutate "no next episode" src/ui.c \
  'if (plex_next_episode(&S.px, it, &S.pl.next) == 0 && S.pl.next.n) {' 'if (0) {'
mutate "Accept: JSON given to reelcore" src/ui.c \
  'if (strncmp(in, "Accept:", 7) && o + n < size) {' 'if (o + n < size) {'
mutate "sleeping while buffering" src/player.c \
  'if (reelcore_net(P.v, &ns) && (ns.buffering || ns.opening))\n        return 0;' ''
mutate "Resume ignored in direct play" src/player.c \
  'if (P.start > 0 && !P.convert)\n        reelcore_seek(P.v, P.start - P.base);' ''
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

rm -rf "$WORK"
echo "$n mutations, $survived survived"
[ "$survived" = 0 ]
