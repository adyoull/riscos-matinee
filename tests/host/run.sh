#!/bin/sh
# run.sh - Matinee's host tests.
#   REEL_SRC=<riscos-ffmpeg checkout> tests/host/run.sh
#
# 1. core_test (Linux, the system FFmpeg): net, plex, caps and handoff
#    against fakeplex.py, with Reel's own player/sources.c reading the
#    hand-off.
# 2. ui_test: the front end (src/ui.c) against a scripted fake Wimp and the
#    same fake server. Built for arm-linux and run under qemu-arm, because
#    Wimp blocks hold 32-bit pointers; tests/host/net_sock.c stands in for
#    FFmpeg's avio there. QEMU=<riscos-ffmpeg's qemu-arm-aligntrap> also
#    traps unaligned accesses, as RISC OS does.
# 3. The version in src/version.h and !Help agree.
# Needs: gcc, pkg-config, libavformat-dev, python3, gcc-arm-linux-gnueabihf,
# qemu-user.
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
REEL_SRC=${REEL_SRC:-$TOP/../riscos-ffmpeg}     # at v5.1.10-riscos16 (Reel 0.1.23, h264_vchiq): reelcore.h, panel_font.h
OUT=${OUT:-/tmp/matinee-tests}
PORT=${PORT:-18411}
[ -f "$REEL_SRC/player/sources.c" ] || { echo "run.sh: set REEL_SRC to a riscos-ffmpeg checkout" >&2; exit 2; }
mkdir -p "$OUT"
gcc -O1 -g -Wall -Wno-format-truncation -I"$TOP/src" -I"$TOP/third_party/cjson" -I"$REEL_SRC/player" \
  -o "$OUT/core_test" "$TOP/tests/host/core_test.c" "$TOP"/src/net.c "$TOP"/src/plex.c "$TOP"/src/jellyfin.c "$TOP"/src/dlna.c "$TOP"/src/xml.c \
  "$TOP"/src/caps.c "$TOP"/src/handoff.c "$TOP/third_party/cjson/cJSON.c" "$REEL_SRC/player/sources.c" \
  $(pkg-config --cflags --libs libavformat libavutil)
arm-linux-gnueabihf-gcc -O1 -g -Wall -Wno-format-truncation -marm -mno-unaligned-access -no-pie \
  -DMATINEE_TEST -DMATINEE_NO_MAIN -D_FILE_OFFSET_BITS=64 \
  -I"$TOP/tests/host/fake" -I"$TOP/src" -I"$TOP/third_party/cjson" -I"$REEL_SRC/player" -I"$REEL_SRC/reelcore" \
  -o "$OUT/ui_test" "$TOP/tests/host/ui_test.c" "$TOP/src/ui.c" "$TOP/src/plex.c" "$TOP/src/jellyfin.c" "$TOP/src/dlna.c" "$TOP/src/xml.c" "$TOP/src/caps.c" \
  "$TOP/src/handoff.c" "$TOP/src/draw.c" "$TOP/src/player.c" "$TOP/src/imgcache.c" "$TOP/tests/host/fake_reelcore.c" "$TOP/tests/host/net_sock.c" "$TOP/third_party/cjson/cJSON.c" "$REEL_SRC/player/sources.c" -lm

python3 "$TOP/tests/host/fakeplex.py" "$PORT" > "$OUT/fakeplex.log" 2>&1 &
FP=$!
trap 'kill $FP 2>/dev/null' EXIT
sleep 1.5
bad=0
timeout 120 "$OUT/core_test" "$PORT" || bad=1

# ui_test under qemu; the alignment trap (if this qemu has it) covers the
# program's own code, not glibc's
end=$(arm-linux-gnueabihf-readelf -lW "$OUT/ui_test" | awk '$1=="LOAD" && / R E / {print $3, $6; exit}')
lo=$(( ${end% *} )); hi=$(( ${end% *} + ${end#* } ))
QEMU_ARM_ALIGN_TRAP=1 QEMU_ARM_ALIGN_IGNORE=$(printf '0-%x,%x-ffffffff' $lo $hi) \
QEMU_LD_PREFIX=${QEMU_LD_PREFIX:-/usr/arm-linux-gnueabihf} \
  timeout 300 "${QEMU:-qemu-arm}" "$OUT/ui_test" "$PORT" "$OUT" || bad=1

v=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
if head -1 "$TOP/app/!Matinee/!Help,fff" | grep -q " $v\$"; then
  echo "versions agree: $v"
else
  echo "FAIL: !Help's first line doesn't end with $v" >&2
  bad=1
fi
exit $bad
