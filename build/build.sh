#!/bin/sh
# build.sh - builds Matinee for RISC OS and checks it:
#   $OUT/matinee_g   the ELF (with debug information: keep it for reading
#                   crash addresses with addr2line; don't ship it)
#   $OUT/matinee     stripped, then !RunImage,ff8 (AIF)
# Checks: every big stack frame is probed, the program was linked with
# UnixLib 5.0.3.3 (the 640-byte pthread block, 64-bit fstat, the
# new _exit), riscos-crossdev's checks where it's installed, and no build
# path or unwanted name is left in the shipped image.
# Needs the GCCSDK GCC 10.2 toolchain with UnixLib 5.0.3.3 (riscos-crossdev's,
# or docs/TOOLCHAIN.md). The two devkits are downloaded (build/fetch-devkits.sh).
set -e
. "$(dirname "$0")/env.sh"
sh "$TOP/build/fetch-devkits.sh"
mkdir -p "$OUT"
CFLAGS="-O2 -Wall -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64"
# reelcore (the built-in player) and FFmpeg's libraries; libavformat brings
# its format and codec lists (its file protocol reaches them), so the codec
# libraries are linked too. reelcore's calls to SDL's audio (its other sound
# output) are answered by src/sdlstub.c.
# -lvcdec after -lavcodec: h264_vchiq, H.264 decoded by the Pi's VideoCore; -lhevcdec: hevc_hwdec, HEVC on the Pi 4's HEVC block
# (riscos-reelhwaccel); reelcore tries it first and falls back to the ARM.
LIBS="-L$FFDEV/lib -lreelcore -lavfilter -lpostproc -lavformat -lavcodec -lvcdec -lhevcdec -lswresample -lswscale -lavutil \
  -ldav1d -lx264 -lmp3lame -lopus -lvorbisenc -lvorbis -logg -L$MESADEV/lib -lz -lm"
"${CROSS}gcc" $CFLAGS -Wno-format-truncation -I"$TOP/src" -I"$TOP/third_party/cjson" -I"$FFDEV/include" \
  "$TOP"/src/*.c "$TOP/third_party/cjson/cJSON.c" -static $LIBS -o "$OUT/matinee_g"
"${CROSS}strip" -o "$OUT/matinee" "$OUT/matinee_g"
rm -f "$OUT/!RunImage,ff8"
"$ELF2AIF" -e "$OUT/matinee" "$OUT/!RunImage,ff8" >/dev/null
"${CROSS}size" "$OUT/matinee_g"

echo "checks:"
python3 "$TOP/tools/check-stack-probes.py" "$OUT/matinee_g" "${CROSS}objdump" | tail -1 | tee "$OUT/probes.txt"
grep -q '; 0 without stack probes' "$OUT/probes.txt" || { echo "unprobed stack frames" >&2; exit 1; }
CROSS=$CROSS sh "$TOP/tools/check-unixlib.sh" "$OUT/matinee_g"
# riscos-crossdev's own checks (static, stack probes, FPU level, UnixLib),
# where it's installed (its container image sets RCD_DIR)
if [ -n "$RCD_DIR" ] && [ -x "$RCD_DIR/checks/check-all.sh" ]; then
  "$RCD_DIR/checks/check-all.sh" "$OUT/matinee_g" > "$OUT/check-all.txt" ||
    { cat "$OUT/check-all.txt"; echo "riscos-crossdev's checks failed" >&2; exit 1; }
  tail -1 "$OUT/check-all.txt"
fi
# the shipped image: no build paths or names that don't belong
if strings -a "$OUT/!RunImage,ff8" | grep -i -E 'claude|anthropic|(^|[ :="])/(home|root|build)/'; then
  echo "unwanted strings in !RunImage" >&2
  exit 1
fi
echo "  !RunImage: no build paths ($(wc -c < "$OUT/!RunImage,ff8") bytes)"
