#!/bin/sh
# FBInk as a static library for the Kindle: refresh control and raw pixel writes (MINIMAL with
# IMAGE, for fbink_print_raw_data: the Colorsoft's framebuffer color path), no fonts.
# Run in the zk-kindlehf container; installs libfbink.a + fbink.h into build-deps/kindle/prefix.
set -eu
cd "$(dirname "$0")/.."
PREFIX=$PWD/build-deps/kindle/prefix
mkdir -p "$PREFIX/lib" "$PREFIX/include"
make -C third_party/FBInk clean >/dev/null 2>&1 || true
make -C third_party/FBInk staticlib KINDLE=1 MINIMAL=1 IMAGE=1 CROSS_TC=arm-kindlehf-linux-gnueabihf -j"$(nproc)" >/dev/null
cp third_party/FBInk/Release/libfbink.a "$PREFIX/lib/"
cp third_party/FBInk/fbink.h "$PREFIX/include/"
make -C third_party/FBInk clean >/dev/null 2>&1 || true
echo "fbink ready in $PREFIX"
