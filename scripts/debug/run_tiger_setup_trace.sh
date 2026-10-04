#!/bin/sh
# Effective PCs from Tiger 10.4 build 8A428's SetupAssistantSupport framework.
# No installer/guest files or CPU results are changed by these probes.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
unset PEARPC_TRACE_RESERVATIONS PEARPC_TRACE_RESERVATIONS_RANGE
export PEARPC_TRACE_PC_FILE=auto
# Phase 2 follows both the keyboard-info conversion and the table row count.
# IntroSection was relocated to 0x000f4000 in the captured 8A428 session;
# shared framework probes remain useful if that bundle moves on another run.
export PEARPC_TRACE_PCS=94a5c410,94a5c414,94a5c4d0,94a5c558,94a5c580,94a5c5a0,94a5c5d0,94a5c60c,94a5c650,94a5c6c0,94a5c790,94a5c7a4,94a5c7b4,94a5c7cc,94a49af0,94a49b30,000f4de4,000f4df8,000f4f30,000f4f34,000f4f38,000f5410,000f53dc,000f53f4,000f53fc,000f5404,000f545c
export PEARPC_TRACE_SNAPSHOT_FILE=auto
export PEARPC_TRACE_SNAPSHOT_PC=94a5c7cc
export PEARPC_TRACE_SNAPSHOT_OCCURRENCE=2
exec "$ROOT/scripts/debug/run_with_crash_capture.sh" "$@"
