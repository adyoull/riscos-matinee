# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
# riscos-ffmpeg's devkit (release v5.1.10-riscos16, with Reel 0.1.23's reelcore and
# h264_vchiq on ReelHWAccel's vcdec, devkit 0.2.1): libavformat and the rest
FFDEV=${FFDEV:-$TOP/../devkit/riscos-ffmpeg-devkit-5.1.10-riscos16}
# riscos-mesa's devkit (release v20.3.5-8): libz
MESADEV=${MESADEV:-$TOP/../devkit/riscos-mesa-devkit-20.3.5-8}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
