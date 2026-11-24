#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build VLC's contribs (third-party libraries) for webOS, static and position-independent.
source "$(dirname "$0")/env.sh"

# Not built, and why:
#   GUI toolkits / desktop-only: qt qtsvg sdl SDL_image caca ncurses glew sparkle breakpad growl
#                                 xau xcb xcb-proto xorg-macros xproto wix wixlzx mingw64 d3d9 d3d11 amf vpl
#   encoders (transcoding to files is not a TV use case; revisit for the sout path): x262 x264 x265
#                                 lame twolame shout
#   visualisations / niche, heavy: goom projectM chromaprint sidplay2 fluid fluidlite kate tiger
#                                 daala bpg asdcplib openapv crystalhd jack mysofa
#   protobuf: only for the Chromecast renderer
#   aom: AV1 is decoded by dav1d in software, and by the TV in hardware
#   vpx: FFmpeg decodes VP8/VP9 in software, the TV decodes VP9 in hardware
#   fontconfig: libass runs without it; fonts come from the app's own fonts/ directory
DISABLE=(
    qt qtsvg sdl SDL_image caca ncurses glew sparkle breakpad growl
    xau xcb xcb-proto xorg-macros xproto wix wixlzx mingw64 d3d9 d3d11 amf vpl
    x262 x264 x265 lame twolame shout
    goom projectM chromaprint sidplay2 fluid fluidlite kate tiger
    daala bpg asdcplib openapv crystalhd jack mysofa
    protobuf aom fontconfig vpx
)

mkdir -p "$CONTRIB_BUILD"
cd "$CONTRIB_BUILD"
if [[ ! -f Makefile ]]; then
    args=(--host="$HOST" --prefix="$CONTRIB_PREFIX")
    for p in "${DISABLE[@]}"; do args+=("--disable-$p"); done
    "$VLC_SRC/contrib/bootstrap" "${args[@]}"
fi

echo "=== contribs to build ==="
make list
make -j"$JOBS" fetch
# Contrib builds are not all parallel-safe at the top level; each package builds with -j inside.
make -j"$JOBS" -k || make
# Not selected by default on Linux: SMB2/3 shares (VLC's smb2 module). libdsm (SMB1 and NetBIOS
# discovery) is left out: it does not build against glibc 2.12's headers (struct timespec defined
# twice), and SMB1 is off by default on current Windows and NAS systems.
make -j"$JOBS" .smb2
echo "=== contribs done: $CONTRIB_PREFIX ==="
