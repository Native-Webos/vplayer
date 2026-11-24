# Roadmap

Each phase ends with something that runs on the TV. "Done" needs a device record: model, webOS
release and date.

## P0: libVLC runs on the TV  ← in progress
- [x] Project layout, kit app scaffold (`app/`, builds and passes its host smoke test)
- [x] Docker build environment (`libvlc/Dockerfile`) with the webosbrew NDK mounted
- [x] VLC 3.0.24 contribs cross-built (`libvlc/build.sh contribs`); two VLC patches in `libvlc/patches/vlc`
- [x] libvlc + libvlccore + 317 plugins cross-built, staged, stripped (`build/libvlc/runtime`, 113 MB)
- [x] DT_NEEDED of libvlc and every plugin: glibc only (≤ GLIBC_2.12), plus `libpulse.so.0`,
      `libasound.so.2` and `libGLESv2.so.2` in seven plugins. No libstdc++, libgcc_s or libatomic.
- [x] App links libVLC, starts it on a worker and shows version, startup time and audio outputs;
      ipk (49 MB) passes `check-package.py`, and `verify.sh` reports 7/7 loadable for webOS 5.3.1 to 11.2.0
- [x] **On the TV** (OLED65C1PTB, webOS 6.5.3, 2026-11-24): the app shows "libVLC 3.0.24 Vetinari is
      running", started in 370 ms (no plugin cache), audio outputs adummy, afile, alsa, amem, pulse
- [x] On the TV, `vlc` CLI: 427 modules, none failed to load; MP3 through `pulse` reached sink
      `pcm_output` (lg115x, the real speaker output); software decode benchmark in RESEARCH.md §9
- [ ] Generate `plugins.dat` on the TV and decide how to ship it

## P1: the app plays something (software path)
- [x] Package libvlc + plugins + lua into the ipk (`wnk_add_webos_package` BUNDLED_LIBS/EXTRA_DIRS);
      set `VLC_PLUGIN_PATH` and `VLC_DATA_PATH` before `libvlc_new`
- [x] `VlcEngine` (libvlc_new on a worker) and `Playback` (app/player/playback.hpp): VLC software
      playback, plus a demo engine (test pattern, fake clock) for host builds
- [x] Video: libvlc video callbacks in I420 → `FrameSink` (three buffers, no waiting) → `VideoView`
      (GlView: three luminance textures, BT.601/709 limited-range shader, letterbox, crop of the coded
      size to the track's visible size)
- [x] Audio: VLC's pulse aout; subtitles blended by VLC before the callback (sidecar files untested)
- [x] On the TV 2026-11-24 (OLED65C1PTB, webOS 6.5.3): H.264 1280×720 MP4 plays with sound, ~190% CPU,
      occasional audio resampling ("playback too late"); HEVC 1080p plays but late frames (210% CPU,
      software limit; needs P2)
- [x] App themed to the approved design (Inter, dark neutral palette, orange accent, white focus ring,
      pill tabs): `app/ui/theme.*`. On the TV 2026-11-24 (OLED65C1PTB, webOS 6.5.3)
- [x] Home: USB drive hero, network/stream shortcuts, "Recently added" (newest videos on the drives,
      scanned on a worker); Browse: drives → folders → files grid, breadcrumbs, Back goes up, drives
      polled every 2 s; Music/Network/Playlists placeholders; About dialog with the libVLC status
- [x] Launch parameter `{"browse": "<folder>"}` opens Browse there (also on relaunch)
- [x] Player screen (mockup 3): title pill, format chip, floating bar with seek bar, ±10 s, play/pause;
      auto-hide after 4 s; hidden: Left/Right skip, OK pauses; media keys; Back/Stop close (stop on a
      worker); screensaver blocked while playing. Launch param `{"play": "<path>"}`
- [x] Remote-key paths verified on the TV (pause, seek, panels) with `app/tools/tv-keys.sh`
- [x] Subtitles on the TV: a sidecar `.srt` is found and drawn by VLC in Inter (`--freetype-font`
      set to the app's font; VLC's default font file does not exist on webOS)
- [ ] **Software path drops frames in the app** (see RESEARCH.md §10): even MPEG-4 ASP 720p, which
      decodes at 4× real time in the CLI, shows 80–500 "picture is too late" per 12 s at 110–170% CPU
      of 400%. Not audio (same with `:no-audio`), not a CPU quota (no cgroup limit), not redraw rate,
      not thread count. Next: profile the vout thread (timestamps in lock/display callbacks), try
      `:no-drop-late-frames`, compare with the CLI using `--vout=vmem`
- [x] Screens: URL entry (Network → Open a network stream; Home → Open a stream)

## P2: hardware path (the reason this port is worth doing)
- [x] `modules/wnk_bridge`: our VLC 3 stream output, built out of tree (`libvlc/build.sh modules`) into
      lib/vlc/plugins/stream_out; hands each elementary stream (codec setup, HDR metadata, PTS/DTS,
      keyframe flags, flushes) to the app through a plain C sink; send blocks for back-pressure
- [x] `VlcSource` (app/player/vlc_source.*): a kit MediaSource on libVLC: parse → tracks; select starts
      `#wnk_bridge` or `#transcode{acodec=a52|s16l}:wnk_bridge` by the kit's audio_path(); Annex-B with
      parameter sets on keyframes, HDR10+ SEI stripped, ADTS for AAC, AV1 temporal units; seek through
      libVLC (fast seek) with packets dropped until a keyframe near the target
- [x] `HwPlayback`: kit PlaybackController + AutoBackend (Starfish, NDL) behind the player's Playback
      interface; UI cleared to transparent over the video plane; automatic fallback to the software
      path when the TV decoder refuses a stream before the first picture
- [x] On the TV 2026-11-24 (OLED65C1PTB, webOS 6.5.3):
      - HEVC 3840×2160 HDR10 + TrueHD 7.1 (→ AC-3 5.1 by VLC) MKV: Starfish, HDR10 metadata sent,
        0 frames dropped, TV shows its HDR badge
      - H.264 720p + AAC MP4: Starfish, AAC with ADTS; seek to 2:00 lands on the keyframe at 1:42.97
        and plays 311 ms after Seek, 0 dropped
      - audio-only MKV (TrueHD → PCM): NDL
      - MPEG-4 ASP AVI: refused by the TV decoder → software path on its own
- [x] Pause/resume, ±10 s skips, audio-track switch and colour keys with remote keys on the TV
      (RESEARCH.md §13); skips land on the keyframe before the target
- [x] Text subtitles on the hardware path (RESEARCH.md §12): SubRip, ASS/SSA (styled by libass), MP4
      timed text and WebVTT from VLC's subtitle streams, files beside the media, the panel's track
      choice and delay; within a frame of the picture. On the TV 2026-11-24
- [x] PGS and DVD subtitles on the hardware path, decoded by the app; on the TV 2026-11-24
- [x] DVB subtitles on the hardware path (host-tested)
- [x] Teletext subtitles on the hardware path (host-tested; ASCII only, no national sets)
- [x] SubRip `{\an8}` (top) placement and tag styling on the hardware path (libass; host-tested)
- [x] Aspect ratio and zoom on the hardware path (the picture's rectangle on the video plane; host-tested)
- [x] Speed on the hardware path: VLC decodes audio to PCM, the app time-stretches it (RESEARCH.md §15)
- [x] Chapters on the hardware path: listed from libVLC, chosen through the controller's seek; on the TV 2026-11-24
- [x] Dolby Vision declared to Starfish (RESEARCH.md §14): configuration record from MKV/MP4, or RPUs in
      the first keyframe; Profile 7 fed as 8.1. On the TV 2026-11-24
- [x] Seek precision: lands on the keyframe at or before the target (was up to 9 s early)
- [x] Settings → Decoder: Automatic / Software only (host-tested)

## P3: VLC's sources
- [x] Network tab (RESEARCH.md §16): servers found by libVLC (UPnP/DLNA, mDNS), servers added by
      address (SMB2/3, NFS, FTP, SFTP, WebDAV), folders through libVLC, sign-in through libVLC's dialogs
      (credentials in its keystore), open a stream by address, recent addresses. Host-tested only
- [x] SMB2/3 in the libVLC build (smb2 module, libsmb2); SMB1/NetBIOS (libdsm) not buildable on glibc 2.12
- [x] Resume points and "Continue watching" on Home; Settings: decoder, resume. Host-tested only
- [ ] On the TV: discovery, an SMB share with a password, a DLNA server, an HLS stream
- [x] Playlists tab: M3U/M3U8 (IPTV groups and logos), PLS, XSPF, local and by address. Host-tested only
- [x] Music tab: albums from folders with covers, shuffle; cover in the player. Host-tested only
- [ ] The TV run of docs/TV-TEST-PLAN.md
- [x] Video thumbnails (the app's own libVLC thumbnailer; host-tested only for its files, the grab needs the TV)

## P4: player feature parity
- [x] Player bar: previous/next file in the folder (queue = the folder's videos/audio/disc images),
      ±10 s, play/pause, Audio, Subtitles, speed, Picture, More; chapter name, chapter ticks on the
      seek bar, A-B range, "Ends at"; Channel +/- and colour keys
- [x] Side panel (app/player/player_screen.cpp, PlayerPanel), all through libVLC 3:
      Audio (track, delay, equalizer presets, volume boost to 200 %), Subtitles (track/off, delay,
      load a subtitle file from the folder), Video (aspect Fit/Fill/16:9/4:3/21:9/Stretch and zoom in
      VideoView; deinterlace; brightness/contrast/saturation/gamma/hue), Playback (speed 0.25–4×,
      A-B repeat, repeat off/file/folder, play next automatically, sleep timer incl. end of file,
      chapters), Info (file, streams, frames shown/dropped, input bitrate; refreshed every second)
- [x] On the TV 2026-11-24: Subtitles tab lists the sidecar SRT; Info tab shows HEVC 3840×2160 24 fps
      and TrueHD 8 ch for thd-20s.mkv, and 203 shown / 66 dropped for MPEG-4 720p (the P1 drop issue)
- [x] Remote interaction on the TV (pause, seek, panels) with app/tools/tv-keys.sh (RESEARCH.md §13)
- [x] Disc titles (software path), discs start at the main film; jump to time; bookmarks; subtitle
      size and background (Settings); search. Host-tested
- [x] Subtitle colour, audio to the TV (decoded PCM option), audio delay on the hardware path,
      snapshots (software path), your own playlists, video thumbnails. Host-tested
- [ ] Teletext pages (not subtitles) as a viewer
- [ ] DVD/Blu-ray ISO and folders with menus (software path)
- [ ] Video: aspect, crop, zoom, deinterlace, adjust; audio: equalizer, normalize, stereo mode
- [ ] Own GLES2 `vout_display` module for the software path (no vmem copy, VLC subpictures drawn by VLC)
- [ ] Sleep timer, repeat/shuffle, bookmarks, snapshot

## P5: release
- [ ] Final name and icon (not "VLC", see RESEARCH.md §6)
- [x] `--enable-merge-ffmpeg`: runtime 114 → 88 MB, package 50.8 → 37.6 MB
- [x] Firmware check (kit's verify.sh): loadable on webOS 5.3 to 11.2; plugins need glibc ≤ 2.12 only
- [ ] Prune plugins further (each one is a VLC feature: decide per plugin). Was: (FFmpeg is linked into
      avcodec, avformat and avparser separately, ~41 MB of the 113 MB)
- [ ] webosbrew Homebrew Channel submission; GPL source release
