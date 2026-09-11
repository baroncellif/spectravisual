#!/usr/bin/env python3
"""Compiler-derived index of the SpectraVisual sources (audit, read-only).

1. Function boundaries: a small C scanner (comments/strings/preprocessor
   blanked, brace matching) finds every function definition with its first
   and last line.
2. Call graph and state access: clang's JSON AST of each translation unit.
   For every function body it records the functions called, every struct
   field touched (R = read, W = assigned / ++ / --, A = address taken or
   array passed to a call, i.e. possibly written by the callee) and every
   file-scope or function-static variable touched.

Output: functions.md (one row per function, callers computed in reverse),
fields.md (writers/readers per field, multi-writer fields flagged), and
index.json with the raw data.
"""
import json, os, re, subprocess, sys
from collections import defaultdict

REPO, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
CFILES = ["main.c", "controller.c", "loader.c", "predfit.c", "intensity_fit.c", "view.c",
          "layout.c", "settings.c", "algorithms.c", "ui_icons.c"]
SCAN = CFILES + ["plotgpu.cpp", "ui_panels.h", "ui_chrome.h"]
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}


def blank_code(text):
    out, i, n = [], 0, len(text)
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start and c in " \t":
            out.append(c); i += 1; continue
        if at_line_start and c == "#":
            while i < n:
                if text[i] == "\n" and text[i - 1] != "\\":
                    break
                out.append("\n" if text[i] == "\n" else " "); i += 1
            continue
        at_line_start = False
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                out.append(" "); i += 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2); j = n if j < 0 else j + 2
            out.extend("\n" if ch == "\n" else " " for ch in text[i:j]); i = j; continue
        if c in "\"'":
            q = c; out.append(" "); i += 1
            while i < n and text[i] != q:
                if text[i] == "\\": out.append(" "); i += 1
                if i < n: out.append("\n" if text[i] == "\n" else " "); i += 1
            out.append(" "); i += 1; continue
        if c == "\n":
            at_line_start = True
        out.append(c); i += 1
    return "".join(out)


def find_functions(path):
    text = blank_code(open(path, encoding="utf-8", errors="replace").read())
    toks = [(m.group(0), text.count("\n", 0, m.start()) + 1)
            for m in re.finditer(r"[A-Za-z_]\w*|[{}();=,\[\]]", text)]
    funcs, depth, pending = [], 0, []
    i = 0
    while i < len(toks):
        t, line = toks[i]
        if depth == 0:
            if t == "{":
                name, is_fn = None, False
                if pending and pending[-1][0] == ")" and not any(p[0] == "=" for p in pending):
                    lvl, k = 0, len(pending) - 1
                    while k >= 0:
                        if pending[k][0] == ")": lvl += 1
                        elif pending[k][0] == "(":
                            lvl -= 1
                            if lvl == 0: break
                        k -= 1
                    if k > 0 and re.match(r"[A-Za-z_]\w*$", pending[k - 1][0]) and pending[k - 1][0] not in KEYWORDS:
                        name, is_fn = pending[k - 1][0], True
                        start = pending[k - 1][1]
                        storage = {p[0] for p in pending[:k - 1]}
                # find the matching brace
                d, j = 0, i
                while j < len(toks):
                    if toks[j][0] == "{": d += 1
                    elif toks[j][0] == "}":
                        d -= 1
                        if d == 0: break
                    j += 1
                if is_fn:
                    funcs.append({"name": name, "start": start, "end": toks[j][1] if j < len(toks) else line,
                                  "static": "static" in storage, "inline": "inline" in storage})
                pending = []
                i = j + 1
                continue
            if t in (";",):
                pending = []
            else:
                pending.append((t, line))
        i += 1
    return funcs


def file_statics(path):
    """File-scope variables (static or global) declared at column 0."""
    names = set()
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^(?:static\s+)?(?:const\s+)?(?:unsigned\s+)?[A-Za-z_][\w]*(?:\s*\*)*\s+(?:const\s+)?\*?\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:=|;)", line)
        if m and not line.startswith("typedef") and "(" not in line.split("=")[0]:
            names.add(m.group(1))
    return names


defs, by_name = {}, defaultdict(list)
globals_by_file = {}
for f in SCAN:
    p = os.path.join(REPO, f)
    for fn in find_functions(p):
        key = f + "::" + fn["name"]
        fn["file"] = f
        defs[key] = fn
        by_name[fn["name"]].append(key)
    globals_by_file[f] = file_statics(p) if f.endswith(".c") or f.endswith(".cpp") else set()
all_globals = set().union(*globals_by_file.values())

cflags = subprocess.check_output(["sdl2-config", "--cflags"]).decode().split()


def unwrap(e):
    while e and e.get("kind") in ("ImplicitCastExpr", "ParenExpr", "CStyleCastExpr") and e.get("inner"):
        e = e["inner"][0]
    return e


def base_type(me):
    b = me["inner"][0] if me.get("inner") else {}
    qt = b.get("type", {}).get("qualType", "")
    qt = re.sub(r"\b(const|struct|volatile)\b", "", qt).replace("*", "").strip()
    qt = re.sub(r"\[.*\]", "", qt).strip()
    return qt or "?"


def lvalue_chain(e):
    out = []
    while e:
        k = e.get("kind")
        if k == "MemberExpr":
            out.append(e); e = e["inner"][0] if e.get("inner") else None
        elif k in ("ArraySubscriptExpr", "ImplicitCastExpr", "ParenExpr"):
            e = e["inner"][0] if e.get("inner") else None
        elif k == "UnaryOperator" and e.get("opcode") == "*":
            e = e["inner"][0] if e.get("inner") else None
        elif k == "DeclRefExpr":
            out.append(e); break
        else:
            break
    return out


info = {}
for f in CFILES:
    cmd = ["clang", "-Xclang", "-ast-dump=json", "-fsyntax-only", "-w"] + cflags + ["-I", REPO, os.path.join(REPO, f)]
    ast = json.loads(subprocess.run(cmd, capture_output=True, check=True).stdout)
    for node in ast.get("inner", []):
        if node.get("kind") != "FunctionDecl" or not any(ch.get("kind") == "CompoundStmt" for ch in node.get("inner", [])):
            continue
        name = node.get("name")
        keys = [k for k in by_name.get(name, []) if k.startswith(f + "::")] or \
               [k for k in by_name.get(name, []) if k.split("::")[0].endswith(".h")]
        if not keys:
            continue
        key = keys[0]
        if key in info:
            continue
        rec = {"calls": set(), "fields": defaultdict(set), "globals": defaultdict(set), "static_locals": set()}
        marked = {}

        def mark(e, mode):
            marked.setdefault(id(e), set()).add(mode)
            if e.get("kind") == "MemberExpr":
                rec["fields"][base_type(e) + "." + e["name"]].add(mode)
            else:
                rd = e.get("referencedDecl", {})
                nm = rd.get("name")
                if rd.get("kind") == "VarDecl" and (nm in all_globals or nm in rec["static_locals"]):
                    rec["globals"][nm].add(mode)

        def visit(n):
            k = n.get("kind")
            if k == "VarDecl" and n.get("storageClass") == "static":
                rec["static_locals"].add(n.get("name"))
            if k == "CallExpr" and n.get("inner"):
                c = unwrap(n["inner"][0])
                if c and c.get("kind") == "DeclRefExpr" and c.get("referencedDecl", {}).get("kind") == "FunctionDecl":
                    rec["calls"].add(c["referencedDecl"]["name"])
                for a in n["inner"][1:]:
                    if a.get("kind") == "ImplicitCastExpr" and a.get("castKind") == "ArrayToPointerDecay" and a.get("inner"):
                        for m in lvalue_chain(a["inner"][0]): mark(m, "A")
            if (k == "BinaryOperator" and n.get("opcode") == "=") or k == "CompoundAssignOperator":
                for m in lvalue_chain(n["inner"][0]): mark(m, "W")
            if k == "UnaryOperator" and n.get("opcode") in ("++", "--") and n.get("inner"):
                for m in lvalue_chain(n["inner"][0]): mark(m, "W")
            if k == "UnaryOperator" and n.get("opcode") == "&" and n.get("inner"):
                for m in lvalue_chain(n["inner"][0]): mark(m, "A")
            if k in ("MemberExpr", "DeclRefExpr") and not ({"W", "A"} & marked.get(id(n), set())):
                mark(n, "R")
            for ch in n.get("inner", []):
                visit(ch)

        visit(node)
        info[key] = rec

internal = set(by_name)
callers = defaultdict(set)
for key, rec in info.items():
    for c in rec["calls"]:
        for tgt in by_name.get(c, []):
            callers[tgt].add(key.split("::")[1])

rows = []
for key, fn in sorted(defs.items(), key=lambda kv: (SCAN.index(kv[1]["file"]), kv[1]["start"])):
    rec = info.get(key, {"calls": set(), "fields": {}, "globals": {}})
    calls_int = sorted(c for c in rec["calls"] if c in internal)
    calls_ext = sorted(c for c in rec["calls"] if c not in internal)
    fl = rec["fields"]
    wo = sorted(f for f, m in fl.items() if "W" in m and "R" not in m)
    rw = sorted(f for f, m in fl.items() if "W" in m and "R" in m)
    ad = sorted(f for f, m in fl.items() if "A" in m and "W" not in m)
    ro = sorted(f for f, m in fl.items() if "R" in m and "W" not in m and "A" not in m)
    gl = sorted(g + ":" + "".join(sorted(m)) for g, m in rec["globals"].items())
    rows.append({"key": key, "file": fn["file"], "name": fn["name"], "start": fn["start"], "end": fn["end"],
                 "static": fn["static"], "inline": fn["inline"], "callers": sorted(callers.get(key, [])),
                 "calls": calls_int, "calls_ext": calls_ext, "writes": wo + rw, "writes_only": wo, "rw": rw,
                 "addr": ad, "reads": sorted(f for f, m in fl.items() if "R" in m), "reads_only": ro,
                 "globals": gl, "analysed": key in info})

with open(os.path.join(OUT, "index.json"), "w") as fp:
    json.dump(rows, fp, indent=1)

def cell(xs, limit=None):
    xs = list(xs)
    if limit and len(xs) > limit:
        xs = xs[:limit] + ["…(+%d)" % (len(xs) - limit)]
    return ", ".join("`%s`" % x for x in xs) if xs else "—"

with open(os.path.join(OUT, "functions.md"), "w") as fp:
    cur = None
    for i, r in enumerate(rows, 1):
        if r["file"] != cur:
            cur = r["file"]
            fp.write("\n### `%s`\n\n| ID | Funzione | Linee | Tipo | Chiamanti | Chiamate interne | Scrive senza leggere (W) | Legge **e** scrive (RW) | Per indirizzo / array a una chiamata (A) | Solo letti (R) | Globali/statici (R/W/A) | Chiamate esterne |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n" % cur)
        kind = ("static " if r["static"] else "") + ("inline" if r["inline"] else "") or "extern"
        fid = "F-%s-%s" % (os.path.splitext(r["file"])[0].upper(), r["name"])
        fp.write("| <a id=\"%s\"></a>`%s` | `%s` | [%d–%d](../../%s#L%d-L%d) | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n" % (
            fid.lower(), fid, r["name"], r["start"], r["end"], r["file"], r["start"], r["end"], kind.strip() or "extern",
            cell(r["callers"]), cell(r["calls"]), cell(r["writes_only"]), cell(r["rw"]), cell(r["addr"]),
            cell(r["reads_only"], 40), cell(r["globals"]),
            cell(r["calls_ext"], 25) if r["analysed"] else "(non analizzata: C++/header)"))

field_w, field_r, field_a = defaultdict(set), defaultdict(set), defaultdict(set)
for key, rec in info.items():
    for fld, m in rec["fields"].items():
        if "W" in m: field_w[fld].add(key)
        if "A" in m: field_a[fld].add(key)
        if "R" in m: field_r[fld].add(key)
with open(os.path.join(OUT, "fields.md"), "w") as fp:
    fp.write("| Campo | #writer (W) | Writer (W) | Passato per indirizzo (A) | #reader | Reader |\n|---|---|---|---|---|---|\n")
    for fld in sorted(set(field_w) | set(field_r) | set(field_a), key=lambda x: (-len(field_w[x]), x)):
        fp.write("| `%s` | %d | %s | %s | %d | %s |\n" % (
            fld, len(field_w[fld]), cell(sorted(k.replace("::", ":") for k in field_w[fld])),
            cell(sorted(k.replace("::", ":") for k in field_a[fld])), len(field_r[fld]),
            cell(sorted(k.replace("::", ":") for k in field_r[fld]), 30)))
print("functions:", len(rows), "analysed:", sum(r["analysed"] for r in rows), "fields:", len(set(field_w) | set(field_r)))
