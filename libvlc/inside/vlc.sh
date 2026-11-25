#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Configure and build libvlc + libvlccore + plugins for webOS against the contribs, then stage them.
source "$(dirname "$0")/env.sh"

[[ -d "$CONTRIB_PREFIX/lib" ]] || { echo "error: contribs not built ($CONTRIB_PREFIX)"; exit 1; }

# configure is generated from configure.ac (the git tree has none).
if [[ ! -x "$VLC_SRC/configure" ]]; then
    (cd "$VLC_SRC" && ./bootstrap)
fi

# Libraries VLC may take from the TV's own sysroot. Each one is needed only by the plugin that uses
# it (aout_pulse, alsa, gles2/egl helpers), so a firmware without it costs that one plugin, never
# libvlc. They are given to configure explicitly instead of through pkg-config, which is pointed at
# the contribs only (env.sh).
export PULSE_CFLAGS="-I$SYSROOT/usr/include"  PULSE_LIBS="-L$SYSROOT/usr/lib -lpulse"
export ALSA_CFLAGS="-I$SYSROOT/usr/include"   ALSA_LIBS="-L$SYSROOT/usr/lib -lasound"
export EGL_CFLAGS="-I$SYSROOT/usr/include"    EGL_LIBS="-L$SYSROOT/usr/lib -lEGL"
export GLES2_CFLAGS="-I$SYSROOT/usr/include"  GLES2_LIBS="-L$SYSROOT/usr/lib -lGLESv2"

# The same for C plugins that link a C++ contrib (srt, gme, modplug): their .pc files ask for
# -lstdc++, so name the static archives there instead.
STDCXX_A="$("$HOST-g++" -print-file-name=libstdc++.a)"
GCC_EH_A="$("$HOST-g++" -print-file-name=libgcc_eh.a)"
[[ -f "$STDCXX_A" && -f "$GCC_EH_A" ]] || { echo "error: static libstdc++/libgcc_eh not found"; exit 1; }
sed -i -E "s#(^|[[:space:]])-lstdc\+\+([[:space:]]|$)#\1$STDCXX_A $GCC_EH_A\2#g; s#(^|[[:space:]])-lgcc_s([[:space:]]|$)#\1-lgcc\2#g" "$CONTRIB_PREFIX"/lib/pkgconfig/*.pc

# Lua scripts are precompiled at build time, so luac must run HERE: the contribs build a host luac
# patched to emit 32-bit bytecode for the target ($BUILD/bin), next to the ARM one in the prefix.
export LUAC="$BUILD/bin/$HOST-luac"
[[ -x "$LUAC" ]] || { echo "error: host luac missing ($LUAC)"; exit 1; }

# Cross links do not follow libvlc.so -> libvlccore.so.9 on their own (bin/vlc, vlc-cache-gen).
export LDFLAGS="$LDFLAGS -Wl,-rpath-link,$VLC_BUILD/src/.libs"
# The TV's libstdc++ is older than GCC 12's (GLIBCXX 3.4.29+ is absent before webOS 11), and
# libatomic.so.1 is missing on webOS 4.x: link the C++ runtime statically, drop unused libraries.
export LDFLAGS="$LDFLAGS -static-libstdc++ -static-libgcc -Wl,--as-needed"

mkdir -p "$VLC_BUILD"
cd "$VLC_BUILD"
CONFIGURE_ARGS=(
    --host="$HOST"
    --prefix=/usr
    --with-contrib="$CONTRIB_PREFIX"
    --enable-neon
    --enable-shared --disable-static
    --disable-nls
    --disable-qt --disable-skins2 --disable-ncurses
    --disable-xcb --disable-xvideo --disable-wayland --disable-vdpau --disable-libva
    --disable-dbus --disable-udev --disable-avahi --disable-lirc --disable-kwallet
    --disable-update-check --disable-sparkle --disable-macosx
    --disable-jack --disable-oss --disable-sndio
    --disable-v4l2 --disable-decklink --disable-dc1394 --disable-dv1394 --disable-linsys
    --disable-vcd --disable-libcddb
    --disable-chromecast --disable-goom --disable-projectm --disable-vsxu
    --disable-sdl-image --disable-fontconfig --disable-notify --disable-secret
    --disable-mtp --disable-gst-decode --disable-omxil --disable-mmal
    --disable-crystalhd --disable-libplacebo
    --enable-gles2 --enable-pulse --enable-alsa
    --enable-lua --enable-sout --enable-vlm
    --enable-vlc
    --enable-smb2 --disable-dsm
    --enable-merge-ffmpeg
    vlc_cv_replace_decl_ffsll=yes
)
# ^ smb2: SMB2/3 shares (libsmb2). dsm (libdsm, SMB1) is not built: see contribs.sh.
#   merge-ffmpeg: one avcodec plugin for decoding, demuxing and parsing, instead of FFmpeg linked
#   into three plugins (about 25 MB of the runtime).
#   vlc_cv_replace_decl_ffsll: glibc 2.12 declares ffsll in <string.h> only, and VLC looks in
#   <strings.h>; the replacement prototype it would add clashes with glibc's noexcept one in every
#   C++ module.
# Configure again whenever the arguments change (the build is otherwise incremental).
# Configure again when its arguments change, or the contribs' pkg-config files (a library rebuilt
# with other dependencies: modules' link flags are taken at configure time).
CONFIGURE_KEY="${CONFIGURE_ARGS[*]} $(cat "$CONTRIB_PREFIX"/lib/pkgconfig/*.pc 2>/dev/null | sha256sum | cut -c1-16)"
if [[ ! -f Makefile || "$(cat .configure-args 2>/dev/null)" != "$CONFIGURE_KEY" ]]; then
    "$VLC_SRC/configure" "${CONFIGURE_ARGS[@]}"
    echo "$CONFIGURE_KEY" > .configure-args
fi

# libtool links C++ shared objects with -nostdlib and its own runtime list, which silently defeats
# -static-libstdc++. Rewrite that list: libstdc++ and the unwinder static, the rest as before
# (glibc_polyfills is the NDK's shim for symbols newer than glibc 2.12).
sed -i -E "s|^postdeps=\"-lstdc\\+\\+ -lm -lgcc_s (.*)\"\$|postdeps=\"$STDCXX_A $GCC_EH_A -lm -lgcc \\1\"|; s|(^postdeps=\".*)-lgcc_s(.*\")|\\1-lgcc\\2|" libtool
grep -q "^postdeps=\"$STDCXX_A" libtool || { echo "error: libtool postdeps not rewritten"; exit 1; }

make -j"$JOBS" LUAC="$LUAC"
rm -rf "$STAGE"
make install DESTDIR="$STAGE"
# The SDK's headers and .la files are for building against libvlc on the desk, not for the TV.
find "$STAGE" -name '*.la' -delete
# RPATHs: libtool wrote /usr/lib/vlc and the NDK's own lib dir. On the TV the tree lives inside the
# app's folder, so every library finds its neighbours relative to itself ($ORIGIN): plugins look in
# lib/vlc (libvlc_pulse.so.0), libvlc.so.5 in lib/ (libvlccore.so.9).
L="$STAGE/usr/lib"
find "$L" -name '*.so*' -type f | while read -r f; do
    case "$f" in
        "$L"/vlc/plugins/*) patchelf --set-rpath '$ORIGIN/../..' "$f" ;;
        "$L"/vlc/*)         patchelf --set-rpath '$ORIGIN' "$f" ;;
        # libvlc.so.5 needs libvlccore.so.9 from the same directory. The app's executable carries a
        # RUNPATH, which (unlike RPATH) does not apply to its libraries' own dependencies.
        *)                  patchelf --set-rpath '$ORIGIN' "$f" ;;
    esac
done
# libatomic.so.1 is absent on webOS 4.x. FFmpeg's .pc pulls -latomic in, but ARMv7 does its
# atomics inline: drop the entry from every file that imports no __atomic_* symbol, fail otherwise.
find "$L" -name '*.so*' -type f | while read -r f; do
    readelf -d "$f" | grep -q 'NEEDED.*libatomic.so.1' || continue
    if readelf --dyn-syms "$f" | grep -E ' UND ' | grep -q '__atomic_'; then
        echo "error: $f really uses libatomic"; exit 1
    fi
    patchelf --remove-needed libatomic.so.1 "$f"
done
echo "=== staged: $STAGE ==="
du -sh "$STAGE/usr/lib" "$STAGE/usr/lib/vlc/plugins" 2>/dev/null || true
find "$STAGE/usr/lib/vlc/plugins" -name '*.so' | wc -l | xargs echo "plugins:"

# The runtime the app ships, laid out as it sits in the ipk next to the executable:
#   lib/libvlc.so.5, lib/libvlccore.so.9         (bundled libraries, under their SONAMEs)
#   lib/vlc/plugins/**, lib/vlc/lua/**, lib/vlc/libvlc_pulse.so.0
#   share/vlc/lua/**                              (VLC's data dir: lib/vlc with /lib/ -> /share/)
# Everything stripped. Desk-only files (headers, .pc, vlc-cache-gen, libcompat.a) stay out.
RT="$BUILD/runtime"
rm -rf "$RT"
mkdir -p "$RT/lib/vlc" "$RT/share/vlc"
cp -L "$L/libvlc.so.5" "$L/libvlccore.so.9" "$RT/lib/"
cp -L "$L/vlc/libvlc_pulse.so.0" "$RT/lib/vlc/"
cp -a "$L/vlc/plugins" "$L/vlc/lua" "$RT/lib/vlc/"
cp -a "$STAGE/usr/share/vlc/lua" "$RT/share/vlc/"
find "$RT" -name '*.so*' -type f -exec "$HOST-strip" --strip-unneeded {} +
# Desk tools for trying libvlc on the TV over ssh (not packaged): the vlc CLI and the cache generator.
mkdir -p "$BUILD/devtools"
cp -L "$STAGE/usr/bin/vlc" "$L/vlc/vlc-cache-gen" "$BUILD/devtools/"
patchelf --set-rpath '$ORIGIN/../lib' "$BUILD/devtools/vlc"
patchelf --set-rpath '$ORIGIN/../lib' "$BUILD/devtools/vlc-cache-gen"
"$HOST-strip" "$BUILD/devtools/vlc" "$BUILD/devtools/vlc-cache-gen"
echo "=== runtime: $RT ==="
du -sh "$RT"
