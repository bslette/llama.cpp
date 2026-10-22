# cmp.py out.yuv ref.yuv W H: per-frame comparison of two I420 files
import sys, math
a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
w, h = int(sys.argv[3]), int(sys.argv[4])
fs = w * h * 3 // 2
n = min(len(a), len(b)) // fs
print(f"frames: out {len(a)//fs}, ref {len(b)//fs}")
exact = 0
for i in range(n):
    fa, fb = a[i*fs:(i+1)*fs], b[i*fs:(i+1)*fs]
    if fa == fb:
        exact += 1
        continue
    planes = []
    for name, off, size in (("Y", 0, w*h), ("U", w*h, w*h//4), ("V", w*h*5//4, w*h//4)):
        pa, pb = fa[off:off+size], fb[off:off+size]
        diff = sum(1 for x, y in zip(pa, pb) if x != y)
        mse = sum((x - y) ** 2 for x, y in zip(pa, pb)) / size
        psnr = 99 if mse == 0 else 10 * math.log10(255 * 255 / mse)
        planes.append(f"{name} {diff} px differ, PSNR {psnr:.1f}")
    print(f"frame {i}: " + "; ".join(planes))
print(f"{exact}/{n} frames bit-exact")
