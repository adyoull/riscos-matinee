# PThreadTicker 0.03 (UnixLib's thread timer module)

The module that UnixLib 5.0.1 and later programs use for their thread timer
when it is loaded (without it UnixLib runs a copy from the RMA). Matinee is
linked with UnixLib 5.0.3.1, so !Matinee carries the module and its !Run loads
it (a copy merged into !System is used first if there is one), as Reel's
and ReelEGL's do.

- From `PThreadTicker-0.03.zip`, release v5.0.3.1 of
  github.com/adyoull/riscos-unixlib (`libunixlib/module/pthticker.s`,
  `docs/THREAD-TICKER.md`; zip sha256
  `a5e9e925ba416986d99fbf0402ca8887062bd3e568f1b58d3fd2284d6ac60658`). 0.03
  keeps the timer running while the program is in Wimp_Poll (threads are
  switched as it returns). Programs built with 5.0.3.1 use only 0.03; older
  ones only 0.01/0.02; either runs its own copy otherwise, so any mix is
  safe. !Run still asks for 0.01 or later, as the module's ReadMe advises
  (an older copy in use can't be replaced).
- `PThrTicker` sha256: see `PThrTicker.sha256` (build/package.sh checks it).
- Revised BSD licence: `Licence`. User notes: `ReadMe`.
