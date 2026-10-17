#!/bin/sh
# ci/riscos-deps-hook.sh NAME OLD_TAG NEW_TAG
#
# Run by .github/workflows/riscos-deps-update.yml when a repository
# .riscos-deps names makes a new release, before .riscos-deps is changed.
# Matinee also pins two of them in build/env.sh, the devkits it downloads:
#   riscos-ffmpeg  v5.1.10-riscosN  ->  FFDEV_VER, FFDEV_SHA256
#   riscos-mesa    v20.3.5-N        ->  MESADEV_VER, MESADEV_SHA256
# This downloads the new release's devkit, takes its sha256, and moves
# those lines. Anything else (riscos-crossdev) needs nothing here.
set -e
name=$1 old=$2 new=$3
case $name in
  riscos-ffmpeg|adyoull/riscos-ffmpeg) var=FFDEV; file=riscos-ffmpeg-devkit; repo=riscos-ffmpeg ;;
  riscos-mesa|adyoull/riscos-mesa)     var=MESADEV; file=riscos-mesa-devkit; repo=riscos-mesa ;;
  *) exit 0 ;;
esac
ver=${new#v}
url=https://github.com/adyoull/$repo/releases/download/$new/$file-$ver.tgz
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
curl -fsSL -o "$tmp" "$url" || { echo "riscos-deps-hook.sh: $new has no $file-$ver.tgz ($url)" >&2; exit 1; }
sum=$(sha256sum "$tmp" | cut -d' ' -f1)
sed -i -e "s/^${var}_VER=.*/${var}_VER=$ver/" -e "s/^${var}_SHA256=.*/${var}_SHA256=$sum/" build/env.sh
grep -q "^${var}_VER=$ver\$" build/env.sh && grep -q "^${var}_SHA256=$sum\$" build/env.sh ||
  { echo "riscos-deps-hook.sh: couldn't move ${var}_VER in build/env.sh" >&2; exit 1; }
echo "build/env.sh: $file $ver ($sum)"
