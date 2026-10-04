#!/bin/sh
# Exercise the actual CUDA register state machine with a Linux-style receiver.
set -eu
source_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build_root=$(CDPATH= cd "${1:-$source_root}" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-cuda-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT
if [ "$(uname -s)" = Darwin ]; then
    strip_flag=-Wl,-dead_strip
else
    strip_flag=-Wl,--gc-sections
fi
"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I"$build_root" -I"$source_root/src" \
    -ffunction-sections -fdata-sections "$strip_flag" \
    "$source_root/test/test_cuda_keyboard.cc" \
    "$build_root/src/system/osapi/posix/systhread.o" \
    "$build_root/src/system/osapi/posix/systimer.o" \
    "$build_root/src/tools/libtools.a" -lpthread -o "$out/test_cuda_keyboard"
"$out/test_cuda_keyboard"
