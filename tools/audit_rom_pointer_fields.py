#!/usr/bin/env python3
"""
Heuristic audit for the "raw GBA ROM pointer copied without translation" bug
class that has been found and fixed repeatedly in this port (see
docs/3ds-port-status-2026-08-17.md sections 5.1, 5.5, 6b, 6c, 6d, and the
audio pHeader/pSample bugs): a pointer-typed struct field whose value was
read straight out of ROM-mapped memory gets assigned to another variable and
later dereferenced on the host, without going through GBA_RESOLVE()/
port_resolve_addr() first.

This is NOT a type-checked static analyzer -- mzm's struct fields
consistently follow a `pName` (Hungarian-notation) convention for pointers,
so this tool works off that naming convention with plain regexes, not a real
C parser. It will have false positives (report something already safe) and
false negatives (miss a real bug). Its job is to narrow down where to look
by hand, the same way the bugs in this file's docstring were originally
found: `grep -rL GBA_RESOLVE src/*.c` plus manually checking pointer-field
assignments in the files that came back with zero hits.

Usage:
    python3 tools/audit_rom_pointer_fields.py [src_dir]

Exit code is always 0 -- this is a report, not a CI gate.
"""
import re
import sys
from pathlib import Path

POINTER_FIELD = re.compile(r"[.\->]{1,2}(p[A-Z][A-Za-z0-9_]*)\b")
ASSIGN_FROM_FIELD = re.compile(
    r"^\s*([\w.\->\[\]]+)\s*=\s*[\w]+(?:\[[^\]]*\])?(?:->|\.)(p[A-Z][A-Za-z0-9_]*)\s*;"
)
# This codebase closes top-level function bodies with a `}` at column 0 (no
# leading whitespace) -- used as the scan boundary so a later function's
# unrelated same-named local variable doesn't get attributed to this one.
FUNC_END = re.compile(r"^\}\s*$")


def find_c_files(src_dir: Path):
    return sorted(src_dir.rglob("*.c"))


def scan_file(path: Path):
    text = path.read_text(errors="replace")
    lines = text.splitlines()
    findings = []

    for i, line in enumerate(lines):
        if "GBA_RESOLVE(" in line:
            continue
        m = ASSIGN_FROM_FIELD.match(line)
        if not m:
            continue
        lhs, field = m.group(1), m.group(2)
        # Common idiom in this codebase: assign the raw field, then resolve
        # it in place on one of the next couple of lines, e.g.
        #   pFrame = pOamData[pOam->oamId].pOam;
        #   pFrame = GBA_RESOLVE(pFrame);
        # Treat that as already handled.
        lhs_escaped_pre = re.escape(lhs)
        already_resolved = any(
            f"GBA_RESOLVE({lhs})" in lines[k] or re.search(rf"\b{lhs_escaped_pre}\s*=\s*GBA_RESOLVE", lines[k])
            for k in range(i + 1, min(i + 4, len(lines)))
        )
        if already_resolved:
            continue
        # Only care about the copy if the field's value is later dereferenced
        # (->, [, or a leading *) rather than merely compared by identity --
        # comparisons against a fresh, equally-raw re-read are the pattern we
        # deliberately preserved when fixing pHeader in InitTrack.
        lhs_escaped = re.escape(lhs)
        deref_pat = re.compile(rf"\b{lhs_escaped}\s*(->|\[)|(\*\s*{lhs_escaped}\b)")
        confidence = "info"
        deref_line = None
        for j in range(i + 1, min(i + 400, len(lines))):
            if deref_pat.search(lines[j]):
                confidence = "high"
                deref_line = j + 1
                break
            if FUNC_END.match(lines[j]):
                break
        findings.append((i + 1, field, lhs, confidence, deref_line, line.strip()))

    return findings


def main():
    src_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("src")
    if not src_dir.is_dir():
        print(f"error: {src_dir} is not a directory", file=sys.stderr)
        return 1

    total_high = 0
    total_info = 0
    for path in find_c_files(src_dir):
        findings = scan_file(path)
        if not findings:
            continue
        printed_header = False
        for line_no, field, lhs, confidence, deref_line, snippet in findings:
            if confidence != "high":
                total_info += 1
                continue
            total_high += 1
            if not printed_header:
                print(f"\n{path}")
                printed_header = True
            print(f"  L{line_no}: `{lhs}` copied from .{field} without GBA_RESOLVE, "
                  f"dereferenced at L{deref_line}")
            print(f"       {snippet}")

    print(f"\n{total_high} high-confidence candidate(s), {total_info} lower-confidence "
          f"(identity-compare-only) skipped.")
    print("Heuristic, naming-convention based -- verify each hit by hand before fixing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
