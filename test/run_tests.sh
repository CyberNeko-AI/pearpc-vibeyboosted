#!/bin/bash
# Run all PearPC headless test ELFs and report results.
# Usage: test/run_tests.sh [path/to/ppc] [timeout_seconds]
#
# Each test is run with a timeout (default 30s). If a test hangs
# (e.g. infinite loop in JIT), it is killed and reported as TIMEOUT.

set -euo pipefail

PPC="${1:-./src/ppc}"
TIMEOUT="${2:-30}"
SOURCE_ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)

if [ ! -x "$PPC" ]; then
    echo "ERROR: $PPC not found or not executable (build first)" >&2
    exit 1
fi

# Resolve the selected executable before entering an isolated runtime directory.
# Configs use relative test/*.elf, video.x and NVRAM paths. Keep their writes
# (and interpreter traces) out of both the source and build trees.
PPC="$(CDPATH= cd "$(dirname "$PPC")" && pwd)/$(basename "$PPC")"
RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-headless-tests.XXXXXX")
trap 'rm -rf "$RUN_DIR"' EXIT
mkdir "$RUN_DIR/test"
ln -s "$SOURCE_ROOT/video.x" "$RUN_DIR/video.x"
cd "$RUN_DIR"

TESTS=(
    test/test_loop.cfg
    test/test_alu.cfg
    test/test_mem.cfg
    test/test_bat_tlb.cfg
    test/test_bat_code.cfg
    test/test_dsi.cfg
    test/test_multiple_dsi.cfg
    test/test_branch_loop.cfg
    test/test_fpu_exc.cfg
    test/test_fpu_arith.cfg
    test/test_altivec.cfg
    test/test_vec_shift_zero.cfg
    test/test_crlogical.cfg
    test/test_defflags.cfg
    test/test_mid_block.cfg
    test/test_bench.cfg
)

passed=0
failed=0
skipped=0
failures=()

run_with_timeout() {
    local t="$1"
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$t" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then
        gtimeout "$t" "$@"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c '
import subprocess, sys
try:
    res = subprocess.run(sys.argv[2:], timeout=float(sys.argv[1]))
    sys.exit(res.returncode)
except subprocess.TimeoutExpired:
    sys.exit(124)
' "$t" "$@"
    else
        "$@"
    fi
}

for cfg in "${TESTS[@]}"; do
    name="${cfg##*/}"
    name="${name%.cfg}"
    elf="${cfg%.cfg}.elf"

    if [ ! -f "$SOURCE_ROOT/$elf" ]; then
        printf "%-24s SKIP (no .elf)\n" "$name"
        skipped=$((skipped + 1))
        continue
    fi

    ln -s "$SOURCE_ROOT/$elf" "$elf"
    if output=$(run_with_timeout "$TIMEOUT" "$PPC" --headless "$SOURCE_ROOT/$cfg" 2>&1); then
        printf "%-24s PASS\n" "$name"
        passed=$((passed + 1))
    else
        rc=$?
        if [ $rc -eq 124 ]; then
            printf "%-24s TIMEOUT (${TIMEOUT}s)\n" "$name"
        else
            printf "%-24s FAIL (exit $rc)\n" "$name"
            echo "$output" | tail -5 | sed 's/^/  /'
        fi
        failures+=("$name")
        failed=$((failed + 1))
    fi
done

echo ""
echo "=== Results: $passed passed, $failed failed, $skipped skipped ==="

if [ ${#failures[@]} -gt 0 ]; then
    echo "Failed: ${failures[*]}"
    exit 1
fi
