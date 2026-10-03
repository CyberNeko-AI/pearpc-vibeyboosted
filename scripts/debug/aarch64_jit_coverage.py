#!/usr/bin/env python3
"""Inventory PPC opcode handlers and AArch64 JIT implementation mode.

This is a static audit, not a correctness proof. It reports handlers present in
both the generic interpreter and AArch64 decoder, and classifies the AArch64
code generator as native, interpreter fallback, or missing.
"""
from pathlib import Path
import re
import argparse
import json

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "src/cpu/cpu_generic"
JIT = ROOT / "src/cpu/cpu_jitc_aarch64"


def funcs(paths, pattern):
    out = set()
    for p in paths:
        text = p.read_text(errors="replace")
        out.update(re.findall(pattern, text))
    return out


def function_body(text, name):
    m = re.search(r"(?:int|void|JITCFlow)\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", text)
    if not m:
        return ""
    start = m.end() - 1
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return text[start:]


generic = funcs(GEN.glob("*.cc"), r"\b(?:void|int)\s+(ppc_opc_[A-Za-z0-9_]+)\s*\(")
jit_impl = funcs(JIT.glob("*.cc"), r"\b(?:int|void)\s+(ppc_opc_[A-Za-z0-9_]+)\s*\(")
gen_names = funcs(JIT.glob("*.cc"), r"\b(?:JITCFlow)\s+(ppc_opc_gen_[A-Za-z0-9_]+)\s*\(")

dec = (JIT / "ppc_dec.cc").read_text(errors="replace")
# Only handlers explicitly installed in the decoder tables are relevant.
installed = set(re.findall(r"ppc_opc_table(?:_gen)?_[A-Za-z0-9]+\[[^]]+\]\s*=\s*ppc_opc_(?!gen_)([A-Za-z0-9_]+)", dec))
# Include primary/group dispatch handlers and special paths.
installed.update(re.findall(r"return\s+ppc_opc_gen_([A-Za-z0-9_]+)\(", dec))
for macro in ("GEN_INTERPRET", "GEN_INTERPRET_BRANCH", "GEN_INTERPRET_LOADSTORE", "GEN_INTERPRET_ENDBLOCK"):
    installed.update(x for x in re.findall(r"\b" + macro + r"\(([^)]+)\)", dec) if x != "name")

classes = {"native": [], "interpreter": [], "missing": []}
for name in sorted(installed):
    body = ""
    for p in JIT.glob("*.cc"):
        body = function_body(p.read_text(errors="replace"), "ppc_opc_gen_" + name)
        if body:
            break
    macro = re.search(r"\b(?:GEN_INTERPRET|GEN_INTERPRET_BRANCH|GEN_INTERPRET_LOADSTORE|GEN_INTERPRET_ENDBLOCK)\(" + re.escape(name) + r"\)", dec)
    if not body and macro:
        classes["interpreter"].append(name)
    elif not body:
        classes["missing"].append(name)
    elif "ppc_opc_gen_interpret" in body:
        classes["interpreter"].append(name)
    else:
        classes["native"].append(name)

ap = argparse.ArgumentParser()
ap.add_argument("--json", type=Path)
args = ap.parse_args()

print(f"repository: {ROOT}")
print(f"decoder-installed handlers: {len(installed)}")
print(f"native generators: {len(classes['native'])}")
print(f"interpreter fallback generators: {len(classes['interpreter'])}")
print(f"missing generator definitions: {len(classes['missing'])}")
for key in ("missing", "interpreter", "native"):
    print(f"\n[{key}]")
    print(" ".join(classes[key]))

# Generic-only operations are useful review targets, but some are aliases or
# helper routines and are intentionally not decoder-visible.
generic_names = {x.removeprefix("ppc_opc_") for x in generic}
installed_names = set(installed)
print("\n[generic handlers not directly installed in AArch64 decoder]")
generic_only = sorted(generic_names - installed_names)
print(" ".join(generic_only))

# Lightweight wrapper checks. These are intentionally conservative: a warning
# asks for review; it does not claim the implementation is wrong.
wrapper_warnings = []
for name in sorted(installed):
    if name in ("name", "invalid"):
        continue
    if re.search(r"GEN_INTERPRET\(" + re.escape(name) + r"\)", dec):
        body = ""
        for p in JIT.glob("*.cc"):
            body = function_body(p.read_text(errors="replace"), "ppc_opc_" + name)
            if body:
                break
        if "ppc_exception" in body:
            wrapper_warnings.append({"opcode": name, "kind": "exception-in-plain-interpreter-wrapper"})
for name in ("rfi", "mtmsr", "icbi"):
    if name in installed and not re.search(r"GEN_INTERPRET_BRANCH\(" + name + r"\)", dec):
        wrapper_warnings.append({"opcode": name, "kind": "state-changing-opcode-not-branch-wrapper"})
print("\n[wrapper warnings]")
for w in wrapper_warnings:
    print(f"{w['opcode']}: {w['kind']}")

if args.json:
    args.json.write_text(json.dumps({
        "repository": str(ROOT),
        "installed": sorted(installed),
        "native": classes["native"],
        "interpreter": classes["interpreter"],
        "indirect_or_unclassified": classes["missing"],
        "generic_only": generic_only,
        "wrapper_warnings": wrapper_warnings,
    }, indent=2))
