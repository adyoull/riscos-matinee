#!/bin/sh
# run.sh - PlexRO's host tests (Linux, the system FFmpeg).
#   REEL_SRC=<riscos-ffmpeg checkout> tests/host/run.sh
# Needs: gcc, pkg-config, libavformat-dev, python3. Reel's player/sources.c
# is compiled in, so the hand-off JSON is checked by Reel's own reader.
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
REEL_SRC=${REEL_SRC:-$TOP/../riscos-ffmpeg}
OUT=${OUT:-/tmp/plexro-tests}
PORT=${PORT:-18411}
[ -f "$REEL_SRC/player/sources.c" ] || { echo "run.sh: set REEL_SRC to a riscos-ffmpeg checkout" >&2; exit 2; }
mkdir -p "$OUT"
gcc -O1 -g -Wall -Wno-format-truncation -I"$TOP/src" -I"$TOP/third_party/cjson" -I"$REEL_SRC/player" \
  -o "$OUT/core_test" "$TOP/tests/host/core_test.c" "$TOP"/src/net.c "$TOP"/src/plex.c \
  "$TOP"/src/caps.c "$TOP"/src/handoff.c "$TOP/third_party/cjson/cJSON.c" "$REEL_SRC/player/sources.c" \
  $(pkg-config --cflags --libs libavformat libavutil)
python3 "$TOP/tests/host/fakeplex.py" "$PORT" > "$OUT/fakeplex.log" 2>&1 &
FP=$!
trap 'kill $FP 2>/dev/null' EXIT
sleep 1.5
timeout 120 "$OUT/core_test" "$PORT"
