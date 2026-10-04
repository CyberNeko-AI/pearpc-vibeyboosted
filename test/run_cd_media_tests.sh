#!/bin/sh
# Uses the normal configured objects and macOS dead stripping, like codegen tests.
set -eu
# Optional first argument: configured build directory (default: source tree).
source_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build_root=$(CDPATH= cd "${1:-$source_root}" && pwd)
if [ "$(uname -s)" != Darwin ]; then
    echo "SKIP: this standalone controller test currently requires macOS linker dead stripping"
    exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-cd-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I"$build_root" -I"$build_root/src" -I"$source_root/src" -Wl,-dead_strip \
    "$source_root/test/test_cd_media.cc" "$build_root/src/io/ide/cd.o" \
    "$build_root/src/io/ide/ata.o" "$build_root/src/io/ide/idedevice.o" \
    "$build_root/src/io/pci/libpci.a" "$build_root/src/system/osapi/posix/sysfile.o" \
    "$build_root/src/system/osapi/posix/systhread.o" "$build_root/src/system/sys.o" "$build_root/src/system/file.o" \
    "$build_root/src/tools/libtools.a" "$build_root/src/system/arch/generic/libsarch.a" -lpthread -o "$out/test_cd_media"
"$out/test_cd_media"
