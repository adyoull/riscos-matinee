#!/bin/sh
# fetch-reel-src.sh - riscos-ffmpeg's source for the host tests (Reel's
# player/sources.c and reelcore.h), at the release build/env.sh's devkit
# is (FFDEV_VER), from GitHub's archive of its tag, into
# build/dl/riscos-ffmpeg-<version>-src (only player/ and reelcore/ are
# kept). Prints that directory. One already there is used as it is.
# REEL_SRC_URL= gives another archive (a .tar.gz with one top directory).
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
. "$TOP/build/env.sh"
dir=$DL/riscos-ffmpeg-$FFDEV_VER-src
if [ ! -f "$dir/player/sources.c" ]; then
  url=${REEL_SRC_URL:-https://github.com/adyoull/riscos-ffmpeg/archive/refs/tags/v$FFDEV_VER.tar.gz}
  mkdir -p "$DL"
  rm -rf "$dir" "$dir.part"
  mkdir -p "$dir.part"
  curl -fsSL "$url" | tar xzf - -C "$dir.part" --strip-components=1 --wildcards '*/player/*' '*/reelcore/*' >&2
  [ -f "$dir.part/player/sources.c" ] && [ -f "$dir.part/reelcore/reelcore.h" ] ||
    { echo "fetch-reel-src.sh: no player/sources.c or reelcore/reelcore.h in $url" >&2; exit 1; }
  mv "$dir.part" "$dir"
fi
echo "$dir"
