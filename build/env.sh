# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=${TOP:-$(cd "$(dirname "$0")/.." && pwd)}
# The toolchain: riscos-crossdev's (RISCOS_TOOLCHAIN, as its container
# image and `make prebuilt` set it: GCCSDK GCC 10.2 with riscos-unixlib),
# else a GCCSDK environment in ~/gccsdk/env.
GCCSDK_ENV=${GCCSDK_ENV:-${RISCOS_TOOLCHAIN:-$HOME/gccsdk/env}}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
if [ -z "$ELF2AIF" ] && [ -x "$GCCSDK_ENV/bin/elf2aif" ]; then
  ELF2AIF=$GCCSDK_ENV/bin/elf2aif
fi
ELF2AIF=${ELF2AIF:-elf2aif}

# The devkits Matinee is built with: the releases .riscos-deps names, each
# downloaded by build/fetch-devkits.sh into $DL and checked by sha256.
# ci/riscos-deps-hook.sh moves these lines when a new release comes.
FFDEV_VER=5.1.10-riscos23
FFDEV_SHA256=1aefaa47169814b6b0301b30111b8f1542265e60feb80d0d23c547b1052dfe0d
MESADEV_VER=20.3.5-17
MESADEV_SHA256=e0bea16a604265cde8d360055d2cb57dc38f4c7442954e599c393acd4c1395b8
DL=${DL:-$TOP/build/dl}
# riscos-ffmpeg's devkit: FFmpeg's libraries, Reel's player core (reelcore)
# and ReelHWAccel's vcdec and hevcdec
FFDEV=${FFDEV:-$DL/riscos-ffmpeg-devkit-$FFDEV_VER}
# riscos-mesa's devkit (zlib): riscos-crossdev's, if it's the same release,
# else the one downloaded
if [ -z "$MESADEV" ] && [ -n "$RISCOS_DEVKIT" ] &&
   grep -qx "riscos-mesa devkit $MESADEV_VER" "$RISCOS_DEVKIT/VERSION" 2>/dev/null; then
  MESADEV=$RISCOS_DEVKIT
fi
MESADEV=${MESADEV:-$DL/riscos-mesa-devkit-$MESADEV_VER}

OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
