# env.sh - where the build finds its tools and libraries (sourced by
# build.sh and package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
# riscos-ffmpeg's devkit: riscos-ffmpeg ae81fed, its opt9 test build on the
# 5.1.10-riscos18 release (Reel 0.1.25's reelcore with the optimisations of
# opt1-opt9: the HEVC block kept up with (clock slip, non-reference pictures
# skipped when behind, devkit 0.2.11), 10-bit narrowed in NEON, HEVC default
# weights, VP9/AV1 skipping), built here until riscos-ffmpeg publishes one
FFDEV=${FFDEV:-$TOP/../devkit/riscos-ffmpeg-devkit-5.1.10-riscos18-opt9}
# riscos-mesa's devkit (release v20.3.5-13): libz
MESADEV=${MESADEV:-$TOP/../devkit/riscos-mesa-devkit-20.3.5-13}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define MATINEE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
