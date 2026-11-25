# Pylon: libVLC for LG webOS

Pylon (code name VPlayer, app id `com.sostk.vplayer`) is a native LG webOS app that brings VLC's playback engine (libVLC 3.0.24) to the TV: a remote-driven
UI on [webos-native-app-kit](https://github.com/Native-Webos/webos-native-app-kit), with the TV's hardware decoder
for 4K/HDR and VLC's software decoders for everything else.

Not affiliated with VideoLAN. "VLC" is a VideoLAN trademark.

- Design and evidence: [docs/RESEARCH.md](docs/RESEARCH.md)
- Plan and status: [docs/ROADMAP.md](docs/ROADMAP.md)

## Compared with VLC

Against VLC 3 for desktop and Android, as of 2026-11-25. Most rows were checked on an LG OLED65C1
(webOS 6.5.3); see [docs/TV-TEST-PLAN.md](docs/TV-TEST-PLAN.md) for each result.

✅ same as VLC (checked on the TV) · 🟡 built, not fully checked on the TV · ❌ missing · ➖ not
planned for a TV app

**In short:** playing files, network sources, streams and discs is at VLC's level, and on the TV
decoder it goes beyond what VLC can do on webOS (4K HDR10 and Dolby Vision with no dropped frames).
What is missing is mostly VLC's work beyond playback: converting and streaming out, its interfaces
and extensions, and some video filters.

### Playing

| Feature | VPlayer | Notes |
|---|---|---|
| Containers and codecs (VLC's demuxers and decoders) | ✅ | All of VLC 3.0.24's, software path |
| TV hardware decoder (H.264, HEVC, VP9, AV1 up to 4K) | ✅ | Beyond VLC: libVLC demuxes, the TV decodes; falls back to software on its own |
| HDR10, Dolby Vision | ✅ | HDR10 metadata sent to the TV; Dolby Vision 8.1, and 7 fed as 8.1, checked on the TV (profile 5 on the PC only). HDR10+ dynamic metadata is dropped |
| Audio to the TV: AC-3/E-AC-3 passed through, others (TrueHD, DTS, FLAC, ...) re-encoded to AC-3 or PCM | ✅ | Or always decoded PCM (Settings) |
| Seeking, ±10 s, jump to time, chapters | ✅ | Both paths |
| Speed 0.25–4× | ✅ | Pitch kept on both paths |
| A-B repeat, repeat, sleep timer, bookmarks, resume | ✅ | Resume and "Continue watching" across launches |
| Frame by frame | 🟡 | Software path only: the TV decoder cannot step |
| DVD images with menus | ✅ | Remote drives the menu; patched libdvdnav (see `libvlc/patches`) |
| DVD folders (VIDEO_TS) | ✅ | Shown as a disc tile in the folder |
| Blu-ray images and folders (BDMV) | 🟡 | Titles; HDMV menus untested (no test disc); BD-J menus need Java, which the build lacks |
| 360° video | ❌ | |

### Sources

| Feature | VPlayer | Notes |
|---|---|---|
| USB drives | ✅ | |
| SMB 2/3 with sign-in, saved sign-ins | ✅ | Passwords saved encrypted where the TV can (webOS 24+), else in the app's storage |
| SMB 1, NetBIOS names | ❌ | libdsm does not build against the TV's glibc 2.12 |
| DLNA/UPnP servers | ✅ | Found by discovery |
| FTP, SFTP | ✅ | SFTP works with current OpenSSH (libssh2 on OpenSSL, our change) |
| NFS, WebDAV | 🟡 | Built, no test server yet |
| HTTP(S), HLS | ✅ | The TV's certificate store |
| DASH, RTSP, RTP/UDP multicast | 🟡 | VLC's modules are in; untested |
| M3U/M3U8 (IPTV, groups, logos), PLS, XSPF | ✅ | iptv-org's 12 000-channel list loads in 0.4 s |
| Your own playlists | 🟡 | Needs typing a name on the TV keyboard; untested |
| Lua scripts (YouTube and other site parsers) | 🟡 | Bundled; untested |
| Optical drives, capture devices, screen recording | ➖ | |

### Subtitles and captions

| Feature | VPlayer | Notes |
|---|---|---|
| SubRip, ASS/SSA (styled, attached fonts), WebVTT, MP4 text | ✅ | Both paths |
| PGS, DVD, DVB bitmaps | ✅ | DVB host-tested |
| Teletext subtitles | 🟡 | Host-tested |
| Teletext pages as a viewer | ❌ | |
| CEA-608 and CEA-708 closed captions | ✅ | |
| Files beside the video (local and on shares), loading another | ✅ | |
| Delay, size, colour, box | ✅ | |
| Download from OpenSubtitles | 🟡 | With your own API key; not yet tried with a real key |

### Audio and video

| Feature | VPlayer | Notes |
|---|---|---|
| Audio track and delay | ✅ | |
| Equalizer (presets and own bands), volume to 200 %, normalise, wider stereo, compressor, spatializer, pitch shift | 🟡 | VLC loads them on both paths on the TV; not checked by ear yet |
| Aspect ratio and zoom | ✅ | Both paths |
| Crop (VLC's ratios) | ✅ | Both paths |
| Deinterlace, picture adjust (brightness, contrast, ...) | 🟡 | Software path only: the TV decoder's picture never reaches VLC |
| Rotate and mirror | 🟡 | Software path only: the TV decoder's picture cannot turn |
| Other video filters (sharpen, blur, motion, puzzle, ...) | ❌ | |
| Snapshot | 🟡 | Software path only |
| Audio visualisations | ✅ | VLC's spectrum, oscilloscope, VU meter, spectrometer, for music |

### Beyond playback

| Feature | VPlayer | Notes |
|---|---|---|
| Music library (albums, covers, shuffle) | 🟡 | From folders; no tag-based library |
| Video thumbnails, search | ✅ | |
| Convert/save, stream out, record | ➖ | |
| Web, telnet and HTTP remote interfaces | ❌ | |
| VLC extensions (Lua add-ons), skins | ➖ | |
| Every action with arrows, OK and Back (no colour keys needed) | ✅ | The pointer works too |

## Layout

| Path | What |
|---|---|
| `app/` | The webOS app (C++17, kit app shell). Created with the kit's `tools/new-app.py`. |
| `libvlc/` | Cross-build of VLC 3.0.24 contribs + libvlc + plugins, inside Docker. |
| `libvlc/patches/` | Our patches to VLC 3.0.24 and its contribs (applied by `libvlc/build.sh`). |
| `modules/` | Our own VLC modules: `wnk_bridge` (streams to the TV decoder), `wnk_keystore` (saved sign-ins). |
| `third_party/` | VLC source checkout (not committed). |
| `build/` | All build output (not committed). |

## Build

Needs: Docker, the webosbrew NDK at `~/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot` (or
`WEBOS_NDK=…`), and the kit at `../native-webos/webos-native-app-kit` (or `WNK_KIT_DIR=…`).

```sh
libvlc/build.sh image       # once
libvlc/build.sh contribs    # ~1 h the first time
libvlc/build.sh vlc         # → build/libvlc/stage/usr/lib/{libvlc.so*,libvlccore.so*,vlc/plugins}

cd app
./build.sh                  # host build + smoke test
./build.sh webos            # ipk
./build.sh install && ./build.sh launch
```

## Trying it

- Desktop screenshots: `app/tools/host-shots.sh OUT_DIR [SAMPLE_FOLDER]` (the sample folder appears
  as an extra drive through `VPLAYER_EXTRA_ROOT`).
- On the TV, open a folder directly: `ares-launch -d tv com.sostk.vplayer -p '{"browse":"/tmp/usb/sda/sda1"}'`.

## Licence

GPL-3.0-or-later (the kit is GPL-3.0+; libVLC is LGPL-2.1+, VLC modules GPL-2.0+/LGPL).
