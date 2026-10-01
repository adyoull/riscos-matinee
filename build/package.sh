#!/bin/sh
# package.sh - $DIST/PlexRO-<version>.zip: !PlexRO with !RunImage (from
# build.sh), !Sprites and !Sprites11, PThreadTicker, the licences and the source. The zip
# carries RISC OS filetypes (tools/mkrozip.py).
set -e
. "$(dirname "$0")/env.sh"
[ -f "$OUT/!RunImage,ff8" ] || { echo "run build/build.sh first" >&2; exit 1; }
T=$OUT/pkg
A=$T/!PlexRO
rm -rf "$T"
mkdir -p "$A"
cp "$TOP/app/!PlexRO/"* "$A/"
cp "$OUT/!RunImage,ff8" "$A/"
python3 "$TOP/tools/mksprites.py" "$A/!Sprites,ff9"
python3 "$TOP/tools/mksprites.py" --hi "$A/!Sprites11,ff9"   # IconSprites picks it in high-resolution modes
( cd "$TOP/third_party/pthreadticker" && sha256sum -c PThrTicker.sha256 >/dev/null ) ||
  { echo "PThrTicker's checksum is wrong" >&2; exit 1; }
cp "$TOP/third_party/pthreadticker/PThrTicker" "$A/PThrTicker,ffa"
L=$A/docs/Licences
mkdir -p "$L" "$A/docs/source/c" "$A/docs/source/h"
cp "$TOP/COPYING" "$L/PlexRO,fff"
cp "$TOP/third_party/cjson/LICENSE" "$L/cJSON,fff"
cp "$TOP/third_party/pthreadticker/Licence" "$L/PThreadTicker,fff"
# what's linked in from riscos-ffmpeg's devkit
for f in FFmpeg GPLv2 LGPLv21 x264 dav1d LAME Opus Ogg Vorbis; do
  cp "$FFDEV/Licences/$f,fff" "$L/$f,fff"
done
for f in "$TOP"/src/*.c "$TOP/third_party/cjson/cJSON.c"; do
  b=$(basename "$f" .c); cp "$f" "$A/docs/source/c/$b,fff"
done
for f in "$TOP"/src/*.h "$TOP/third_party/cjson/cJSON.h"; do
  b=$(basename "$f" .h); cp "$f" "$A/docs/source/h/$b,fff"
done
cat > "$A/docs/source/ReadMe,fff" <<EOT
PlexRO $VERSION's own source (c and h), with cJSON 1.7.18. It is built
by build/build.sh in the riscos-plex repository, against riscos-ffmpeg's
devkit (5.1.10-riscos14: FFmpeg's libraries, and reelcore for the built-in
player) and UnixLib 5.0.3.1-rc8.
EOT
# RISC OS names ignore case: two files that differ only in case would
# overwrite each other when unzipped
dups=$(cd "$T" && find . | tr A-Z a-z | sed 's/,[0-9a-f][0-9a-f][0-9a-f]$//' | sort | uniq -d)
[ -z "$dups" ] || { echo "names that differ only in case: $dups" >&2; exit 1; }
mkdir -p "$DIST"
rm -f "$DIST/PlexRO-$VERSION.zip"
( cd "$T" && python3 "$TOP/tools/mkrozip.py" "$DIST/PlexRO-$VERSION.zip" '!PlexRO' )
ls -l "$DIST/PlexRO-$VERSION.zip"
