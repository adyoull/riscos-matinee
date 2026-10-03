# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
# riscos-ffmpeg's devkit: release v5.1.10-riscos16 (Reel 0.1.23's reelcore,
# h264_vchiq) rebuilt with ReelHWAccel's devkit 0.2.4 (vcdec 0.4.2: its pools
# never take our page at &8000, the "EMT trap, code 6" fix) until riscos-ffmpeg
# publishes one with it (docs/TOOLCHAIN.md)
FFDEV=${FFDEV:-$TOP/../devkit/riscos-ffmpeg-devkit-5.1.10-riscos16-vcdec042}
# riscos-mesa's devkit (release v20.3.5-8): libz
MESADEV=${MESADEV:-$TOP/../devkit/riscos-mesa-devkit-20.3.5-8}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
