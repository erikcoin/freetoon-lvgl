#!/usr/bin/env python3
"""Take the LATEST version of each src/*.{c,h} from transcript history,
then deduplicate splatted blocks. Two passes:

  1) line-level: collapse N consecutive identical lines to 1
  2) block-level: find the longest sequence k starting at line i such that
     lines[i:i+k] == lines[i+k:i+2k], collapse to one copy, repeat until no
     such adjacent-duplicate sequence exists at i. Then move to i+1.

This handles both 'volatile float water_pressure;' (1-line) and the bigger
'static lv_obj_t * fc_day_lbl[…]\\nstatic …' chunks (multi-line).
"""
import json, glob, os, re, sys

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
                    if "/" in rel: continue
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

def dedup_lines(content):
    """Pass 1: collapse consecutive identical non-blank lines."""
    out, prev = [], None
    for ln in content.splitlines(keepends=True):
        if ln == prev and ln.strip():
            continue
        out.append(ln); prev = ln
    return "".join(out)

def dedup_blocks(content, max_k=80):
    """Pass 2: collapse consecutive duplicate line-sequences of length 1..max_k.
    Scans left-to-right; at each position finds the longest k for which
    lines[i:i+k] == lines[i+k:i+2k], keeps one copy, advances past all
    consecutive copies, continues."""
    lines = content.splitlines()
    out, i = [], 0
    while i < len(lines):
        found = False
        for k in range(min(max_k, (len(lines) - i) // 2), 0, -1):
            if lines[i:i+k] == lines[i+k:i+2*k]:
                out.extend(lines[i:i+k])
                j = i + 2*k
                while j + k <= len(lines) and lines[i:i+k] == lines[j:j+k]:
                    j += k
                i = j
                found = True
                break
        if not found:
            out.append(lines[i])
            i += 1
    # preserve trailing newline if present
    res = "\n".join(out)
    if content.endswith("\n") and not res.endswith("\n"):
        res += "\n"
    return res

def latest(events_for_path):
    cur = None
    for (_ts, op, _p, payload) in events_for_path:
        cur = apply_one(cur, op, payload)
    return cur

def main():
    events = collect()
    per_file = {}
    for ev in events:
        per_file.setdefault(ev[2], []).append(ev)

    os.makedirs(OUT_PREFIX, exist_ok=True)
    summary = []
    for path, evs in sorted(per_file.items()):
        latest_content = latest(evs)
        if latest_content is None:
            summary.append((path, len(evs), "skip-no-write", 0, 0)); continue
        a = dedup_lines(latest_content)
        b = dedup_blocks(a)
        rel = path[len(PREFIX + SUBDIR):]
        out_path = os.path.join(OUT_PREFIX, rel)
        with open(out_path, "w") as f:
            f.write(b)
        summary.append((rel, len(evs), "OK",
                        len(latest_content.splitlines()),
                        len(b.splitlines())))

    print(f"{'file':<32} {'#evt':>5} {'before':>7} {'after':>6}  status")
    for rel, n, st, before, after in summary:
        print(f"{rel:<32} {n:>5} {before:>7} {after:>6}  {st}")

if __name__ == "__main__":
    main()
