# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
# riscos-ffmpeg's devkit: the 5.1.10-riscos19 release (b3a6e24, Reel 0.1.26's
# reelcore: opt1-opt12's speed-ups and the review's fixes)
FFDEV=${FFDEV:-$TOP/../devkit/riscos-ffmpeg-devkit-5.1.10-riscos19}
# riscos-mesa's devkit (release v20.3.5-13): libz
MESADEV=${MESADEV:-$TOP/../devkit/riscos-mesa-devkit-20.3.5-13}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
