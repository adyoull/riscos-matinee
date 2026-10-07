# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
# riscos-ffmpeg's devkit: the 5.1.10-riscos20 release (61ff56a, Reel 0.1.27's
# reelcore: riscos19's with UnixLib 5.0.3.3)
FFDEV=${FFDEV:-$TOP/../devkit/riscos-ffmpeg-devkit-5.1.10-riscos20}
# riscos-mesa's devkit (release v20.3.5-14): libz
MESADEV=${MESADEV:-$TOP/../devkit/riscos-mesa-devkit-20.3.5-14}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
