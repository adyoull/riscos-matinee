# Matinee

A Plex and Jellyfin client for RISC OS, built natively in C. It browses a Plex Media Server
(or a Jellyfin server) and plays
the videos in its own window with a **built-in player** (riscos-ffmpeg's reelcore, the core
of Reel), or hands them to **ReelEGL** or **Reel**. Files that can be played as they are
are played directly. Everything else is converted by the server into a stream that can
be (H.264 + stereo AAC over HLS), because the client only advertises what RISC OS can
play.

**Status: phase 2, a test build (0.1.0-test29).** test1 to test3 worked on a Raspberry Pi;
test5 added the built-in player; test6 fixed its scrolling and sync figure; test7 added metadata, search, the dashboard and
an image cache; test8 seeks in converted streams as the Plex apps do, and brings ReelEGL's
stats panel, a hidden pointer in full screen and a question before signing out; test9 chooses
subtitles while playing (drawn by the player when it plays the file itself) and says Direct Play
or Transcoded; test10 opens the window at 75% of the screen, centred, adds Reel's mini player
and scales the details backdrop with the window; test11 is a new look, after Plex clients such
as Plezy (badges on the posters, a details page with the backdrop fading into it and the poster
beside the title, a show's page with its series as tabs and its episodes as a list); test12 fixes
the show's page on a real server (Plex lists "All episodes" before the seasons); test13 adds the
home page (a featured item, then rows: Continue watching, Recently added); test14 puts Home and
the libraries in tabs under the bar, and the poster menu on Adjust too, with Remove from Continue
watching; test15 adds a library's bar (Collections, Playlists, Sort, Unwatched, A to Z) and More
like this and trailers on the details page; test16 adds Skip intro and Skip credits (the
server's markers) and chapters while playing; test17 is test16 linked with UnixLib
5.0.3.1-rc8 (and PThreadTicker 0.02); test18 is linked with the UnixLib 5.0.3.1 release
(and PThreadTicker 0.03). test19 is the name Matinee (it was PlexRO),
Plex Home users (Switch user, with PINs), your star rating, and the picture's box blacked
out under the hardware overlay; test20 is quicker in its hot paths (fitting text, big
libraries, the player's bar, compressed lists); test21 crops tall pictures from the foot
rather than through the middle, and has a Matinee icon (a little theatre); test22 is built on riscos-ffmpeg's Reel 0.1.22
reelcore with riscos-reelhwaccel's h264_vchiq: H.264 decoded by the Pi's VideoCore;
test23 is the same code built on the published riscos-ffmpeg 5.1.10-riscos16 devkit (Reel
0.1.23; ReelHWAccel's vcdec under GPL version 2); test24 has vcdec 0.4.2 (ReelHWAccel devkit
0.2.4), which fixes the "EMT trap, code 6" left behind after VideoCore playback; test25
adds Jellyfin servers (one at a time, beside Plex's on the Servers menu: Quick Connect or a
name and password, the same browsing, playing and progress); test26 has Add a server (Plex or
Jellyfin, then its sign-in, with Back and Cancel) and plays HEVC as it is on a Pi 4, decoded by
its HEVC block (riscos-ffmpeg's hevc_hwdec, ReelHWAccel devkit 0.2.7); test27 gives the network
reader the time between pictures while the read-ahead is short, and logs it; test28 is a code
audit's fixes, relinked with riscos-ffmpeg's 5.1.10-riscos17 release (Reel 0.1.24: the HEVC
block's pictures converted once, straight into the overlay), UnixLib 5.0.3.2 and riscos-mesa
20.3.5-11's zlib; test29 shows the collection a film is in on its details page, and doesn't keep
posters that arrived cut short (they showed as their top, then black).
It's tested on Linux too: the core against a fake Plex and Jellyfin server, and the Wimp front end
against a scripted fake Wimp under qemu.

- Sign-in with a plex.tv/link code, or a server's address and token typed by hand.
- Jellyfin servers too, one at a time, switched on the Servers menu: Quick Connect or a name
  and password; libraries, Continue watching (resume and next up), details, search, direct
  play or an HLS stream the server converts (asked for with a DeviceProfile), progress and
  watched marks reported, media segments as Skip intro / Skip credits.
- Servers, and a dark poster browser: Continue watching, films, TV shows, seasons and
  episodes, with shadows, a highlight under the pointer, and three poster sizes.
- One window: the sign-in page, the posters, and a details page (the backdrop, title,
  year, running time, rating, summary and buttons; the genres, director, writers, studio,
  dates, ratings and file; the cast with photos). Smooth, anti-aliased buttons and
  corners.
- Search across every library, with results as you type.
- An image cache on disc (posters, backdrops, cast photos), held to a size, with Clear image
  cache on the icon bar menu.
- Subtitles: choose a track (kept on the server); the server burns it in.
- The built-in player: in the window, or full screen in the desktop; the picture through
  a hardware overlay (VideoOverlay), which scales it for nothing; the stats; sound
  tracks; seeking (a converted stream is started again at the new place); the server
  told where you've got to, from a play queue (so it shows in the dashboard's Now
  Playing); the next episode offered at the end.
- The direct-play or transcode decision, and the hand-off to ReelEGL or Reel.
- Resume, Play from start, Mark watched or unwatched.
- Save original file: the file itself, up to 4GB−1, with its speed shown.
- Test speed: how fast the server sends a video, against what it needs to play directly.

Matinee was called PlexRO (a working name) up to test18. Plex is a trademark of Plex,
Inc.; Matinee isn't made by Plex or connected with it.

Licence: GPL v2 or later (the own files; see COPYING). `third_party/cjson` is MIT (see
its LICENSE), and `third_party/pthreadticker` is revised BSD. The program links riscos-ffmpeg's devkit,
which includes ReelHWAccel's vcdec under GPL version 2, so a built `!RunImage` is under GPL
version 2.

## Layout

| Path | What |
|---|---|
| `src/net.[ch]` | HTTP(S) through FFmpeg's avio (AcornSSL on RISC OS, via riscos-ffmpeg patch 0018) |
| `src/plex.[ch]` | The Plex API: PIN sign-in, servers, lists, posters, scrobble, file URL |
| `src/caps.[ch]` | What Reel can play; the direct-play decision and the transcode request |
| `src/handoff.[ch]` | yt-dlp-style JSON that Reel 0.1.17+ plays unmodified |
| `src/ui.[ch]` | The Wimp front end: icon bar, sign-in, browser, details, menus, hand-off, saving, Choices, and the player page's Plex side |
| `src/player.[ch]` | The built-in player: reelcore in the window, the overlay, the sprite, full screen, the bar, the stats |
| `src/imgcache.[ch]` | The image cache on disc: pictures by a hash of their key, 256 directories, trimmed oldest first |
| `src/sdlstub.c` | The SDL audio calls reelcore makes, answered without SDL (sound goes through SharedSoundBuffer) |
| `src/draw.[ch]` | Drawing: true-colour shapes and outline-font text for the dark windows |
| `src/version.h`, `src/proginfo.h` | The version; the Info window (from riscos-ffmpeg) |
| `app/!Matinee` | `!Boot`, `!Run`, `!Help` |
| `build/` | `build.sh` (compile, link, AIF, checks), `package.sh` (the RISC OS zip), `env.sh` |
| `tools/` | `mksprites.py` (the icon), `mkrozip.py`, `check-stack-probes.py`, `check-unixlib.sh` |
| `third_party/cjson` | cJSON 1.7.18 (MIT) |
| `third_party/pthreadticker` | UnixLib's PThreadTicker module 0.03, which `!Run` loads |
| `tests/host/fakeplex.py` | A stand-in for plex.tv and a Plex Media Server |
| `tests/host/core_test.c` | 98 checks against it, with Reel's own `sources.c` reading the hand-off |
| `tests/host/ui_test.c` | The front end against a scripted fake Wimp and a fake VideoOverlay, under qemu-arm (302 checks); it also draws the windows into pictures |
| `tests/host/fake_reelcore.[ch]` | A stand-in for reelcore, for ui_test: a video that plays by the fake clock |
| `tests/host/net_sock.c` | `net.h` over plain sockets, for ui_test |
| `tests/host/run.sh`, `mutate.sh` | Build and run the tests; break the code on purpose and check they notice |
| `docs/` | Feasibility, design, and the toolchain and build |

## Building and testing

See `docs/TOOLCHAIN.md`. In short:

```
GCCSDK_ENV=~/gccsdk/env ELF2AIF=path/to/elf2aif build/build.sh && build/package.sh
REEL_SRC=../riscos-ffmpeg tests/host/run.sh
REEL_SRC=../riscos-ffmpeg tests/host/mutate.sh
```

Expected: `core_test: 109 checks, 0 failed`, `ui_test: 302 checks, 0 failed`, and
`48 mutations, 0 survived`. ui_test also leaves `signin.ppm`, `browser.ppm`, `details.ppm`,
`player.ppm` and `upnext.ppm` in its output directory: the window's pages as its fake
Wimp drew them (with a stand-in font, made-up posters and a made-up picture).
