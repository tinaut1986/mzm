#!/usr/bin/env python3
"""Keeps src/ and include/ compiling back to upstream's text when the port is off.

The GBA ROM built from this tree (`make REGION=eu`) has to match the retail one,
so every change the port makes to a file that comes from metroidret/mzm must sit
inside `#if defined(MZM_3DS) || defined(PORT_NATIVE)`, with upstream's text in
`#else`. This tool checks that, and can add the guards.

The "GBA view" of a file is the file with the conditionals that mention only
port macros resolved as if no port macro were defined. The check is: the GBA
view of every file equals upstream's text (at the commit in tools/upstream_base).

    tools/guard_port.py check [PATH...]   # exit 1 and list the files that differ
    tools/guard_port.py fix [PATH...]     # wrap the differences in guards

PATH defaults to src/ and include/. The upstream text is read with
`git show <rev>:<path>`; the revision comes from tools/upstream_base or --rev,
and has to be in the local repository (`git fetch <upstream url> <rev>`).
Files that upstream does not have (the port's own) are skipped.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys

PORT_MACROS = {"MZM_3DS", "PORT_NATIVE", "__3DS__", "PLATFORM_LINUX", "PORT_LINUX_DIAG_WARP_TO_DEOREM",
               "PORT_VERBOSE_FRAME_LOG", "PLATFORM_3DS", "PORT_HOST_TEST", "PORT_DEBUG_TOOLS_ACTIVE"}
PORT_ON = {"MZM_3DS", "PORT_NATIVE", "__3DS__"}  # defined in the port build(s) we care about
GUARD = "#if defined(MZM_3DS) || defined(PORT_NATIVE)"
DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def evaluate(kind, rest, values):
    """True/False if the condition only mentions port macros, else None."""
    rest = re.sub(r"/\*.*?\*/|//.*$", "", rest).strip()
    if kind == "ifdef":
        names = [rest.split()[0]] if rest else []
        if len(names) == 1 and names[0] in PORT_MACROS:
            return values.get(names[0], False)
        return None
    if kind == "ifndef":
        names = [rest.split()[0]] if rest else []
        if len(names) == 1 and names[0] in PORT_MACROS:
            return not values.get(names[0], False)
        return None
    ids = set(re.findall(r"[A-Za-z_]\w*", rest)) - {"defined"}
    if not ids or not ids <= PORT_MACROS:
        return None
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                  lambda m: "1" if values.get(m.group(1) or m.group(2), False) else "0", rest)
    expr = re.sub(r"[A-Za-z_]\w*", lambda m: "1" if values.get(m.group(0), False) else "0", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))
    except Exception:
        return None


def view(lines, values):
    """(kept lines, index of each kept line in `lines`) with port-only conditionals resolved."""
    out, idx = [], []
    # frame: [is_port, taken_before, active_now]
    stack = []
    live = True
    for n, line in enumerate(lines):
        m = DIRECTIVE.match(line)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                val = evaluate(kind, rest, values)
                if val is None:
                    stack.append([False, False, True])
                    if live:
                        out.append(line); idx.append(n)
                else:
                    stack.append([True, val, val])
                    live = all(f[2] for f in stack if f[0])
                continue
            if kind == "elif":
                f = stack[-1]
                if f[0]:
                    val = evaluate("if", rest, values)
                    if val is None:
                        raise ValueError("elif mixes port and other macros at line %d" % (n + 1))
                    f[2] = (not f[1]) and val
                    f[1] = f[1] or val
                    live = all(g[2] for g in stack if g[0])
                    continue
                if live:
                    out.append(line); idx.append(n)
                continue
            if kind == "else":
                f = stack[-1]
                if f[0]:
                    f[2] = not f[1]
                    f[1] = True
                    live = all(g[2] for g in stack if g[0])
                    continue
                if live:
                    out.append(line); idx.append(n)
                continue
            if kind == "endif":
                f = stack.pop()
                if f[0]:
                    live = all(g[2] for g in stack if g[0])
                    continue
                if live:
                    out.append(line); idx.append(n)
                continue
        if live:
            out.append(line); idx.append(n)
    return out, idx


def balance(lines):
    """Depth change and the lowest point of the other-conditionals in `lines`."""
    d = low = 0
    for line in lines:
        m = DIRECTIVE.match(line)
        if not m:
            continue
        k = m.group(1)
        if k in ("if", "ifdef", "ifndef"):
            d += 1
        elif k == "endif":
            d -= 1
            low = min(low, d)
    return d, low


def norm(line):
    # a comment after a directive (`#endif // REGION_EU`) is not a difference
    if DIRECTIVE.match(line):
        line = re.sub(r"\s*(//.*|/\*.*\*/)\s*$", "", line)
    return line.rstrip()


def upstream_text(rev, path):
    r = subprocess.run(["git", "show", "%s:%s" % (rev, path)], capture_output=True)
    return r.stdout.decode("utf-8", "surrogateescape") if r.returncode == 0 else None


def wrap_file(up, ours, label):
    """Returns the new text of `ours` (list of lines) or None if it needs no change; exits on error."""
    gba, gidx = view(ours, {})
    sm = difflib.SequenceMatcher(None, [norm(x) for x in gba], [norm(x) for x in up], autojunk=False)
    ops = sm.get_opcodes()
    hunks = []
    k = 0
    while k < len(ops):
        tag, i1, i2, j1, j2 = ops[k]
        k += 1
        if tag == "equal":
            continue
        a1, a2, b1, b2 = i1, i2, j1, j2
        while True:
            da, la = balance(gba[a1:a2])
            db, lb = balance(up[b1:b2])
            if da == 0 and la == 0 and db == 0 and lb == 0:
                break
            if k >= len(ops):
                raise ValueError("%s: cannot balance a difference near line %d" % (label, gidx[min(a1, len(gidx) - 1)] + 1))
            a2, b2 = ops[k][2], ops[k][4]
            k += 1
        hunks.append((a1, a2, b1, b2))
    if not hunks:
        return None
    edits = []  # (o1, o2, block); a run of our lines is contiguous in the file
    for a1, a2, b1, b2 in hunks:
        theirs = up[b1:b2]
        runs = []
        for n in range(a1, a2):
            if runs and gidx[n] == runs[-1][1]:
                runs[-1][1] = gidx[n] + 1
            else:
                runs.append([gidx[n], gidx[n] + 1])
        if not runs:
            at = gidx[a1] if a1 < len(gidx) else len(ours)
            edits.append((at, at, ["#if !(defined(MZM_3DS) || defined(PORT_NATIVE))"] + theirs + ["#endif"]))
            continue
        for n, (o1, o2) in enumerate(runs):
            mine = ours[o1:o2]
            if n == 0 and theirs:
                block = [GUARD] + mine + ["#else"] + theirs + ["#endif"]
            else:
                block = [GUARD] + mine + ["#endif"]
            edits.append((o1, o2, block))
    result = list(ours)
    for o1, o2, block in sorted(edits, key=lambda e: -e[0]):
        result[o1:o2] = block
    if result[-1] != "":  # keep the final newline
        result.append("")
    check, _ = view(result, {})
    if [norm(c) for c in check] != [norm(c) for c in up]:
        raise ValueError("%s: the guarded file still differs from upstream; fix it by hand" % label)
    stack = []
    for n, line in enumerate(result):
        m = DIRECTIVE.match(line)
        if not m:
            continue
        if m.group(1) in ("if", "ifdef", "ifndef"):
            stack.append(n)
        elif m.group(1) == "endif":
            if not stack:
                raise ValueError("%s: unbalanced #endif at line %d after guarding" % (label, n + 1))
            stack.pop()
    if stack:
        raise ValueError("%s: unbalanced #if at line %d after guarding" % (label, stack[-1] + 1))
    before, _ = view(ours, {m: True for m in PORT_ON})
    after, _ = view(result, {m: True for m in PORT_ON})
    if [norm(x) for x in before] != [norm(x) for x in after]:
        raise ValueError("%s: guarding would change what the port builds" % label)
    return result


def source_files(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                out += [os.path.join(root, n) for n in names if n.endswith((".c", ".h", ".inc"))]
        else:
            out.append(p)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["check", "fix"])
    ap.add_argument("paths", nargs="*", default=["src", "include"])
    ap.add_argument("--rev", help="upstream revision (default: the one in tools/upstream_base)")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    rev = args.rev or open("tools/upstream_base").read().split()[0]
    if subprocess.run(["git", "cat-file", "-e", rev + "^{commit}"], capture_output=True).returncode != 0:
        print("upstream revision %s is not in this repository; git fetch it first" % rev)
        return 2
    differing, errors = [], []
    for path in source_files(args.paths):
        up_text = upstream_text(rev, path)
        if up_text is None:
            continue
        ours = open(path, newline="", errors="surrogateescape").read().split("\n")
        try:
            result = wrap_file(up_text.split("\n"), ours, path)
        except ValueError as e:
            errors.append(str(e))
            continue
        if result is None:
            continue
        differing.append(path)
        if args.mode == "fix":
            open(path, "w", newline="", errors="surrogateescape").write("\n".join(result))
    for e in errors:
        print("ERROR", e)
    if args.mode == "check":
        for p in differing:
            print("unguarded port change:", p)
        if differing or errors:
            print("\n%d file(s) differ from upstream with the port macros off; run tools/guard_port.py fix" % (len(differing) + len(errors)))
            return 1
        print("ok: every file matches upstream with the port macros off")
        return 0
    print("guarded %d file(s)" % len(differing))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
