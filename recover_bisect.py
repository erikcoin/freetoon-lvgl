#!/usr/bin/env python3
"""Find the last-clean cutoff in Claude transcript history and reconstruct
the toonui source tree at /tmp/qt_rebuild/lvgl_ui_recovered/src/ using only
events up to that cutoff.

Approach: replay Write/Edit/MultiEdit events in timestamp order; after each
event check four "is splat" heuristics; remember the latest timestamp where
ALL heuristics still pass. Then re-replay events up to that cutoff and write
files to disk.
"""
import json, glob, os, re, sys

TX_DIR = os.path.expanduser("~/.claude/projects/-tmp")
PREFIX = "/tmp/qt_rebuild/"
OUT_PREFIX = "/tmp/qt_rebuild/lvgl_ui_recovered/"

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
                    if not fp.startswith(PREFIX): continue
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

def apply_one(files, op, path, payload):
    if op == "write":
        files[path] = payload
    elif op == "edit":
        old, new, rall = payload
        cur = files.get(path)
        if cur is None or old not in cur: return
        files[path] = cur.replace(old, new) if rall else cur.replace(old, new, 1)
    elif op == "multiedit":
        cur = files.get(path)
        if cur is None: return
        for ed in payload:
            old = ed.get("old_string", ""); new = ed.get("new_string", "")
            rall = bool(ed.get("replace_all", False))
            if old in cur:
                cur = cur.replace(old, new) if rall else cur.replace(old, new, 1)
        files[path] = cur

def is_dirty(files):
    h = files.get(PREFIX + "lvgl_ui/src/screen_home.c")
    if h:
        # orphan if-block at file scope = splat
        lines_top = h.lstrip().splitlines()[:1]
        if lines_top and lines_top[0].strip().startswith("if (envelope_btn)"):
            return "screen_home.c starts with orphan if"
    bh = files.get(PREFIX + "lvgl_ui/src/boxtalk.h")
    if bh and len(re.findall(r"volatile float +water_pressure", bh)) > 1:
        return "boxtalk.h dup water_pressure"
    sd = files.get(PREFIX + "lvgl_ui/src/screen_dim.c")
    if sd and sd.count("LV_FONT_DECLARE(lv_font_montserrat_96_custom)") > 1:
        return "screen_dim.c dup LV_FONT_DECLARE"
    mk = files.get(PREFIX + "lvgl_ui/src/Makefile")
    if mk:
        if mk.count("lv_font_montserrat_96_custom.c") > 1:
            return "Makefile dup font src"
        if mk.count("-MMD -MP") > 1:
            return "Makefile dup -MMD -MP"
    return None

def main():
    events = collect()
    print(f"collected {len(events)} events from {TX_DIR}")
    # forward pass — find last clean event index
    files = {}
    last_clean_idx = -1
    first_dirty = None
    for i, (ts, op, path, payload) in enumerate(events):
        apply_one(files, op, path, payload)
        # only check after we've touched something in src/
        if not path.startswith(PREFIX + "lvgl_ui/src/"): continue
        dirt = is_dirty(files)
        if dirt is None:
            last_clean_idx = i
        elif first_dirty is None:
            first_dirty = (i, ts, dirt)

    if first_dirty:
        print(f"first dirty event #{first_dirty[0]} at ts={first_dirty[1]} ({first_dirty[2]})")
    else:
        print("no dirty state ever observed — replaying everything")
    print(f"last clean event index: {last_clean_idx} (ts={events[last_clean_idx][0] if last_clean_idx >= 0 else '-'})")

    # second pass — replay events 0..last_clean_idx INCLUSIVE
    files = {}
    for i, (ts, op, path, payload) in enumerate(events):
        if i > last_clean_idx: break
        apply_one(files, op, path, payload)

    # write all reconstructed files under lvgl_ui/src/ to OUT_PREFIX/src/
    n = 0
    for path, content in files.items():
        if not path.startswith(PREFIX + "lvgl_ui/src/"): continue
        rel = path[len(PREFIX + "lvgl_ui/src/"):]
        out_path = os.path.join(OUT_PREFIX, "src", rel)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w") as f:
            f.write(content)
        n += 1
    print(f"wrote {n} files to {OUT_PREFIX}src/")
    # also verify post-write heuristics on disk
    final = {}
    for path, content in files.items():
        if path.startswith(PREFIX + "lvgl_ui/src/"):
            final[path] = content
    final_dirt = is_dirty(final)
    print(f"final dirt check: {final_dirt}")

if __name__ == "__main__":
    main()
