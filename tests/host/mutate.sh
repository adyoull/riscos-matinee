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

rm -rf "$WORK"
echo "$n mutations, $survived survived"
[ "$survived" = 0 ]
