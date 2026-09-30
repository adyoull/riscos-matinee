# Toolchain and dependencies (how this session set them up)

No prebuilt GCCSDK toolchain is published on GitHub: riscos-crossdev v1.0 wasn't pushed,
and the Warzone tarball isn't a release asset. So it was built from source with
riscos-warzone2100's `build/build-toolchain.sh`, with one change: the UnixLib patch is
riscos-unixlib **v5.0.2**'s `unixlib-riscos.diff`, so the compiler's headers and library
support files over 2GB.

## Steps

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

The toolchain goes into `~/gccsdk/env`.

## Checksums (sha256; they match riscos-warzone2100's SHA256SUMS.txt)

| File | sha256 |
|---|---|
| gccsdk-64c6f81.tar.gz | d4a2e97e180ebc7b19e49e74b5ac60a71ed8bb3757b64128e7629cd27d9ab9b7 |
| gcc-10.2.0.tar.gz | 033ae71e7956b52e41045bf680ba8ae3c1be699dfb001073ec2665443c6ba2e2 |
| binutils_2.30.orig.tar.xz | 6e46b8aeae2f727a36f0bd9505e405768a72218f1796f0d09757d45209871ae6 |
| gmp_6.2.1+dfsg.orig.tar.xz | c6ba08e3f079260ab90ff44ab8801eae134cd62cd78f4aa56317c0e70daa40cb |
| mpfr4_4.1.0.orig.tar.xz | 0c98a3f1732ff6ca4ea690552079da9c597872d30e96ec28414ee23c95558a7f |
| mpclib3_1.2.1.orig.tar.gz | 17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459 |
| cJSON-1.7.18 tarball (codeload v1.7.18) | 3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5 |

`libunixlib.a` and `unixlib-riscos.diff` pass v5.0.2's own SHA256SUMS.

## Build time

The cloud container had one CPU core, and the build was still in binutils when the
session ended. Expect well over an hour on one core. It's much quicker on a multi-core
Mac VM. If you have a working `~/gccsdk/env` from the Warzone or FFmpeg work, reuse it
and relink with the v5.0.2 `libunixlib.a` instead.

## Devkits (release assets)

- `riscos-ffmpeg/releases/download/v5.1.10-riscos13/riscos-ffmpeg-devkit-5.1.10-riscos13.tgz`
  (libavformat and the rest, libreelcore, headers)
- `riscos-mesa/releases/download/v20.3.5-8/riscos-mesa-devkit-20.3.5-8.tgz`
  (libEGL, libOSMesa and libz, for a later embedded ReelEGL)
- `riscos-ffmpeg/releases/download/reel-0.1.20/Reel-0.1.20.zip` (the players, for testing
  on the Pi)

## Planned compile (not yet run)

```
arm-riscos-gnueabihf-gcc -O2 -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
  -fstack-clash-protection -D_FILE_OFFSET_BITS=64 -Isrc -Ithird_party/cjson \
  -I$FFDEV/include src/*.c third_party/cjson/cJSON.c -static -L$FFDEV/lib \
  -lavformat -lavcodec -lswresample -lavutil -L$MESADEV/lib -lz -lm -o plexro
elf2aif -e plexro !PlexRO/!RunImage,ff8
```

libavformat's protocols may pull in more of libavcodec, and libavcodec needs x264, dav1d
and the others. Add those libraries as the linker asks, as in riscos-ffmpeg's
`build/build-apps.sh`.
