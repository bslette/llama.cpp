#!/usr/bin/env python3
"""cmp.py out.yuv official.md5: compare vp9test output (with out.yuv.sizes) frame by frame with libvpx's md5 list."""
import hashlib
import re
import sys

yuv, ref = sys.argv[1], sys.argv[2]
sizes = [l.split() for l in open(yuv + ".sizes")]
expect = []
for line in open(ref):
    md5, name = line.split()
    m = re.search(r"-(\d+)x(\d+)-\d+\.i420$", name)
    w, h = map(int, m.groups()) if m else (None, None)  # some lists name frames without sizes
    expect.append((md5, w, h))

bad, f = 0, open(yuv, "rb")
for i, s in enumerate(sizes):
    w, h = int(s[0]), int(s[1])
    data = f.read(w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2))
    got = hashlib.md5(data).hexdigest()
    if i >= len(expect):
        print(f"frame {i}: extra output {w}x{h}")
        bad += 1
        continue
    md5, ew, eh = expect[i]
    if ew is None:
        ew, eh = w, h
    if (w, h) != (ew, eh) or got != md5:
        bad += 1
        if bad <= 5:
            print(f"frame {i}: {w}x{h} {got[:8]}  expected {ew}x{eh} {md5[:8]}{' (engine error)' if len(s) > 2 else ''}")
missing = len(expect) - len(sizes)
status = "OK" if not bad and not missing else "FAIL"
print(f"{status}: {len(sizes)}/{len(expect)} frames, {bad} mismatched{f', {missing} missing' if missing > 0 else ''}")
sys.exit(0 if status == "OK" else 1)
