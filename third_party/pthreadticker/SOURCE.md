# PThreadTicker 0.02 (UnixLib's thread timer module)

The module that UnixLib 5.0.1 and later programs use for their thread timer
when it is loaded (without it UnixLib runs a copy from the RMA). PlexRO is
linked with UnixLib 5.0.3.1-rc8, so !PlexRO carries the module and its !Run loads
it (a copy merged into !System is used first if there is one), as Reel's
and ReelEGL's do.

- From `PThreadTicker-0.02.zip`, pre-release v5.0.3.1-rc8 of
  github.com/adyoull/riscos-unixlib (`libunixlib/module/pthticker.s`,
  `docs/THREAD-TICKER.md`; zip sha256
  `5009d401f2ffc5f07b8c7035affb1a182bc0507205bbd6d6dadeb3504e49310e`). 0.02
  updates its count of programs with interrupts off; same interface. !Run
  still asks for 0.01 or later (a 0.01 already loaded is kept: it can't be
  replaced while a program uses it).
- `PThrTicker` sha256: see `PThrTicker.sha256` (build/package.sh checks it).
- Revised BSD licence: `Licence`. User notes: `ReadMe`.
