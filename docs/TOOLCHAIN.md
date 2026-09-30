# Toolchain, dependencies and building

## The toolchain: GCCSDK GCC 10.2 with UnixLib 5.0.2

PlexRO is built with the GCCSDK GCC 10.2 cross-compiler (`arm-riscos-gnueabihf-`) in
`~/gccsdk/env`, with its UnixLib replaced by **UnixLib 5.0.2** (riscos-unixlib release
v5.0.2), so programs built with `-D_FILE_OFFSET_BITS=64` handle files over 2GB (up to
4GB−1). It's the same set-up riscos-ffmpeg's 5.1.10-riscos13 and Reel 0.1.20 use.

### Quickest: the prebuilt toolchain

1. Unpack riscos-warzone2100's `gccsdk-gcc10.2-x86_64-linux-env.tgz` into your home
   directory. It gives `~/gccsdk/env`.
2. Put UnixLib 5.0.2 into it:
   - `libunixlib.a` from the v5.0.2 release goes to
     `~/gccsdk/env/arm-riscos-gnueabihf/lib/`. Its sha256 is
     `bcd012806ec2b28e33633f1badf3b6946a583bad3ffefba672d67873ba812254`.
   - The headers that changed come from the v5.0.2 tag (`libunixlib/include/`):
     `sys/stat.h`, `sys/mman.h`, `unistd.h` and `sched.h`. They go to
     `~/gccsdk/env/arm-riscos-gnueabihf/include/`.
3. elf2aif (with the large-image fix): riscos-crossdev's toolchain has a Linux build in
   `bin/elf2aif`. riscos-ffmpeg's `tools/elf2aif` also builds one from source.

`build/build.sh` checks the result: `tools/check-unixlib.sh` fails if the program
wasn't linked with UnixLib 5.0.2.

### From source

No prebuilt GCCSDK toolchain is published as a GitHub release asset, so it can also be
built from source. Use riscos-warzone2100's `build/build-toolchain.sh`, with one change:
the UnixLib patch is riscos-unixlib **v5.0.2**'s `unixlib-riscos.diff`.

```
git clone --depth 1 https://github.com/adyoull/riscos-warzone2100.git wz
mkdir dl && cd dl
curl -LO https://github.com/adyoull/riscos-unixlib/releases/download/v5.0.2/unixlib-riscos.diff
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
| UnixLib 5.0.2 `libunixlib.a` | bcd012806ec2b28e33633f1badf3b6946a583bad3ffefba672d67873ba812254 |
| UnixLib 5.0.2 `unixlib-riscos.diff` | cabf419e8d55dba55bbd4ee2df81a6c773321f79fb5ad10f38c0da31f04433cc |

## Devkits (release assets)

Unpack them beside the repository, in `../devkit/`, or set `FFDEV` and `MESADEV`:

- `riscos-ffmpeg/releases/download/v5.1.10-riscos13/riscos-ffmpeg-devkit-5.1.10-riscos13.tgz`:
  libavformat and the rest, built against UnixLib 5.0.2, with AcornSSL https
  (patch 0018).
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
links them statically with libavformat, libavcodec, libswresample, libavutil, the codec
libraries (dav1d, x264, LAME, Opus, Vorbis, Ogg) and zlib. The codec libraries are only
there because libavformat pulls in its format list (its file
protocol checks names against every format).

The results, in `build/out/`:

- `plexro_g`: the ELF with debug information. Keep it for `addr2line` on crash
  addresses, and don't ship it. UnixLib 5.0.2's debug information holds its build
  path.
- `!RunImage,ff8`: the AIF.

Then it checks three things:

- no big stack frame goes unprobed;
- UnixLib 5.0.2 was linked;
- no build path or unwanted name is in `!RunImage`.

`build/package.sh` makes `dist/PlexRO-<version>.zip`, with RISC OS filetypes. It holds
`!PlexRO` with `!RunImage`, `!Sprites` (`tools/mksprites.py`), `PThrTicker` (its sha256
is checked), the licences and the source. It stops if two names differ only in case.

Build in a directory whose path holds nothing personal (such as `/build/riscos-plex`):
paths can end up in objects.

## Host tests

```
sudo apt install gcc pkg-config libavformat-dev python3 gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-user
REEL_SRC=../riscos-ffmpeg QEMU=path/to/qemu-arm-aligntrap tests/host/run.sh
REEL_SRC=../riscos-ffmpeg QEMU=path/to/qemu-arm-aligntrap tests/host/mutate.sh
```

`QEMU` is optional. riscos-ffmpeg's patched qemu (`tools-bin/qemu-arm-aligntrap`) also
traps unaligned accesses in the program's own code, as RISC OS does. The plain
`qemu-arm` runs the test without that.
