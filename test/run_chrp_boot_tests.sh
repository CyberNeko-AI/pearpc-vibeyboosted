#!/bin/sh
# No configured build is needed. Optional arguments: captured ofboot.b and expected boot path (default hd:2,\\yaboot).
set -eu
source_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-chrp-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$source_root/src" "$source_root/test/test_chrp_boot.cc" \
    "$source_root/src/io/prom/chrpboot.cc" -o "$out/test_chrp_boot"
"$out/test_chrp_boot" "$@"
