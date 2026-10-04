#!/bin/bash
# Exercise actual backend handlers against a byte-wise reference, with UBSan.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-vector-shifts.XXXXXX")
trap 'rm -rf "$out"' EXIT
link_flags=(-Wl,--gc-sections)
if [[ $(uname -s) == Darwin ]]; then link_flags=(-Wl,-dead_strip); fi
flags=(-std=c++11 -O3 -DHAVE_CONFIG_H -I. -Isrc -ffunction-sections -fdata-sections
       -fsanitize=undefined -fno-sanitize-recover=undefined)
for backend in cpu_jitc_aarch64 cpu_generic; do
    defines=(-DTEST_AARCH64)
    if [[ $backend == cpu_generic ]]; then defines=(-DTEST_GENERIC); fi
    "${CXX:-c++}" "${flags[@]}" -c "src/cpu/$backend/ppc_vec.cc" -o "$out/$backend.o"
    "${CXX:-c++}" "${flags[@]}" "${defines[@]}" "${link_flags[@]}" \
        test/test_vector_shifts.cc "$out/$backend.o" -o "$out/test-$backend"
    printf '%s: ' "$backend"
    "$out/test-$backend"
done
