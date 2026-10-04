#!/bin/sh
# Run PearPC with timestamped logs and crash dumps.
# Usage: scripts/debug/run_with_crash_capture.sh [config-file] [ppc options...]

set -u

# Execute a memory snapshot of this script. A shell waiting for the emulator
# must not resume at an old byte offset in an in-place edited source file.
if [ "${PEARPC_CAPTURE_SCRIPT_SNAPSHOT:-}" != 1 ]; then
    export PEARPC_CAPTURE_SCRIPT_SNAPSHOT=1
    exec /bin/sh -c "$(cat "$0")" "$0" "$@"
fi
unset PEARPC_CAPTURE_SCRIPT_SNAPSHOT

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CONFIG=${1:-ppccfg.osx}
if [ "$#" -gt 0 ]; then
    shift
fi

STAMP=$(date '+%Y%m%d-%H%M%S')-$$
OUT=${PEARPC_CRASH_DIR:-"$ROOT/crash-captures"}/$STAMP
mkdir -p "$OUT" || exit 1

if [ -f "$CONFIG" ]; then
    cp "$CONFIG" "$OUT/config.input"
fi

LOG="$OUT/console.log"
printf 'PearPC crash capture: %s\n' "$OUT"
printf 'Logs: %s\n' "$LOG"

# The diagnostic launcher uses "auto" so every run has a distinct trace.
if [ "${PEARPC_TRACE_RESERVATIONS:-}" = auto ]; then
    PEARPC_TRACE_RESERVATIONS="$OUT/atomics.csv"
    export PEARPC_TRACE_RESERVATIONS
fi
if [ -n "${PEARPC_TRACE_RESERVATIONS:-}" ]; then
    printf 'Atomic trace: %s (PA range: %s)\n' "$PEARPC_TRACE_RESERVATIONS" "${PEARPC_TRACE_RESERVATIONS_RANGE:-all}"
    printf 'path=%s\nrange=%s\n' "$PEARPC_TRACE_RESERVATIONS" "${PEARPC_TRACE_RESERVATIONS_RANGE:-all}" > "$OUT/atomic-trace.settings"
fi

if [ "${PEARPC_TRACE_PC_FILE:-}" = auto ]; then
    PEARPC_TRACE_PC_FILE="$OUT/guest-pc.csv"
    export PEARPC_TRACE_PC_FILE
fi
if [ -n "${PEARPC_TRACE_PC_FILE:-}" ]; then
    printf 'Guest PC trace: %s\n' "$PEARPC_TRACE_PC_FILE"
    printf 'pcs=%s\n' "${PEARPC_TRACE_PCS:-}" > "$OUT/guest-pc.settings"
fi

if [ "${PEARPC_TRACE_SNAPSHOT_FILE:-}" = auto ]; then
    PEARPC_TRACE_SNAPSHOT_FILE="$OUT/probe-memory.bin"
    export PEARPC_TRACE_SNAPSHOT_FILE
fi
if [ -n "${PEARPC_TRACE_SNAPSHOT_FILE:-}" ]; then
    printf 'snapshot_pc=%s\nsnapshot_occurrence=%s\n' "${PEARPC_TRACE_SNAPSHOT_PC:-}" "${PEARPC_TRACE_SNAPSHOT_OCCURRENCE:-1}" >> "$OUT/guest-pc.settings"
fi

BINARY=${PEARPC_BINARY:-"$ROOT/src/ppc"}
if [ ! -x "$BINARY" ]; then
    printf 'PearPC executable not found: %s\n' "$BINARY" >&2
    exit 1
fi
printf 'Executable: %s\n' "$BINARY"
printf '%s\n' "$BINARY" > "$OUT/executable.txt"
PEARPC_GENERIC_TRACE_FILE="$OUT/generic-dispatch.log"
export PEARPC_GENERIC_TRACE_FILE
"$BINARY" \
    --memdump-file="$OUT/guest-memory.bin" \
    --framebuffer-dump-file="$OUT/framebuffer.bin" \
    "$CONFIG" "$@" >"$LOG" 2>&1
STATUS=$?

printf '\nPearPC exited with status %d\n' "$STATUS" >>"$LOG"
printf 'PearPC exited with status %d; capture saved in %s\n' "$STATUS" "$OUT"
exit "$STATUS"
