#!/bin/sh
# Tiger 8A428: trace the input-source icon URL path and libTIFF error entry.
# Pass --cpu-pvr=0x00088302 for a G3/no-AltiVec comparison; no config is edited.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
unset PEARPC_TRACE_RESERVATIONS PEARPC_TRACE_RESERVATIONS_RANGE
export PEARPC_TRACE_PC_FILE=auto
export PEARPC_TRACE_USER_ONLY=1
export PEARPC_TRACE_PCS=94a5c558,94a5c6c0,94a5c6d4,94a5c6ec,94a5c6fc,94a5c738,94a5c75c,94a5c790,94a5c7cc,91b482a4,91b48a0c,91b53f48,91b4d794,91b4e350,91b625c0,91b629c8,91b69ac8
export PEARPC_TRACE_SNAPSHOT_FILE=auto
export PEARPC_TRACE_SNAPSHOT_PC=91b53f48
export PEARPC_TRACE_SNAPSHOT_OCCURRENCE=1
exec "$ROOT/scripts/debug/run_with_crash_capture.sh" "$@"
