# Make gemm_hc variants: nb (barrier without memory semantics), v4 (uvec4 shared), nbv4 (both)
src = open('/root/gemmbench/gemm_hc.comp').read()
nb = src.replace('\t\tbarrier();', '\t\tcontrolBarrier(gl_ScopeWorkgroup, gl_ScopeWorkgroup, 0, 0);')
assert nb != src
def v4(s):
    s = s.replace('shared float16_t sa[2][BM * PITCH];\nshared float16_t sb[2][BN * PITCH];',
                  'const uint PV = PITCH / 8;                 // uvec4 per shared row\nshared uvec4 sa[2][BM * PV];\nshared uvec4 sb[2][BN * PV];')
    i0 = s.index('\t\t// registers -> shared[buf]'); i1 = s.index('\t\tbarrier();') if '\t\tbarrier();' in s else s.index('\t\tcontrolBarrier(')
    s = s[:i0] + '''\t\t// registers -> shared[buf]
\t\t[[unroll]] for (uint i = 0; i < 2; i++) {
\t\t\tsa[buf][(lr + 64 * i) * PV + lc] = ra[i];
\t\t\tsb[buf][(lr + 64 * i) * PV + lc] = rb[i];
\t\t}
''' + s[i1:]
    s = s.replace('(wm * 64 + i * 16) * PITCH + ks, PITCH,', '(wm * 64 + i * 16) * PV + ks / 8, PV,')
    s = s.replace('(wn * 32 + j * 16) * PITCH + ks, PITCH,', '(wn * 32 + j * 16) * PV + ks / 8, PV,')
    return s
open('/root/gemmbench/gemm_nb.comp', 'w').write(nb)
open('/root/gemmbench/gemm_v4.comp', 'w').write(v4(src))
open('/root/gemmbench/gemm_nbv4.comp', 'w').write(v4(nb))
print('ok')
