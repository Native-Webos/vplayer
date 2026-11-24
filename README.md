# VPlayer: libVLC for LG webOS (working name)

A native LG webOS app that brings VLC's playback engine (libVLC 3.0.24) to the TV: a remote-driven
UI on [webos-native-app-kit](../native-webos/webos-native-app-kit), with the TV's hardware decoder
for 4K/HDR and VLC's software decoders for everything else.

Not affiliated with VideoLAN. "VLC" is a VideoLAN trademark.

- Design and evidence: [docs/RESEARCH.md](docs/RESEARCH.md)
- Plan and status: [docs/ROADMAP.md](docs/ROADMAP.md)

## Layout

| Path | What |
|---|---|
| `app/` | The webOS app (C++17, kit app shell). Created with the kit's `tools/new-app.py`. |
| `libvlc/` | Cross-build of VLC 3.0.24 contribs + libvlc + plugins, inside Docker. |
| `libvlc/patches/` | Our patches to VLC (none yet). |
| `modules/` | Our own VLC modules (the hardware-path bridge, the GL vout). Not started. |
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
