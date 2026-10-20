# gemm_tiled: gemm_hc reading A and B as contiguous 128x32 tiles ([M/128][K/32][128][32] f16),
# timing-only (the harness fills row-major data, so the check fails).  gemm_tiledA: only A tiled.
src = open('/root/gemmbench/gemm_hc.comp').read()
def tiled(s, a=True, b=True):
    if a:
        s = s.replace('ra[i] = a[(m0 + lr + 64 * i) * kv + lc];', 'ra[i] = a[(gl_WorkGroupID.y * (p.K / BK)) * 512 + (lr + 64 * i) * 4 + lc];')
        s = s.replace('ra[i] = a[(m0 + lr + 64 * i) * kv + kk + lc];', 'ra[i] = a[(gl_WorkGroupID.y * (p.K / BK) + kt + 1) * 512 + (lr + 64 * i) * 4 + lc];')
    if b:
        s = s.replace('rb[i] = b[(n0 + lr + 64 * i) * kv + lc];', 'rb[i] = b[(gl_WorkGroupID.x * (p.K / BK)) * 512 + (lr + 64 * i) * 4 + lc];')
        s = s.replace('rb[i] = b[(n0 + lr + 64 * i) * kv + kk + lc];', 'rb[i] = b[(gl_WorkGroupID.x * (p.K / BK) + kt + 1) * 512 + (lr + 64 * i) * 4 + lc];')
    return s
t = tiled(src); assert t.count('* 512 +') == 4, t.count('* 512 +')
open('/root/gemmbench/gemm_tiled.comp', 'w').write(t)
open('/root/gemmbench/gemm_tiledA.comp', 'w').write(tiled(src, b=False))
print('ok')
