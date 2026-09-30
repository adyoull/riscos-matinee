#!/bin/sh
# build.sh - builds PlexRO for RISC OS and checks it:
#   $OUT/plexro_g   the ELF (with debug information: keep it for reading
#                   crash addresses with addr2line; don't ship it)
#   $OUT/plexro     stripped, then !RunImage,ff8 (AIF)
# Checks: every big stack frame is probed, the program was linked with
# UnixLib 5.0.2 (the 472-byte pthread block, 64-bit fstat), and no build
# path or unwanted name is left in the shipped image.
# Needs the GCCSDK GCC 10.2 toolchain with UnixLib 5.0.2 (docs/TOOLCHAIN.md)
# and the two devkits (build/env.sh).
set -e
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT"
CFLAGS="-O2 -Wall -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64"
# libavformat brings its format and codec lists (its file protocol reaches
# them), so the codec libraries are linked too
LIBS="-L$FFDEV/lib -lavformat -lavcodec -lswresample -lavutil -ldav1d -lx264 -lmp3lame -lopus \
  -lvorbisenc -lvorbis -logg -L$MESADEV/lib -lz -lm"
"${CROSS}gcc" $CFLAGS -Wno-format-truncation -I"$TOP/src" -I"$TOP/third_party/cjson" -I"$FFDEV/include" \
  "$TOP"/src/*.c "$TOP/third_party/cjson/cJSON.c" -static $LIBS -o "$OUT/plexro_g"
"${CROSS}strip" -o "$OUT/plexro" "$OUT/plexro_g"
rm -f "$OUT/!RunImage,ff8"
"$ELF2AIF" -e "$OUT/plexro" "$OUT/!RunImage,ff8" >/dev/null
"${CROSS}size" "$OUT/plexro_g"

echo "checks:"
python3 "$TOP/tools/check-stack-probes.py" "$OUT/plexro_g" "${CROSS}objdump" | tail -1 | tee "$OUT/probes.txt"
grep -q '; 0 without stack probes' "$OUT/probes.txt" || { echo "unprobed stack frames" >&2; exit 1; }
CROSS=$CROSS sh "$TOP/tools/check-unixlib.sh" "$OUT/plexro_g"
# the shipped image: no build paths or names that don't belong
if strings -a "$OUT/!RunImage,ff8" | grep -i -E 'claude|anthropic|/home/|/root/|/build/'; then
  echo "unwanted strings in !RunImage" >&2
  exit 1
fi
echo "  !RunImage: no build paths ($(wc -c < "$OUT/!RunImage,ff8") bytes)"
