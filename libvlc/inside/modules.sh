#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Our own VLC modules (../../modules), built out of tree against the staged libVLC and installed
# into the runtime the app packages. Run after vlc.sh (which recreates the runtime).
source "$(dirname "$0")/env.sh"

RT="$BUILD/runtime"
INC="$STAGE/usr/include/vlc/plugins"
[[ -f "$INC/vlc_plugin.h" && -f "$RT/lib/libvlccore.so.9" ]] || { echo "error: run vlc.sh first"; exit 1; }

# The flags vlc-plugin.pc gives, plus our own.
CFLAGS_PLUGIN=(-std=gnu11 -O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter
               -I"$INC" -D__PLUGIN__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE)

build_module() {  # name source category [more sources and flags]
    local name="$1" src="$2" dir="$RT/lib/vlc/plugins/$3"
    shift 3
    local out="$dir/lib${name}_plugin.so"
    mkdir -p "$dir"
    "$HOST-gcc" "${CFLAGS_PLUGIN[@]}" -DMODULE_STRING="\"$name\"" -shared -o "$out" "$src" "$@" \
        "$RT/lib/libvlccore.so.9" -Wl,--as-needed -Wl,-soname,"lib${name}_plugin.so"
    patchelf --set-rpath '$ORIGIN/../..' "$out"
    "$HOST-strip" --strip-unneeded "$out"
    echo "built $out"
}

build_module wnk_bridge "$ROOT/modules/wnk_bridge/wnk_bridge.c" stream_out
# VLC's keystore list helpers (modules/keystore/list_util.c), compiled in.
build_module wnk_keystore "$ROOT/modules/wnk_keystore/wnk_keystore.c" keystore \
    "$VLC_SRC/modules/keystore/list_util.c" -I"$VLC_SRC/modules/keystore"
