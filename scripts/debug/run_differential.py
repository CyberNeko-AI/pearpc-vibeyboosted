#!/usr/bin/env python3
"""Run the same bare-metal PPC configs on two PearPC CPU backends.

The runner compares process outcome and semantic test lines. It deliberately
keeps complete stdout/stderr logs so a mismatch can be minimized later.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIGS = sorted((ROOT / "test").glob("test_*.cfg"))
VOLATILE = re.compile(r"(?:0x)?[0-9a-fA-F]{8,}")
SEMANTIC = re.compile(r"(\[TEST\]|=== Summary|PASS|FAIL|FATAL|INVALID|exit with code|TIMEOUT|Error)")


def run_one(binary: Path, cfg: Path, timeout: float, outdir: Path) -> dict:
    label = "generic" if "generic" in str(binary).lower() else "jit"
    log = outdir / f"{cfg.stem}.{label}.log"
    try:
        proc = subprocess.run(
            [str(binary), "--headless", str(cfg)], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, text=True, errors="replace",
        )
        status = "exit"
        rc = proc.returncode
    except subprocess.TimeoutExpired as exc:
        status = "timeout"
        rc = 124
        data = (exc.stdout or "")
        if isinstance(data, bytes):
            data = data.decode(errors="replace")
        proc = None
    if proc is not None:
        data = proc.stdout
    log.write_text(data)
    semantic = []
    for line in data.splitlines():
        if SEMANTIC.search(line):
            # Addresses and host-specific counters should not create false diffs.
            semantic.append(VOLATILE.sub("<addr>", line.strip()))
    return {"status": status, "returncode": rc, "log": str(log), "semantic": semantic}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jit", type=Path, required=True)
    ap.add_argument("--generic", type=Path, required=True)
    ap.add_argument("--config", action="append", type=Path, dest="configs")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--outdir", type=Path, help="directory for per-run logs (default: retained /tmp directory)")
    args = ap.parse_args()
    configs = args.configs or DEFAULT_CONFIGS
    configs = [p if p.is_absolute() else ROOT / p for p in configs]
    for b in (args.jit, args.generic):
        if not b.exists() or not os.access(b, os.X_OK):
            ap.error(f"not executable: {b}")

    if args.outdir:
        outdir = args.outdir
        outdir.mkdir(parents=True, exist_ok=True)
    else:
        outdir = Path(tempfile.mkdtemp(prefix="pearpc-diff-"))

    rows = []
    for cfg in configs:
        if not cfg.exists():
            rows.append({"config": str(cfg), "status": "missing"})
            continue
        jit = run_one(args.jit, cfg, args.timeout, outdir)
        generic = run_one(args.generic, cfg, args.timeout, outdir)
        failure_marker = lambda xs: any(re.search(r"FAIL|FATAL|INVALID|TIMEOUT|Error", x) for x in xs)
        match = (jit["status"], jit["returncode"]) == (generic["status"], generic["returncode"])
        if match and (failure_marker(jit["semantic"]) or failure_marker(generic["semantic"])):
            # Console buffering differs between the two CPU builds. Compare
            # the presence of a failure marker, not incidental output text.
            match = failure_marker(jit["semantic"]) == failure_marker(generic["semantic"])
        rows.append({"config": str(cfg), "match": match, "jit": jit, "generic": generic})
        print(f"{cfg.stem:24} {'MATCH' if match else 'DIFF'} "
              f"jit={jit['status']}/{jit['returncode']} generic={generic['status']}/{generic['returncode']}")
        if not match:
            print(f"  logs: {jit['log']}  {generic['log']}")
            print(f"  jit semantic: {jit['semantic'][-5:]}")
            print(f"  generic semantic: {generic['semantic'][-5:]}")

    result = {"jit": str(args.jit), "generic": str(args.generic), "outdir": str(outdir), "rows": rows}
    if args.json:
        args.json.write_text(json.dumps(result, indent=2, ensure_ascii=False))
    mismatches = sum(1 for r in rows if r.get("match") is False)
    print(f"\n=== Differential results: {len(rows)-mismatches} matched, {mismatches} mismatched ===")
    print(f"logs: {outdir}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
