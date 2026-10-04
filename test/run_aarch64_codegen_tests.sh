#!/bin/bash
# Run after a normal macOS AArch64 JIT build, from any working directory.
set -euo pipefail

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo "SKIP: native codegen tests require macOS arm64"
    exit 0
fi

# Optional first argument: configured build directory (default: source tree).
source_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build_root=$(CDPATH= cd "${1:-$source_root}" && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-codegen.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
cpu_dir="$build_root/src/cpu/cpu_jitc_aarch64"

# Dead stripping keeps the actual emitter and register-flush implementation
# while excluding unused emulator entry points and device dependencies.
"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I"$build_root" -I"$build_root/src" -I"$source_root/src" -Wl,-dead_strip \
    "$source_root/test/test_stwcx_fragments.cc" "$cpu_dir/jitc.o" "$cpu_dir/ppc_mmu.o" \
    "$cpu_dir/ppc_alu.o" "$cpu_dir/aarch64asm.o" -o "$test_dir/test_stwcx_fragments"
"$test_dir/test_stwcx_fragments"

"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I"$build_root" -I"$build_root/src" -I"$source_root/src" -Wl,-dead_strip \
    "$source_root/test/test_branch_state.cc" "$cpu_dir/jitc.o" "$cpu_dir/ppc_alu.o" \
    "$cpu_dir/aarch64asm.o" -o "$test_dir/test_branch_state"
"$test_dir/test_branch_state"

"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I"$build_root" -I"$build_root/src" -I"$source_root/src" -Wl,-dead_strip \
    "$source_root/test/test_interpret_exception.cc" "$cpu_dir/jitc.o" "$cpu_dir/ppc_exc.o" \
    "$cpu_dir/ppc_alu.o" "$build_root/src/tools/libtools.a" "$cpu_dir/aarch64asm.o" -o "$test_dir/test_interpret_exception"
"$test_dir/test_interpret_exception"
