# PThreadTicker 0.01 (UnixLib's thread timer module)

The module that UnixLib 5.0.1 and later programs use for their thread timer
when it is loaded (without it UnixLib runs a copy from the RMA). PlexRO is
linked with UnixLib 5.0.2, so !PlexRO carries the module and its !Run loads
it (a copy merged into !System is used first if there is one), as Reel's
and ReelEGL's do.

- From `PThreadTicker-0.01.zip`, release v5.0.2 of
  github.com/adyoull/riscos-unixlib (`libunixlib/module/pthticker.s`,
  `docs/THREAD-TICKER.md`); copied here from riscos-ffmpeg's
  `third_party/pthreadticker`.
- `PThrTicker` sha256: see `PThrTicker.sha256` (build/package.sh checks it).
- Revised BSD licence: `Licence`. User notes: `ReadMe`.
