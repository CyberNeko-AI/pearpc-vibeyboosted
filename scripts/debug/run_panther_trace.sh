#!/bin/sh
# Trace mapBumpBusy/mapDropBusy in the Panther 7.0 kernel examined in
# doc/OSX103_INSTALL_HANG_20261003.md. Other kernels need a different PA range.
# Usage: scripts/debug/run_panther_trace.sh [config-file] [ppc options...]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export PEARPC_TRACE_RESERVATIONS=auto
export PEARPC_TRACE_RESERVATIONS_RANGE=${PEARPC_TRACE_RESERVATIONS_RANGE:-8aaa0:8aad0}
exec "$ROOT/scripts/debug/run_with_crash_capture.sh" "$@"
