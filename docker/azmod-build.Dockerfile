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
RUN apt-get update -qq && apt-get install -y -qq gcc g++ libc6-dev make \
    libx11-dev libxext-dev libfreetype6-dev libasound2-dev pkg-config
