# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
# riscos-ffmpeg's devkit: riscos-ffmpeg 856b84b (the riscos16 tree with
# hevc_hwdec, ReelHWAccel devkit 0.2.7: HEVC on the Pi 4's HEVC block, and
# vcdec 0.4.2's EMT fix), built from source as riscos-ffmpeg's test build
# r80 (hevc2) until it publishes the devkit (docs/TOOLCHAIN.md)
FFDEV=${FFDEV:-$TOP/../devkit/riscos-ffmpeg-devkit-5.1.10-riscos16-hevc2}
# riscos-mesa's devkit (release v20.3.5-8): libz
MESADEV=${MESADEV:-$TOP/../devkit/riscos-mesa-devkit-20.3.5-8}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
