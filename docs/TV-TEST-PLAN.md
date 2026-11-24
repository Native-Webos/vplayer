# TV test plan

What has been built and tested on the desktop host but **not yet on the TV** (OLED65C1PTB,
webOS 6.5.3), followed by a short regression pass over what the TV already verified. Each item says
how to test it, what should happen, and what the log shows.

## Setup

- Test media: `app/tools/test-media/make.sh OUT_DIR` on the desktop, then copy `OUT_DIR` to the USB
  drive as `wnk-test/subs/` (`ares-push -d tv OUT_DIR/. /tmp/usb/sda/sda1/wnk-test/subs/`).
- Install: `cd app && ./build.sh webos && ARES_DEVICE=tv ./build.sh install`.
- Log: `/tmp/com.sostk.vplayer/events.log` on the TV
  (`ares-shell -d tv --no-pty -r 'tail -f /tmp/com.sostk.vplayer/events.log'`).
- Remote keys without the remote: `app/tools/tv-keys.sh up down ok back red …`.
- Launch with parameters: `ares-launch -d tv com.sostk.vplayer -p '{"play":"<path>"}'`
  (also `"at"` seconds, `"panel"`, `"decoder"`, `"vlcOptions"`, `"browse"`).
- Needed on the network for section 3: a DLNA server (a NAS, Plex, Jellyfin or minidlna), an SMB share
  with a password (a NAS or Windows), optionally NFS/FTP/SFTP servers.

## 1. Install and start (new permissions)

| # | Test | Expected | Log |
|---|---|---|---|
| 1.1 | Install the new package | Installs; `database.operation` is asked for in appinfo.json | — |
| 1.2 | Start the app | Home appears; the key/value store is DB8 | `storage.kv supported db8`, `[history] 0 entries` |
| 1.3 | HTTP transport | libcurl found | `net.http supported` (and an http cache line) |
| 1.4 | libVLC with SMB2 and one merged FFmpeg plugin | starts as before (~430 → fewer modules); MKV/MP4/AVI still open | `libVLC 3.0.24 … ready` |
| 1.5 | webOS 7–11 TVs (a C2, a G4), if available | The app starts and plays; kit's verify.sh says the package loads on 5.3–11.2 | — |

## 2. Settings

| # | Test | Expected |
|---|---|---|
| 2.1 | Top bar → settings icon | "Settings": Decoder, Continue where I stopped, About VPlayer, Close |
| 2.2 | Decoder → Software (VLC) only, play `two-audio.mkv` | Format chip says "software"; restart the app: still software |
| 2.3 | Decoder back to Automatic | The next file plays on the TV decoder again |
| 2.4 | About VPlayer | libVLC version and start time |

## 3. Network (never run on the TV)

| # | Test | Expected | Log |
|---|---|---|---|
| 3.1 | Network tab, wait 5 s | DLNA servers appear (and SMB/NFS NAS boxes that announce themselves over mDNS) | `[network] discovery: upnp`, `discovery: microdns` |
| 3.2 | Open a DLNA server | Its folders; a video plays on the TV decoder | `[network] list upnp://…` |
| 3.3 | Add a server → SMB, the NAS's address, a share | A sign-in dialog appears (user name, password, "Remember on this TV") | `[dialogs] sign-in sent` |
| 3.4 | Sign in, remember on | The share's folders; leave and open again: no second prompt (libVLC keystore) | — |
| 3.5 | Wrong password | The dialog comes back or an error notice; no hang | — |
| 3.6 | Play an MKV from the share, skip ±10 s | Plays on the TV decoder; skips land as on USB | `seek landed` |
| 3.7 | A video with a `.srt` beside it on the share | Subtitles found and shown (VLC's autodetect over SMB) | `subtitle stream` |
| 3.8 | NFS, FTP, SFTP servers (if available) | Same as 3.3–3.6 | — |
| 3.9 | Open a network stream → `https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8` | HLS plays; it appears under recent addresses | — |
| 3.10 | An https stream with a self-signed certificate | A certificate question with VLC's choices | — |
| 3.11 | Red on a saved server / recent address | "Forgot …"; gone after a restart | — |
| 3.12 | Unplug the network while browsing | An error message in the folder, no freeze | — |
| 3.13 | Home → "Open a stream" card | The Network tab's address dialog | — |

## 4. Playlists and IPTV (never run on the TV)

| # | Test | Expected |
|---|---|---|
| 4.1 | Playlists tab | `local.m3u` and `local.pls` from the drive listed |
| 4.2 | Open `local.m3u` | Its two entries; one plays, Next goes to the other |
| 4.3 | Add a playlist → an IPTV provider's M3U address | Groups (group-title), then channels with logos; a channel plays |
| 4.4 | A large IPTV list (10 000+ channels) | Opens within a few seconds; scrolling stays smooth |
| 4.5 | An HLS master playlist added as a list | Plays as a stream instead of listing |
| 4.6 | A live TS channel | Plays; no resume point is kept (live) |
| 4.7 | Red on an added list, restart | Forgotten |

## 5. Music (never run on the TV)

| # | Test | Expected |
|---|---|---|
| 5.1 | A USB drive with `Artist/Album/*.mp3` and `cover.jpg` | Music tab: albums with covers, "Shuffle all" first |
| 5.2 | Open an album, play a track | Plays (software path, PulseAudio); the player shows the cover; the next track follows |
| 5.3 | Shuffle all | Tracks from every album in random order |
| 5.4 | FLAC, AAC/M4A, OGG, Opus, WAV tracks | All play |
| 5.5 | Unplug the drive | The albums go; plug it back: they return |

## 6. Resume and history (never run on the TV)

| # | Test | Expected | Log |
|---|---|---|---|
| 6.1 | Play `two-audio.mkv` for 1 minute, Back | Home opens on "Continue watching" with it ("From 1:00", minutes left) | `[history]` writes, no "not stored" |
| 6.2 | Open it again | Starts at about 1:00 with "Continuing from 1:00…" | `starting at` |
| 6.3 | Playback tab → Start from the beginning | Back to 0:00 | — |
| 6.4 | Play to the end | It leaves "Continue watching" | — |
| 6.5 | Restart the app, then reboot the TV | The list and resume points are still there (DB8) | `[history] N entries` |
| 6.6 | Settings → Continue where I stopped off | Files start at 0:00 | — |
| 6.7 | A network file and an IPTV channel | The network file resumes; the live channel is not kept | — |

## 7. Subtitles added since the last TV run

| # | Test | Expected | Log |
|---|---|---|---|
| 7.1 | `ass-font.mkv` | The centre line is in DejaVu Serif (the embedded font), the others in Inter | `1 fonts attached`, `1 attached font(s)` |
| 7.2 | `dvb.ts` (TV decoder) | "PGS N.0 s" labels shown as DVB bitmaps at the bottom centre, white with a black outline | `subtitle stream 2: dvbs` |
| 7.3 | `dvb.ts` with Settings → Software | VLC's own DVB rendering, for comparison | — |
| 7.4 | A real DVB recording (from a TV or set-top box) | Subtitles show and clear on time | — |
| 7.5 | A real MKV with ASS and fonts (anime release) | Styled as authored | — |

## 7b. Player additions (never run on the TV)

| # | Test | Expected |
|---|---|---|
| 7b.1 | `subs-srt-ass.mkv`, English SRT track, at 0:06–0:12 | The `{\an8}` line is at the top (libass); other lines at the bottom |
| 7b.2 | Settings → Subtitle size Huge, Subtitles on a dark box; SRT on both decoders | Bigger text on a box, on the TV decoder and in software |
| 7b.3 | Video tab → aspect 4:3, 16:9, Stretch, Fill; zoom 150 % on the TV decoder | The picture's area changes; Fill and zoom past the edges (check Starfish accepts it) |
| 7b.4 | Playback → Jump to time "0:30", "95", "1:75" | Jumps; the last says it is not a time |
| 7b.5 | Playback → Add a bookmark here, twice at different places; restart the app | Both bookmarks listed and jump there |
| 7b.6 | A DVD ISO and a Blu-ray ISO | Start at the main film, not the menu; Playback → Titles lists the rest (software path) |
| 7b.7 | Top bar → search, type part of a file name with the remote's keyboard | Results as you type; one plays |
| 7b.8 | Audio delay +0.5 s on the TV decoder (two-audio.mkv) | Audio comes later after a second or two; no stall | 
| 7b.9 | Settings → Audio to the TV: Always decoded, play thd-20s.mkv | Info: "To the TV … pcm"; plays |
| 7b.10 | Settings → Subtitle colour Yellow; SRT on both decoders | Yellow text |
| 7b.11 | Software decoder → Video → Take a snapshot | "Snapshot saved…" and a PNG in "VPlayer snapshots" on the drive (can the app write to USB?) |
| 7b.12 | Playback → Add to a playlist → New playlist; Playlists tab | The list with the file; plays; Red removes an entry / the list |
| 7b.13 | Browse a folder of videos, Home "Recently added" | Thumbnails appear one by one (log `[thumbs] … in N ms`); none made while a video plays |
| 7b.14 | A DVB recording with teletext subtitles (e.g. page 888) on the TV decoder | Subtitles show and clear, with the language's accented letters |

## 8. Software path (frame drops, RESEARCH.md §10)

| # | Test | Expected | Log |
|---|---|---|---|
| 8.1 | `full-range.mp4` with Software decoder | Plays; correct contrast (not washed out) | `decoder J420, taken as J420`, then every 5 s `vlc: 5 s: N pictures handed over … lost` |
| 8.2 | The phone recording `lv_0_….mp4` in software | Fewer lost pictures than before (66 of 269) | same lines |
| 8.3 | The MPEG-4 ASP AVI | Note the 5 s lines: are pictures lost (VLC) or gaps long (the app)? | same lines |

## 9. Regression pass (verified on the TV before; check quickly)

| # | Test | Expected |
|---|---|---|
| 9.1 | 4K HEVC TrueHD (`thd-20s.mkv`) | TV decoder, Dolby Vision 7→8.1, 0 dropped |
| 9.2 | `dv81.mkv` | TV shows its Dolby Vision badge |
| 9.3 | Skips ±10 s on `two-audio.mkv` and `thd-20s.mkv` | Land on the keyframe before the target |
| 9.4 | Audio switch to the French AC-3 track | Plays the AC-3 track |
| 9.5 | `chapters.mkv`: Playback → Ending | Jumps; bar shows "Chapter 3 · Ending" |
| 9.6 | Speed 1.5× on `two-audio.mkv`; 3× | 1.5× plays; 3× says "too fast… playing at 2×" |
| 9.7 | `subs-srt-ass.mkv`, `subs-tx3g.mp4`, `subs-webvtt.mkv`, `pgs-dvd.mkv`, `side car.mp4` | Each subtitle kind shows; switching tracks works |
| 9.8 | `sync.mkv` | Cue label within a frame of the burned-in clock |
| 9.9 | Home, Browse with the USB drive | As before |

## Not built yet

- Video thumbnails (libVLC 3 has no thumbnailer; would need a hidden player per file).
- Teletext subtitles on the TV decoder path (zvbi pages), closed captions (CEA-608/708).
- SMB1 shares and NetBIOS discovery (libdsm does not build against glibc 2.12).
- Reading Dolby Vision configuration records and attached fonts from network files (local files
  only; the Dolby Vision RPU scan works for network files).
