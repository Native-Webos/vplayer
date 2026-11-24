# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced inside the build container. Cross environment for arm-webos-linux-gnueabi.
set -euo pipefail
export HOST=arm-webos-linux-gnueabi
# The NDK's bin/ also carries buildroot HOST tools (perl, python, pkg-config, cmake, meson, …)
# that are broken outside buildroot (perl without FindBin; a pkg-config that prefixes the sysroot).
# Append it, so the container's tools win and only the arm-webos-linux-gnueabi-* names come from it.
export PATH="$PATH:$NDK/bin"
export SYSROOT="$NDK/$HOST/sysroot"

BUILD="$ROOT/build/libvlc"
export CONTRIB_BUILD="$BUILD/contrib-build"      # where contribs are unpacked and compiled
export CONTRIB_PREFIX="$BUILD/contrib/$HOST"     # their static libraries and headers
export VLC_BUILD="$BUILD/vlc-build"
export STAGE="$BUILD/stage"

# Same code generation as the app kit: armv7-a, NEON, softfp (the TV's userland ABI).
export CFLAGS="-O2 -g -fPIC -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""

# Never let a contrib or configure find the sysroot's copy of a library through pkg-config by
# accident: firmware libraries move SONAMEs between webOS releases (see the kit's
# docs/research/PACKAGING-AND-FIRMWARE.md). The sysroot .pc files that ARE allowed are listed in
# vlc.sh, one by one.
export PKG_CONFIG_LIBDIR="$CONTRIB_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=""
export PKG_CONFIG_SYSROOT_DIR=""
