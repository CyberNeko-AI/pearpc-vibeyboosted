#!/bin/sh
# Uses the normal configured objects and macOS dead stripping, like codegen tests.
set -eu
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Darwin ]; then
    echo "SKIP: this standalone controller test currently requires macOS linker dead stripping"
    exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-cd-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I. -Isrc -Wl,-dead_strip \
    test/test_cd_media.cc src/io/ide/cd.o src/io/ide/ata.o src/io/ide/idedevice.o \
    src/io/pci/libpci.a src/system/osapi/posix/sysfile.o \
    src/system/osapi/posix/systhread.o src/system/sys.o src/system/file.o \
    src/tools/libtools.a src/system/arch/generic/libsarch.a -lpthread -o "$out/test_cd_media"
"$out/test_cd_media"
