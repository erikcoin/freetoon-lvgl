#!/usr/bin/env python3
"""Analyze the ttymxc0 capture."""
import re
import sys
from collections import Counter

w_bytes = []
r_bytes = []
events = []  # (time, dir, byte)

with open('/tmp/qt_rebuild/captures/ttymxc0_capture.log') as f:
    for line in f:
        m = re.match(r'([WR]) (\d+\.\d+) n=(\d+) (.+)', line)
        if not m:
            continue
        d, t, n, payload = m.groups()
        bs = [int(x, 16) for x in payload.split()]
        for b in bs:
            events.append((float(t), d, b))
        if d == 'W':
            w_bytes.extend(bs)
        else:
            r_bytes.extend(bs)

print(f"writes total bytes: {len(w_bytes)}, reads total bytes: {len(r_bytes)}")
print(f"events: {len(events)}")
print("\nTop write bytes:")
for b, c in Counter(w_bytes).most_common(20):
    print(f"  0x{b:02x} : {c}")
print("\nTop read bytes:")
for b, c in Counter(r_bytes).most_common(20):
    print(f"  0x{b:02x} : {c}")

print(f"\ndistinct write bytes: {len(set(w_bytes))}")
print(f"distinct read bytes:  {len(set(r_bytes))}")
print(f"distinct write set: {sorted(set(w_bytes))}")
print(f"distinct read set:  {sorted(set(r_bytes))}")
