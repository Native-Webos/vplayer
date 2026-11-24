#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build libvlc 3.0.x for LG webOS (armv7, softfp, glibc 2.12) inside Docker.
#
#   libvlc/build.sh image        build the Docker image (once)
#   libvlc/build.sh contribs     fetch + build VLC's third-party libraries (static, -fPIC)
#   libvlc/build.sh vlc          configure + build libvlc, libvlccore and the plugins
#   libvlc/build.sh all          image + contribs + vlc
#   libvlc/build.sh shell        a shell in the build container
#
# Output: build/libvlc/stage/  (lib/libvlc.so*, lib/libvlccore.so*, lib/vlc/plugins/**, include/vlc)
# Logs:   build/libvlc/logs/
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VLC_VERSION="${VLC_VERSION:-3.0.24}"
IMAGE="vplayer-libvlc-build:1"
NDK="${WEBOS_NDK:-$HOME/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot}"
[[ -x "$NDK/bin/arm-webos-linux-gnueabi-gcc" ]] || { echo "error: webOS NDK not found at $NDK (set WEBOS_NDK)" >&2; exit 1; }
NDK="$(cd "$NDK" && pwd -P)"

VLC_SRC="$ROOT/third_party/vlc-$VLC_VERSION"
if [[ ! -d "$VLC_SRC" ]]; then
    git clone --depth 1 --branch "$VLC_VERSION" https://code.videolan.org/videolan/vlc.git "$VLC_SRC"
fi
mkdir -p "$ROOT/build/libvlc/logs"

# Our changes to VLC live as patches in libvlc/patches/vlc, applied once to the checkout.
for p in "$ROOT"/libvlc/patches/vlc/*.patch; do
    [[ -e "$p" ]] || continue
    if git -C "$VLC_SRC" apply --check "$p" 2>/dev/null; then
        git -C "$VLC_SRC" apply "$p" && echo "applied $(basename "$p")"
    elif ! git -C "$VLC_SRC" apply --reverse --check "$p" 2>/dev/null; then
        echo "error: $(basename "$p") neither applies nor is already applied" >&2; exit 1
    fi
done

run() {
    docker run --rm ${DOCKER_TTY:-} \
        --user "$(id -u):$(id -g)" \
        -v "$ROOT:$ROOT" -v "$NDK:$NDK:ro" \
        -e NDK="$NDK" -e ROOT="$ROOT" -e VLC_SRC="$VLC_SRC" -e JOBS="${JOBS:-$(nproc)}" \
        -w "$ROOT" "$IMAGE" "$@"
}

case "${1:-all}" in
    image)    docker build -t "$IMAGE" "$ROOT/libvlc" ;;
    contribs) run bash libvlc/inside/contribs.sh 2>&1 | tee "$ROOT/build/libvlc/logs/contribs.log" ;;
    vlc)      run bash libvlc/inside/vlc.sh 2>&1 | tee "$ROOT/build/libvlc/logs/vlc.log" ;;
    all)      "$0" image && "$0" contribs && "$0" vlc ;;
    shell)    DOCKER_TTY=-it run bash ;;
    *)        echo "usage: $0 [image|contribs|vlc|all|shell]" >&2; exit 2 ;;
esac
