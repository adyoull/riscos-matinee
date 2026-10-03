# Toolchain, dependencies and building

## The toolchain: GCCSDK GCC 10.2 with UnixLib 5.0.3.1

Matinee is built with the GCCSDK GCC 10.2 cross-compiler (`arm-riscos-gnueabihf-`) in
`~/gccsdk/env`, with its UnixLib replaced by **UnixLib 5.0.3.1** (riscos-unixlib
release v5.0.3.1; test17 used pre-release rc8, test1 to test16 release v5.0.2), so programs built with
`-D_FILE_OFFSET_BITS=64` handle files over 2GB (up to 4GB−1), and fork, `_exit`, heaps past 128 MB and
threads in programs that poll often are in. riscos-ffmpeg's devkit (5.1.10-riscos14) was built against 5.0.2:
its libraries link with 5.0.3.1 unchanged (no interface changes; 5.0.3.1's headers differ
from 5.0.2's only in a comment in `sched.h`).

### Quickest: the prebuilt toolchain

1. Unpack riscos-warzone2100's `gccsdk-gcc10.2-x86_64-linux-env.tgz` into your home
   directory. It gives `~/gccsdk/env`.
2. Put UnixLib 5.0.3.1 into it:
   - `libunixlib.a` from the v5.0.3.1 release goes to
     `~/gccsdk/env/arm-riscos-gnueabihf/lib/`. Its sha256 is
     `fa98152f7e05313aa94d15004c5011d7e2cfec92623adc467d4d50a23368354e`.
   - The headers that changed since GCCSDK come from the v5.0.3.1 tag
     (`libunixlib/include/`): `sys/stat.h`, `sys/mman.h`, `unistd.h` and `sched.h`.
     They go to `~/gccsdk/env/arm-riscos-gnueabihf/include/`.
3. elf2aif (with the large-image fix): riscos-crossdev's toolchain has a Linux build in
   `bin/elf2aif`. riscos-ffmpeg's `tools/elf2aif` also builds one from source.

`build/build.sh` checks the result: `tools/check-unixlib.sh` fails if the program
wasn't linked with UnixLib 5.0.3.1 (`__exit_status`, and the 640-byte pthread
block).

### From source

No prebuilt GCCSDK toolchain is published as a GitHub release asset, so it can also be
built from source. Use riscos-warzone2100's `build/build-toolchain.sh`, with one change:
the UnixLib patch is riscos-unixlib **v5.0.3.1**'s `unixlib-riscos.diff`.

```
git clone --depth 1 https://github.com/adyoull/riscos-warzone2100.git wz
mkdir dl && cd dl
curl -LO https://github.com/adyoull/riscos-unixlib/releases/download/v5.0.3.1/unixlib-riscos.diff
curl -L -o gccsdk-64c6f81.tar.gz https://codeload.github.com/jhamby/riscos-gccsdk/tar.gz/64c6f81
curl -L -o gcc-10.2.0.tar.gz https://codeload.github.com/gcc-mirror/gcc/tar.gz/refs/tags/releases/gcc-10.2.0
U=http://archive.ubuntu.com/ubuntu/pool
curl -LO $U/main/b/binutils/binutils_2.30.orig.tar.xz
curl -LO $U/main/g/gmp/gmp_6.2.1+dfsg.orig.tar.xz
curl -LO $U/main/m/mpfr4/mpfr4_4.1.0.orig.tar.xz
curl -LO $U/main/m/mpclib3/mpclib3_1.2.1.orig.tar.gz
cd ../wz
rm patches/unixlib/*.diff
cp ../dl/unixlib-riscos.diff patches/unixlib/
mkdir -p dl
ln -s ../../dl/* dl/
sudo apt install autogen autoconf2.69 autoconf2.64 automake1.11 libtool flex bison m4 texinfo
build/build-toolchain.sh all
```

It takes well over an hour on one core.

### Checksums (sha256; they match riscos-warzone2100's SHA256SUMS.txt and UnixLib's)

| File | sha256 |
|---|---|
| gccsdk-64c6f81.tar.gz | d4a2e97e180ebc7b19e49e74b5ac60a71ed8bb3757b64128e7629cd27d9ab9b7 |
| gcc-10.2.0.tar.gz | 033ae71e7956b52e41045bf680ba8ae3c1be699dfb001073ec2665443c6ba2e2 |
| binutils_2.30.orig.tar.xz | 6e46b8aeae2f727a36f0bd9505e405768a72218f1796f0d09757d45209871ae6 |
| gmp_6.2.1+dfsg.orig.tar.xz | c6ba08e3f079260ab90ff44ab8801eae134cd62cd78f4aa56317c0e70daa40cb |
| mpfr4_4.1.0.orig.tar.xz | 0c98a3f1732ff6ca4ea690552079da9c597872d30e96ec28414ee23c95558a7f |
| mpclib3_1.2.1.orig.tar.gz | 17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459 |
| cJSON-1.7.18 tarball (codeload v1.7.18) | 3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5 |
| UnixLib 5.0.3.1 `libunixlib.a` | fa98152f7e05313aa94d15004c5011d7e2cfec92623adc467d4d50a23368354e |
| UnixLib 5.0.3.1 `unixlib-riscos.diff` | 1209261295caa39ead46ecbc01e7c510da8bd2905fe8738ffb0dab7e2a557f10 |
| UnixLib 5.0.3.1 `PThreadTicker-0.03.zip` | a5e9e925ba416986d99fbf0402ca8887062bd3e568f1b58d3fd2284d6ac60658 |

## Devkits (release assets)

Unpack them beside the repository, in `../devkit/`, or set `FFDEV` and `MESADEV`:

- `riscos-ffmpeg-devkit-5.1.10-riscos16-vcdec042.tgz` (sha256
  `3afb1bad1d5724c262e65e15b1c0fdcc7ab475859d308b5a2af0861c3dac0f68`), used from test24:
  riscos-ffmpeg's v5.1.10-riscos16 tree (`21a1046`) with its `third_party/reelhwaccel`
  `vcdec.h` and `libvcdec.a` replaced by ReelHWAccel devkit 0.2.4's (sha256 `8876f366…`,
  `ReelHWAccel/dist/r26`: vcdec 0.4.2, whose contiguous pools never include the program's
  page at &8000; taking it made RISC OS move the program, ARMEABISupport 1.08 kept a stale
  record, and a later program could stop with "EMT trap, code 6"). h264_vchiq's patch is
  the same (0.2.4's differs only in its licence header). Built with riscos-ffmpeg's own
  scripts (`RECONFIGURE=1 build/build-ffmpeg.sh`, `build/build-apps.sh`,
  `REEL_VERSION=0.1.23-vcdec042 build/package.sh 5.1.10-riscos16-vcdec042`; deps as for
  test22). Not a riscos-ffmpeg release: switch to theirs once they adopt 0.2.4.
- `riscos-ffmpeg/releases/download/v5.1.10-riscos16/riscos-ffmpeg-devkit-5.1.10-riscos16.tgz`
  (sha256 `6d7ab4fb886e3dc42646678476ac530beaaa9972025f0f61365479a7095be8b9`): FFmpeg
  5.1.10-riscos16 with `h264_vchiq`, ReelHWAccel's `libvcdec.a` (devkit 0.2.1, vcdec
  0.4.1; GPL version 2: its licence is `Licences/ReelHWAccel`, packaged with Matinee),
  and Reel 0.1.23's `libreelcore.a`. Link `-lvcdec` after `-lavcodec`. Used from test23.
- test22 used `riscos-ffmpeg-devkit-5.1.10-riscos15-vc4.tgz` (sha256
  `dd2d7cd3d6a90e2b0495423784dd8f0fc3b5dee27c454dd9ce06656c15122b0b`), the same code
  before the release: built from riscos-ffmpeg's main at `8a828f4` (riscos15, Reel 0.1.22,
  riscos-reelhwaccel devkit 0.2.1 in its `third_party/reelhwaccel`) with its own
  scripts: `build/build-deps.sh`, `build/build-ffmpeg.sh`, `build/build-apps.sh`, then
  `REEL_VERSION=0.1.22-vc4 build/package.sh 5.1.10-riscos15-vc4` (GCCSDK GCC 10.2 with
  UnixLib 5.0.3.1; `DEVKIT` = riscos-mesa's 20.3.5-8 devkit; meson for dav1d). It has
  libavformat and the rest with AcornSSL https and `h264_vchiq`, `libvcdec.a` (H.264
  on the Pi's VideoCore: link `-lvcdec` after `-lavcodec`), and `libreelcore.a` with
  `reelcore.h` (Reel 0.1.22's: the VideoCore first for H.264, `REELCORE_NO_VIDEOCORE`,
  `ReelCoreStats.decoder`). When riscos-ffmpeg publishes its riscos15 devkit, use that.
  test21 and earlier used the v5.1.10-riscos14 release (sha256 `074585cd…`).
- `riscos-mesa/releases/download/v20.3.5-8/riscos-mesa-devkit-20.3.5-8.tgz`: libz now,
  and libEGL and libOSMesa for a later embedded ReelEGL.
- `riscos-ffmpeg/releases/download/reel-0.1.20/Reel-0.1.20.zip`: the players, for testing
  on the Pi.

## Building

```
GCCSDK_ENV=~/gccsdk/env ELF2AIF=path/to/elf2aif build/build.sh
build/package.sh
```

`build/build.sh` compiles `src/*.c` and cJSON with `-O2 -march=armv7-a
-mfpu=neon-vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64`. It
links them statically with reelcore, libavfilter, libpostproc, libavformat, libavcodec,
libswresample, libswscale, libavutil, the codec libraries (dav1d, x264, LAME, Opus,
Vorbis, Ogg) and zlib. The codec libraries are there because libavformat pulls in its
format list (its file protocol checks names against every format). SDL2 isn't linked: reelcore's
calls to SDL's audio are answered by `src/sdlstub.c`.

The results, in `build/out/`:

- `matinee_g`: the ELF with debug information. Keep it for `addr2line` on crash
  addresses, and don't ship it. UnixLib's debug information holds its build
  path.
- `!RunImage,ff8`: the AIF.

Then it checks three things:

- no big stack frame goes unprobed;
- UnixLib 5.0.3.1 was linked;
- no build path or unwanted name is in `!RunImage`.

`build/package.sh` makes `dist/Matinee-<version>.zip`, with RISC OS filetypes. It holds
`!Matinee` with `!RunImage`, `!Sprites` (`tools/mksprites.py`), `PThrTicker` (its sha256
is checked), the licences and the source. It stops if two names differ only in case.

Build in a directory whose path holds nothing personal (such as `/build/riscos-matinee`):
paths can end up in objects.

## Host tests

```
sudo apt install gcc pkg-config libavformat-dev python3 gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-user
REEL_SRC=../riscos-ffmpeg QEMU=path/to/qemu-arm-aligntrap tests/host/run.sh
REEL_SRC=../riscos-ffmpeg QEMU=path/to/qemu-arm-aligntrap tests/host/mutate.sh
```

ui_test draws its fake windows' text with riscos-ffmpeg's `reelcore/panel_font.h` (from
`REEL_SRC`, a riscos-ffmpeg checkout at `reel-0.1.21` (riscos14), which also gives
`reelcore.h`), and leaves `signin.ppm`, `browser.ppm`,
`details.ppm`, `player.ppm` and `upnext.ppm` in `OUT`. It links
`tests/host/fake_reelcore.c` in place of reelcore. fakeplex.py is a fake Jellyfin server
as well as a fake Plex one (Jellyfin's paths start with a capital letter); ui_test's
third run uses it and leaves `signin-jellyfin.ppm` and `details-jellyfin.ppm`.

`PROFILE=1` makes ui_test print, for each picture it saves (and for 1.2 s of playing),
the SWIs called and how often, the smooth shapes made and the text widths measured: a
quick way to see what a change costs a redraw.

`QEMU` is optional. riscos-ffmpeg's patched qemu (`tools-bin/qemu-arm-aligntrap`) also
traps unaligned accesses in the program's own code, as RISC OS does. The plain
`qemu-arm` runs the test without that.
