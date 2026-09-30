# riscos-plex (working name: PlexRO)

A Plex client for RISC OS, built natively in C. It browses a Plex Media Server and hands
playback to **ReelEGL** or **Reel** (riscos-ffmpeg). Files that Reel can play as they are
go to Reel directly. Everything else is converted by the server into a stream Reel can
handle (H.264 + stereo AAC over HLS), because the client only advertises what RISC OS
can play.

**Status: phase 1 prototype, unfinished.**

What exists now is the core, and it has been tested on Linux only:

- sign-in
- server discovery
- browsing
- the direct-play or transcode decision
- the hand-off to Reel

What doesn't exist yet:

- the Wimp front end
- a RISC OS build
- the `!PlexRO` application directory
- "Save original file" (large-file download)


Licence: GPL v2 or later (the own files). `third_party/cjson` is MIT (see its LICENSE).

## Layout

| Path | What |
|---|---|
| `src/net.[ch]` | HTTP(S) through FFmpeg's avio (AcornSSL on RISC OS, via riscos-ffmpeg patch 0018) |
| `src/plex.[ch]` | The Plex API: PIN sign-in, servers, lists, posters, scrobble, file URL |
| `src/caps.[ch]` | What Reel can play; the direct-play decision and the transcode request |
| `src/handoff.[ch]` | yt-dlp-style JSON that Reel 0.1.17+ plays unmodified |
| `third_party/cjson` | cJSON 1.7.18 (MIT) |
| `tests/host/fakeplex.py` | A stand-in for plex.tv and a Plex Media Server |
| `tests/host/core_test.c` | 86 checks against it, with Reel's own `sources.c` reading the hand-off |
| `tests/host/run.sh` | Builds and runs the host test |
| `docs/` | Feasibility, design, toolchain notes, session state |

## Host test

```
sudo apt install gcc pkg-config libavformat-dev python3
git clone https://github.com/adyoull/riscos-ffmpeg.git ../riscos-ffmpeg
REEL_SRC=../riscos-ffmpeg tests/host/run.sh
```

Expected output: `core_test: 86 checks, 0 failed`.
