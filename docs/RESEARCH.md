# VLC on LG webOS: research and architecture

Status: research + bootstrap, 2026-11-24. Claims are tagged **[src]** (read in source), **[dev]**
(measured on a TV, with model/release), **[kit]** (carried over from webos-native-app-kit's own
evidence in `docs/research/` and `docs/compatibility/records.json`), or **[plan]** (a decision,
not yet proven).

## 1. Goal

Bring the features of VLC media player to LG webOS TVs as a native app: open almost any file,
stream or network share, with a TV interface driven by the Magic Remote. VLC upstream has never
shipped for webOS. There is no official port and no libVLC build for it.

## 2. Platform facts that constrain the design

| Fact | Consequence |
|---|---|
| Native apps are one ELF in an `.ipk`, launched by SAM; Developer Mode TVs are not rooted **[kit]** | Everything libVLC needs (libvlc, libvlccore, ~300 plugins, lua scripts) ships inside the ipk. |
| Toolchain: webosbrew buildroot NDK, GCC 12.2, **glibc 2.12**, armv7-a NEON **softfp** **[src: `gcc -v`, sysroot `libc-2.12.2.so`]** | VLC 3.0.x builds against old glibc through its `compat/`; VLC 4 (unreleased) needs newer C11/threads, so it is not a fit today. |
| Firmware libraries change SONAME between webOS releases (FFmpeg, curl, AcbAPI deleted at 5.0) **[kit]** | VLC's own contribs are built static into libvlc/plugins. Only `libpulse.so.0`, `libasound.so.2`, `libEGL.so.1` and `libGLESv2.so.2` come from the TV, and each one is needed only by the single plugin that uses it. |
| 4K/HEVC/HDR/Dolby Vision decode is possible only in the TV's hardware decoder, reached through **Starfish** (`libplayerAPIs`, webOS ≥ 4 / 5+) or **NDL directmedia**. Both take *elementary streams* and draw on a video plane under the UI, with their own A/V clock **[kit, native-jelly `src/starfish.c`]** | VLC cannot use its usual decoder → picture → vout chain for these. VLC has to demux and the TV has to decode and present (§4). |
| The ARM CPU decodes 1080p H.264 in software at best, and cannot decode 4K HEVC **[plan; to be measured]** | A software path is a fallback for codecs the hardware lacks (MPEG-4 ASP, WMV, RealVideo, Theora, …), not the main path. |
| Audio: PulseAudio is on the TV; SDL audio goes through it **[kit]** | VLC's `pulse` aout works for the software path. On the hardware path, audio must go into the same Starfish/NDL pipeline as video for lip-sync (AC-3/E-AC-3 passthrough, AAC with ADTS, otherwise PCM or AC-3 re-encode) **[kit `media.hpp`]**. |
| UI: SDL2 (webOS port) + GLES2 on Wayland; the kit owns the window, GL context and main loop **[kit]** | VLC's Qt/skins interfaces are not used. The UI is written in C++ on the kit, against the libVLC C API. |

## 3. Why VLC 3.0.24 (not 4.0)

- 3.0.24 is the newest stable tag on code.videolan.org (checked 2026-11-24). 4.0 is still `4.0-dev`.
- VLC 3's `src/linux/dirs.c` finds its plugin directory relative to `libvlccore.so` through
  `/proc/self/maps` **[src]**, so the ipk can carry `lib/vlc/plugins` with no fixed install prefix.
- VLC 3 contribs cross-compile for "linux" hosts other than desktop distros. Android builds them the
  same way.
- Cost: VLC 3 has no `libvlc_video_set_output_callbacks` (GL rendering into the app's own context).
  The software path starts with `vmem` (a CPU copy). §5 P3 replaces it with a VLC 3 `vout_display`
  module that draws into the kit's context.

## 4. Architecture

```
 ┌──────────────────────── VPlayer (native app, webos-native-app-kit) ───────────────────────┐
 │ UI: browse (USB, SMB, UPnP/DLNA, NFS, SFTP, FTP, URL, IPTV m3u), playlists, history/resume, │
 │     player OSD, track / subtitle / delay / speed / aspect menus, settings                   │
 │                       │ libVLC C API (media, media_player, media_discoverer, media_list)    │
 ├───────────────────────┼─────────────────────────────────────────────────────────────────────┤
 │ libVLC 3.0.24: access (file, http(s), smb2, nfs, sftp, ftp, rtsp, upnp, bluray/dvd ISO …),   │
 │   demux (mkv, mp4, ts, avi, ogg, …), playlist parsers, lua, subtitle decoders, packetizers   │
 │            │                                              │                                  │
 │  HARDWARE PATH (default)                      SOFTWARE PATH (fallback)                       │
 │  sout module "wnk" (ours): ES blocks            VLC decoders (avcodec, dav1d, …)             │
 │   → kit PlaybackController                      → vout: vmem → GL texture in the kit's        │
 │   → Starfish / NDL backend (TV decoder,           context (later: our own vout_display)      │
 │     video plane, A/V sync, passthrough)         → aout: pulse                                 │
 │  unsupported audio: VLC transcodes in the      filters, deinterlace, equalizer, snapshots    │
 │   sout chain (PCM or AC-3)                     all work here                                 │
 └─────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Hardware path design [plan].** VLC 3 already sends demuxed, packetized elementary streams to a
`sout_stream_t` (this is how its Chromecast renderer works: `modules/stream_out/chromecast`). Our
module receives `es_format_t` and `block_t` (PTS/DTS in µs, the same unit as the kit), converts
them to `wnk::media::Packet`, and feeds the kit's Starfish/NDL backend. The kit and native-jelly
have already solved the hard parts of those backends on device: HDR/DV metadata, seek, flush,
ADTS, and Feed backpressure. Backpressure works because VLC 3's decoder FIFO waits for a full sout
(`input_DecoderDecode(..., b_do_pace)`) **[src]**. Seek uses `sout_stream_t::pf_flush`. Track
switches are ES add/del. The open questions to answer on the TV first:
pause (VLC's input has no pause hook into sout; chromecast uses a demux-filter), position reporting
(the backend's presented time, not the demuxer's), and subtitle timing against the backend clock.

**Choosing a path [plan].** After `libvlc_media_parse`, the app compares the track codecs with the
backend's `BackendCaps`. If the video codec is hardware-decodable, it plays through
`--sout=#wnk` (with `transcode{acodec=…}` in front when the audio is not). Otherwise it plays
through the software path. The user can force either path per file.

## 5. VLC features on webOS

| VLC feature | webOS plan | Path |
|---|---|---|
| Play local files (USB drives) | kit `platform/` USB listing → `file://` | both |
| Network streams: HTTP(S), HLS/DASH (VLC adaptive), RTSP/RTP/UDP multicast, SRT, RIST | VLC access modules | both |
| Network shares: SMB (smb2/dsm), NFS, SFTP, FTP, UPnP/DLNA, mDNS | VLC services discovery + access | both |
| Containers: MKV, MP4, TS/M2TS, AVI, OGG, FLV, WMV/ASF, MOV, … | VLC demux | both |
| Video codecs: H.264, HEVC, VP9, AV1, MPEG-2 (+HDR10/HLG/DV) | TV hardware | HW |
| Other video codecs: MPEG-4 ASP, WMV/VC-1, Theora, RealVideo, ProRes, … | avcodec / dav1d | SW |
| Audio: AAC, AC-3, E-AC-3 (Atmos), MP3, Opus, PCM; DTS/TrueHD/FLAC/Vorbis/WMA | passthrough or transcode in sout; software path decodes everything | both |
| Subtitles: SRT, ASS/SSA (libass), PGS, DVB, DVD, WebVTT, TTML, teletext | VLC decodes; drawn by the app (kit `ass_renderer`, `subtitle_view`) on HW path, by VLC's SPU on SW | both |
| Subtitle/audio delay, track selection, external subtitle files | libVLC API | both (delay on HW path needs PTS shifting in the bridge) |
| Playback speed | SW: full; HW: depends on backend rate support | SW / partial HW |
| DVD / Blu-ray (ISO, folders), menus, chapters, titles | libdvdnav / libbluray (no AACS/BD-J) | SW for menus |
| Video filters, deinterlace, adjust, crop, zoom, aspect | SW: VLC filters; HW: aspect/crop through plane placement | SW (HW partial) |
| Audio equalizer, normalize, spatializer, stereo mode | VLC audio filters | SW (HW: when decoded to PCM) |
| Playlists (m3u, xspf, pls), repeat/shuffle, IPTV | libVLC media_list | both |
| Resume, history, bookmarks, A-B loop, sleep timer | app (DB8 via kit storage) | both |
| Snapshot, recording | VLC `scene`/sout duplicate | SW / later |
| Web interface / remote control (lua http) | VLC lua `http` intf, if the TV's network allows it | later |
| Qt / skins / desktop integration, v4l2, screen capture, LIRC, Chromecast sender | not applicable on a TV | — |

## 6. Risks

1. **Starfish/NDL behind a sout**: unproven. The first technical milestone (§7, P2) is to play one MKV
   through it, then pause, seek, and switch tracks.
2. **Software decode speed** on the TV CPU: measure early with `vlc --play-and-exit --vout=dummy`
   and `--codec=avcodec --avcodec-threads=4`.
3. **Package size**: libVLC with all plugins is about 60–100 MB unstripped. Strip, and drop plugins
   that are never used on a TV.
4. **Plugin cache** (`plugins.dat`): qemu cannot run the glibc 2.12 binaries (it segfaults in the
   build container). Plan: run `vlc-cache-gen` on the TV once and ship the file, or start without a
   cache (slower first start).
5. **Licensing**: libVLC is LGPL-2.1+, its modules are GPL-2.0+ or LGPL, and the kit is GPL-3.0+.
   The combined app is GPL-3.0-or-later and its full source must be published.
   **Trademark**: "VLC" and the cone are VideoLAN trademarks. The app must not use either as its
   name or icon without VideoLAN's permission. "VPlayer" is a placeholder; "powered by libVLC" is
   a factual description.

## 7. Prior art used

- `~/Desktop/native-webos/webos-native-app-kit`: app shell, UI, focus, input, packaging, and the
  Starfish/NDL backends, HDR/DV handling and firmware checks. Used by reference: VPlayer's `app/`
  was created with `tools/new-app.py`.
- `~/Desktop/native-jelly` (PlxNative lineage): Starfish + ACB/exported-window plane binding,
  verified on an OLED65C1 running webOS 6.5.3.
- moonlight-tv: SDL-webOS, NDL and Starfish usage.
- VLC Android (`vlc-android`): how a libVLC 3 app ships plugins and drives libVLC from a TV UI.

## 8. Build findings (2026-11-24)

What it took to cross-build VLC 3.0.24 with the webosbrew NDK. Each one is now handled in `libvlc/`.

| Problem | Fix |
|---|---|
| The NDK's `bin/` also contains buildroot *host* tools (perl without FindBin, a pkg-config that prefixes the sysroot) | `env.sh` puts the NDK at the end of PATH |
| libpng refuses `PNG_ARM_NEON=check` when the compiler already enables NEON | patch 0001: `on` for webOS |
| glibc 2.12's `<assert.h>` has no C11 `static_assert` (FFmpeg, postproc configure) | patch 0002: `-Dstatic_assert=_Static_assert` for those two |
| VLC checks for `ffsll` in `<strings.h>`; glibc 2.12 declares it in `<string.h>`, so VLC's own prototype clashes in C++ | `vlc_cv_replace_decl_ffsll=yes` |
| `luac` must run on the build machine | contribs' host `luac` (`build/libvlc/bin/arm-webos-linux-gnueabi-luac`, 32-bit bytecode) |
| libtool's C++ links use `-nostdlib … -lstdc++ -lgcc_s`, which defeats `-static-libstdc++`; firmware libstdc++ is too old for GCC 12 until webOS 11 (fw-lib-matrix) | rewrite libtool `postdeps` and the contribs' `.pc` files to the static archives |
| `libatomic.so.1` is missing on webOS 4.x; FFmpeg's `.pc` pulls it in, but ARMv7 does atomics inline | `--as-needed`, and the entry is removed from any file that imports no `__atomic_*` symbol (checked) |
| libtool RPATHs (`/usr/lib/vlc`, the NDK's lib dir) | patchelf: plugins `$ORIGIN/../..`, nothing else |
| qemu-arm segfaults on the glibc 2.12 binaries | no plugin cache at build time (generate it on the TV; see ROADMAP P0) |
| vpx contrib (its asm/rtcd generation) | not built: FFmpeg decodes VP8/VP9 |

## 9. First device run (OLED65C1PTB, webOS 6.5.3, 2026-11-24) [dev]

- libVLC 3.0.24 and all 317 plugins load; `vlc --list` shows 427 modules, none failing.
- PulseAudio: the `pulse` aout connects to `/var/run/pulse/native` and plays to sink `pcm_output`
  (lg115x). The other 9 sinks are null sinks.
- In the app: `libvlc_new` takes 370 ms without a plugin cache. Setting `VLC_PLUGIN_PATH` as well
  made VLC scan the plugins twice (502 ms, every module listed twice), so it is not set.
- The CPU offers 3 cores to apps (Cortex-A55, `CPU part 0xd05`). Software decode with `--vout=dummy
  --aout=dummy` (no picture output, so real playback costs more), 20 s synthetic clips (testsrc2 + noise):

| Clip | 1× | 2× | 4× |
|---|---|---|---|
| H.264 720p30, 5 Mbit/s | ok | ok | late |
| MPEG-4 ASP 720p30, 4 Mbit/s | ok | ok | ok (6 late) |
| H.264 1080p30, 10 Mbit/s | ok (1 late) | late | late |
| HEVC 1080p30, 6 Mbit/s | ok | late | late |

  Conclusion: the software path is good for ≤720p and legacy codecs. 1080p H.264/HEVC has no headroom
  even before drawing, so the hardware path (P2) is needed for anything 1080p and above.
- Pitfalls: the `vlc` CLI refuses root (run it as `nobody` via `su`); `luna-send` needs a pty
  (`script -qec "ares-shell --pty …"`); the screen capture service on webOS 6 is
  `luna://com.webos.service.capture/executeOneShot` (not `…tv.capture`).

## 10. First playback in the app (OLED65C1PTB, webOS 6.5.3, 2026-11-24) [dev]

- Path: libvlc video callbacks (I420) → FrameSink (three buffers) → GlView YUV shader. libVLC hands the
  callbacks the coded size (1280×738 for 1280×720 H.264); the visible size comes from the track.
- Works: H.264 720p MP4 and HEVC 1080p MKV play with sound through PulseAudio; letterboxing; sidecar
  SRT subtitles (after pointing `--freetype-font` at a bundled TTF); the screensaver is blocked while
  playing and released on close; libVLC stop runs on a worker.
- Problem: frames are dropped ("picture is too late", 17–50 ms late) on every file tried, including
  MPEG-4 ASP 720p that the CLI decodes at 4× real time. CPU stays at 110–170% of 400% (the TV has 4
  cores online; ares-shell's `nproc` said 3). Ruled out: audio (`:no-audio`), cgroup CPU quota (none,
  cpu.shares 1024, SCHED_OTHER), redrawing the UI at 60 Hz (now redrawn per video frame), decoder
  threads (1, 2, auto), direct rendering. Counts vary 80–500 between identical runs, which points at
  scheduling/timing rather than throughput. `--avcodec-fast --avcodec-skiploopfilter=1` are set but
  did not change it measurably.
- Debug hook: launch parameter `{"play": "<path>", "vlcOptions": [":option", ...]}` passes per-media
  options to libVLC.

## 11. Hardware path through libVLC (OLED65C1PTB, webOS 6.5.3, 2026-11-24) [dev]

- VLC's `smem` stream output passes compressed video, but not the codec setup, DTS, keyframe flags or
  flushes, and only raw audio: not enough to feed a hardware decoder. Our own stream output
  (`modules/wnk_bridge`, ~200 lines of C) passes all of that through a plain C sink whose address
  travels in the sout chain (`#wnk_bridge{sink=<address>}`).
- VLC 3 paces a sout by its decoder FIFO (`input_DecoderDecode(..., b_do_pace)` waits at 10 blocks), so a
  sink that blocks in send makes libVLC read only as fast as the TV consumes.
- The kit's PlaybackController drives the decoder unchanged: VlcSource is just another MediaSource.
  Starfish Load of a 4K HEVC stream took 2.3 s; HDR10 mastering metadata from VLC's es_format reached
  setHdrInfo. VLC's packetizers emit Annex-B with Annex-B extradata for H.264/HEVC from MP4/MKV.
- `:input-fast-seek` makes libVLC land on a keyframe; for a phone recording with long GOPs that was
  17 s before the target. Precise seeking needs feeding from the keyframe and hiding frames before the
  target (or a decoder that honours a decode-from time).
- Launch parameters for testing without a remote: `{"play": path, "at": seconds, "panel": tab,
  "decoder": "software"|"hardware", "vlcOptions": [...]}`.

## 12. Subtitles on the hardware path (OLED65C1PTB, webOS 6.5.3, 2026-11-24) [dev]

- With `:no-sout-spu` removed, VLC hands the selected subtitle stream to the sout like any other ES.
  Matroska marks every track default unless told otherwise, so VLC may start with the last one; the
  viewer's choice goes through `libvlc_video_set_spu`, which makes VLC add the new stream to the sout.
  Files beside the media are found by VLC itself (`sub-autodetect-file`), names with spaces included.
- What VLC's demuxers send (VLC 3.0.24): `subt` plain text (SubRip keeps `<i>`/`<font>` tags and
  `{\an8}`), `ssa ` the Matroska event form "ReadOrder,Layer,Style,..." with the script header as
  extradata (VLC's own subtitle file reader produces the same form), `tx3g` a 16-bit length then text
  then style boxes, `wvtt` ISO boxes (vttc → payl) for MP4 and for Matroska/WebM, which VLC repacks.
  The encoding is only declared when the demuxer knows it (Matroska: UTF-8); VLC's decoder falls back
  to Windows-1252 for invalid UTF-8, and so does `app/player/subtitle_text.cpp`.
- The kit's `PlaybackController::subtitles_now()` places cues on its estimated clock, which starts at
  the first packet fed: with Starfish that ran 0.7–0.9 s ahead of the picture. The snapshot's
  position comes from Starfish's FRAMEREADY (the presented PTS): cues placed at it were within one
  frame (measured with a burned-in timer and a cue every 200 ms). The delay slider is applied on top.
- ASS/SSA tracks are drawn by the kit's libass renderer from the script header (positions, colours,
  styles as authored); other text is laid out by the kit's subtitle view. SubRip's `{\an8}` is not
  honoured there (VLC's own renderer puts it at the top).
- Bitmap formats (PGS, DVD, DVB) still reach the bridge but are not drawn: VLC 3 would have to decode
  them in the module (its SPU decoders produce subpicture regions), or the app decodes them itself.
- The rooted TV accepts input events written to `/dev/input/event1` (the IR remote): `app/tools/tv-keys.sh`
  presses remote keys, which the app cannot tell from the real remote (Back is code 412).
