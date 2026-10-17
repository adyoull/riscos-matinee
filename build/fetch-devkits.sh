#!/bin/sh
# fetch-devkits.sh - downloads the devkits build/env.sh names (riscos-ffmpeg's
# and riscos-mesa's, from their GitHub releases) into $DL, checks each
# against its sha256, and unpacks it. One already there is left as it is;
# one given from outside (FFDEV=, MESADEV=, or riscos-crossdev's riscos-mesa
# devkit when it's the same release) isn't fetched. Run by build/build.sh.
set -e
. "$(dirname "$0")/env.sh"
mkdir -p "$DL"

# fetch NAME VER SHA256 URL DIR LIB: DIR is where env.sh expects it, LIB a
# library in it (to see whether it's there already)
fetch() {
  [ -f "$5/lib/$6" ] && return 0
  tgz=$DL/$1-$2.tgz
  if [ ! -f "$tgz" ] || ! echo "$3  $tgz" | sha256sum -c - >/dev/null 2>&1; then
    echo "downloading $1 $2"
    curl -fsSL -o "$tgz.part" "$4"
    mv "$tgz.part" "$tgz"
  fi
  echo "$3  $tgz" | sha256sum -c - >/dev/null 2>&1 ||
    { echo "fetch-devkits.sh: $tgz isn't the one build/env.sh names (sha256)" >&2; exit 1; }
  rm -rf "$DL/$1-$2"
  tar xzf "$tgz" -C "$DL"
  [ -f "$5/lib/$6" ] || { echo "fetch-devkits.sh: no lib/$6 in $tgz" >&2; exit 1; }
}

fetch riscos-ffmpeg-devkit "$FFDEV_VER" "$FFDEV_SHA256" \
  "https://github.com/adyoull/riscos-ffmpeg/releases/download/v$FFDEV_VER/riscos-ffmpeg-devkit-$FFDEV_VER.tgz" \
  "$FFDEV" libreelcore.a
fetch riscos-mesa-devkit "$MESADEV_VER" "$MESADEV_SHA256" \
  "https://github.com/adyoull/riscos-mesa/releases/download/v$MESADEV_VER/riscos-mesa-devkit-$MESADEV_VER.tgz" \
  "$MESADEV" libz.a
