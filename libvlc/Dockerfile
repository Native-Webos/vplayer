# SPDX-License-Identifier: GPL-3.0-or-later
# Build environment for cross-compiling VLC 3.0.x (contribs + libvlc + plugins) for LG webOS.
# The webosbrew NDK is NOT baked in: libvlc/build.sh bind-mounts it at the same absolute path it
# was relocated to, because a buildroot SDK only works from the path relocate-sdk.sh gave it.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential autoconf automake autopoint libtool libtool-bin gettext pkg-config \
        cmake meson ninja-build bison flex gperf nasm yasm ragel help2man texinfo \
        python3 python3-venv python3-setuptools git wget curl ca-certificates \
        xz-utils bzip2 lzip unzip zip file patch rsync \
        lua5.2 liblua5.2-dev \
        qemu-user-static patchelf \
    && rm -rf /var/lib/apt/lists/*

# Contrib and configure scripts write caches under $HOME.
ENV HOME=/tmp
