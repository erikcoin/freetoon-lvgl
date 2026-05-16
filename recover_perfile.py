#!/usr/bin/env python3
"""Per-file bisection: for each path under /tmp/qt_rebuild/lvgl_ui/src/, find
the LAST event in transcript history that left the file in a non-splat state,
and emit that snapshot to /tmp/qt_rebuild/lvgl_ui_recovered/src/.

Each file is reconstructed independently — there's no global cutoff, so we
don't have to roll back the entire project to the time the first splat
appeared. Files added late (e.g. weather.c, stats.c) get the latest clean
revision they ever had; files corrupted early get the latest clean revision
they ever had.
"""
import json, glob, os, re, sys
from collections import Counter

TX_DIR     = os.path.expanduser("~/.claude/projects/-tmp")
PREFIX     = "/tmp/qt_rebuild/"
SUBDIR     = "lvgl_ui/src/"
OUT_PREFIX = "/tmp/qt_rebuild/lvgl_ui_recovered/src/"

def collect():
    events = []
    for jf in sorted(glob.glob(os.path.join(TX_DIR, "*.jsonl"))):
        with open(jf, errors='replace') as fh:
            for line in fh:
                line = line.strip()
                if not line: continue
                try: obj = json.loads(line)
                except Exception: continue
                ts = obj.get("timestamp", "")
                msg = obj.get("message", obj)
                content = msg.get("content")
                if not isinstance(content, list): continue
                for b in content:
                    if not (isinstance(b, dict) and b.get("type") == "tool_use"):
                        continue
                    name = b.get("name")
                    inp = b.get("input") or {}
                    fp = inp.get("file_path", "")
                    if not fp.startswith(PREFIX + SUBDIR): continue
                    rel = fp[len(PREFIX + SUBDIR):]
                    if "/" in rel: continue       # only top-level files in src/
                    if name == "Write":
                        events.append((ts, "write", fp, inp.get("content", "")))
                    elif name == "Edit":
                        events.append((ts, "edit", fp,
                            (inp.get("old_string", ""),
                             inp.get("new_string", ""),
                             bool(inp.get("replace_all", False)))))
                    elif name == "MultiEdit":
                        events.append((ts, "multiedit", fp, inp.get("edits", [])))
    events.sort(key=lambda e: e[0])
    return events

def apply_one(content, op, payload):
    if op == "write":
        return payload
    if content is None:
        return None
    if op == "edit":
        old, new, rall = payload
        if old not in content: return content
        return content.replace(old, new) if rall else content.replace(old, new, 1)
    if op == "multiedit":
        cur = content
        for ed in payload:
            old = ed.get("old_string", ""); new = ed.get("new_string", "")
            rall = bool(ed.get("replace_all", False))
            if old in cur:
                cur = cur.replace(old, new) if rall else cur.replace(old, new, 1)
        return cur
    return content

def is_dirty(path, content):
    if content is None: return "no content"
    base = os.path.basename(path)
    lines = content.splitlines()
    # generic: any non-trivial static decl repeated 2+ times
    statics = [ln.strip() for ln in lines
               if ln.lstrip().startswith(("static ", "extern ", "LV_FONT_DECLARE"))
               and len(ln.strip()) > 30]
    c = Counter(statics)
    for ln, n in c.items():
        if n >= 2:
            return f"repeated decl ({n}x): {ln[:70]}"
    # .c files should start with a recognizable construct
    if base.endswith(".c"):
        for ln in lines[:30]:
            s = ln.strip()
            if not s or s.startswith(("//", "/*", "*", "#")): continue
            if s.startswith("if ("):
                return f"orphan if at top: {s[:50]}"
            break
    # .h files: no duplicate struct members
    if base.endswith(".h"):
        members = re.findall(r"^\s+([A-Za-z_][\w ]*?\s+\w+\s*;)", content, re.M)
        c = Counter(m.strip() for m in members)
        for m, n in c.items():
            if n >= 2 and "/*" not in m and len(m) > 8:
                return f"dup struct member ({n}x): {m[:60]}"
    # Makefile splats
    if base == "Makefile":
        if content.count("-MMD -MP") > 1: return "Makefile dup -MMD -MP"
        if content.count("lv_font_montserrat_96_custom.c") > 1: return "Makefile dup font src"
        if "icons.c icons.c icons.c" in content: return "Makefile APP_SRC splat"
    return None

def main():
    events = collect()
    print(f"collected {len(events)} src-targeting events")
    # bucket events per file path
    per_file = {}
    for ev in events:
        per_file.setdefault(ev[2], []).append(ev)

    os.makedirs(OUT_PREFIX, exist_ok=True)
    summary = []
    for path, evs in sorted(per_file.items()):
        cur = None
        last_clean = None      # (idx, ts, content)
        for idx, (ts, op, _path, payload) in enumerate(evs):
            cur = apply_one(cur, op, payload)
            dirt = is_dirty(path, cur)
            if dirt is None:
                last_clean = (idx, ts, cur)
        rel = path[len(PREFIX + SUBDIR):]
        out_path = os.path.join(OUT_PREFIX, rel)
        if last_clean is None:
            summary.append((rel, len(evs), None, "no clean rev found", None))
            continue
        with open(out_path, "w") as f:
            f.write(last_clean[2])
        summary.append((rel, len(evs), last_clean[0], "OK", last_clean[1]))

    # print table
    print(f"{'file':<35} {'#evts':>6} {'clean@idx':>10}  {'ts':<24} status")
    for rel, nev, idx, status, ts in summary:
        idxs = "-" if idx is None else str(idx)
        tss  = "-" if ts is None else ts
        print(f"{rel:<35} {nev:>6} {idxs:>10}  {tss:<24} {status}")

    # also report still-dirty count
    dirty = [s for s in summary if s[3] != "OK"]
    print(f"\n{len(summary) - len(dirty)} files recovered clean, {len(dirty)} still dirty")
    print(f"output: {OUT_PREFIX}")

if __name__ == "__main__":
    main()
