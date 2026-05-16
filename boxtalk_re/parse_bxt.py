#!/usr/bin/env python3
"""Parse bxt_capture.log → reassemble per-direction streams → split on NUL → emit one XML per line + summary."""

import re, sys, xml.etree.ElementTree as ET
from collections import Counter, defaultdict

def load_capture(path):
    with open(path, 'rb') as f:
        data = f.read()
    pat = re.compile(rb'\n=([RWSQ]) fd=(\d+) t=(\d+)\.(\d+) n=(\d+)\n', re.DOTALL)
    matches = list(pat.finditer(data))
    streams = defaultdict(bytearray)  # (fd, dir) -> bytes
    events = []
    for m in matches:
        dir_ = m.group(1).decode()
        fd = int(m.group(2))
        ts = float(m.group(3) + b'.' + m.group(4))
        n = int(m.group(5))
        payload = bytes(data[m.end():m.end()+n])
        # tracker for truncated payloads (header may have requested n but actual data ends with truncation marker)
        if b"[...truncated...]" in payload:
            payload = payload.split(b"\n[...truncated...]")[0]
        streams[(fd, dir_)].extend(payload)
        events.append((ts, fd, dir_, len(payload)))
    return streams, events

def split_messages(stream):
    """Split a stream on NUL bytes, drop empty"""
    return [m for m in stream.split(b'\x00') if m.strip()]

def classify(xml_text):
    try:
        # ezxml-style: messages have a root element
        # Strip XML processing instructions
        s = xml_text.lstrip()
        if not s.startswith(b'<'):
            return ('banner-or-other', s[:80].decode('utf-8','replace'))
        root = ET.fromstring(xml_text)
        return (root.tag, root.attrib)
    except Exception as e:
        return ('parse-error', str(e))

def summarize_message(xml_text):
    try:
        root = ET.fromstring(xml_text)
        tag = root.tag
        attrs = root.attrib
        info = {
            'tag': tag,
            'class': attrs.get('class'),
            'nts': attrs.get('nts'),
            'uuid': attrs.get('uuid'),
            'destuuid': attrs.get('destuuid'),
            'type': attrs.get('type'),
            'serviceid': attrs.get('serviceid'),
            'requestid': attrs.get('requestid'),
        }
        # Look for first child action verb
        for child in root:
            cn = re.sub(r'^\{[^}]+\}', '', child.tag)
            info['op'] = cn
            break
        return info
    except Exception:
        return {'tag': '?', 'raw': xml_text[:120].decode('utf-8','replace')}

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'bxt_capture.log'
    streams, events = load_capture(path)
    print(f"=== streams ({len(streams)}) ===")
    for k, s in sorted(streams.items()):
        msgs = split_messages(bytes(s))
        print(f"  fd={k[0]} dir={k[1]} stream={len(s)}B msgs={len(msgs)}")
    print()

    print("=== ALL UNIQUE MESSAGE TAGS / classes / nts / ops ===")
    tag_attrs = Counter()
    service_actions = defaultdict(Counter)
    discovery_types = Counter()
    for k, s in streams.items():
        for raw in split_messages(bytes(s)):
            info = summarize_message(raw)
            tag = info.get('tag')
            cls = info.get('class') or info.get('nts') or ''
            op = info.get('op') or ''
            sid = info.get('serviceid') or ''
            tag_attrs[(tag, cls, op, sid)] += 1
            if tag == 'discovery':
                discovery_types[info.get('type','?')] += 1
            elif tag in ('action','query','op'):
                sid_short = sid.split(':')[-1] if sid else '?'
                service_actions[sid_short][op] += 1
    print(f"{'count':>6}  tag        | class/nts            op                                   serviceid")
    for (tag, cls, op, sid), n in tag_attrs.most_common():
        sid_short = sid.split(':')[-1] if sid else ''
        print(f"{n:>6}  {tag:<10} | {cls:<18} {op:<35} {sid_short}")
    print()
    print(f"=== discovery types (devices) ===")
    for t, n in discovery_types.most_common():
        print(f"  {n:>4}  {t}")
    print()
    print(f"=== per-service action verbs ===")
    for sid, verbs in service_actions.items():
        print(f"  {sid}:")
        for v, n in verbs.most_common():
            print(f"    {n:>4}  {v}")
