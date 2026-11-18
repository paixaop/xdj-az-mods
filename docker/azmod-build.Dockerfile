# SPDX-License-Identifier: GPL-3.0-only
FROM ubuntu:18.04
# Toolchain for the azmod + azstem release build (make azmod azstem).
#
# Ubuntu 18.04 ships glibc 2.27. What matters for the deck is the highest GLIBC_
# symbol version the built .so files require; measure it on the artifact:
#   objdump -T azmod.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
#
# Images built from this RUN line on different machines produce byte-identical
# azmod.so and azstem.so from the same source.
#
#   docker build --platform=linux/arm64 -f docker/azmod-build.Dockerfile -t azmod-build:18.04 .
RUN apt-get update -qq && apt-get install -y -qq gcc libstdc++-7-dev libc6-dev make \
    libx11-dev libxext-dev libxtst-dev libfreetype6-dev libasound2-dev pkg-config
# The distro's static XTest archive is not PIC. Build the matching authenticated
# Ubuntu source as PIC so the core can include it without a deck-side dependency.
RUN sed -n 's/^deb /deb-src /p' /etc/apt/sources.list > /etc/apt/sources.list.d/source.list \
    && apt-get update -qq && apt-get install -y -qq dpkg-dev \
    && mkdir /tmp/xtst-build && cd /tmp/xtst-build && apt-get source libxtst \
    && cd libxtst-* && ./configure --prefix=/usr --libdir=/usr/lib/aarch64-linux-gnu \
        --disable-shared --enable-static --with-pic CFLAGS='-O2 -fPIC' \
    && make -j2 && make install && rm -rf /tmp/xtst-build
