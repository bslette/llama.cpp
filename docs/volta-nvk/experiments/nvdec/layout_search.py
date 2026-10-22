# Find the luma layout of a dumped NVDEC surface by scoring candidate de-tilers against the reference.
import sys
W, H, PITCH = 320, 240, 320
ref = open(sys.argv[1], 'rb').read()[:W * H]
def gob_std(x, y):   # 16Bx2 sectors (Maxwell+ / Tegra GOB)
    return ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)
def gob_rows(x, y):  # plain 64 B rows
    return (y % 8) * 64 + (x % 64)
def gob_16x2_linear(x, y):  # sectors of 16x2 stored in raster order of sectors
    return ((y % 8) // 2) * 128 + ((x % 64) // 16) * 32 + (y % 2) * 16 + (x % 16)
def bl(gob, bh, pitch=PITCH):
    gobs_x = (pitch + 63) // 64
    def f(x, y):
        blk = (y // (8 * bh)) * gobs_x + x // 64
        return blk * 512 * bh + ((y % (8 * bh)) // 8) * 512 + gob(x, y)
    return f
def tile16(x, y, pitch=PITCH):
    return ((y // 16) * (pitch // 16) + x // 16) * 256 + (y % 16) * 16 + (x % 16)
def linear(x, y): return y * PITCH + x
cands = {"linear": linear, "tile16x16": tile16}
for gname, g in (("std", gob_std), ("rows", gob_rows), ("16x2lin", gob_16x2_linear)):
    for bh in (1, 2, 4, 8, 16, 32):
        cands[f"bl-{gname}-bh{bh}"] = bl(g, bh)
for path in sys.argv[2:]:
    s = open(path, 'rb').read()
    res = []
    for name, f in cands.items():
        ok = 0
        for y in range(0, H, 3):
            for x in range(0, W, 3):
                o = f(x, y)
                if o < len(s) and s[o] == ref[y * W + x]: ok += 1
        res.append((ok / ((H // 3 + 1) * (W // 3 + 1)), name))
    res.sort(reverse=True)
    print(path.split('/')[-1], " ".join(f"{n}={v:.3f}" for v, n in res[:4]))
