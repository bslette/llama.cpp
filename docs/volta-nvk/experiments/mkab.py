# gemm_ab: gemm_h2 plus skip bit 8 = load the A/B fragments from shared memory only on the first K tile
s = open('/root/gemmbench/gemm_h2.comp').read()
s = s.replace('''		[[unroll]] for (uint ks = 0; ks < BK; ks += 16) {
			coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA> fa[4];
			coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB> fb[2];
			[[unroll]] for (uint i = 0; i < 4; i++)''', '''		[[unroll]] for (uint ks = 0; ks < BK; ks += 16) {
			if ((p.skip & 8) == 0 || kt == 0) {
			[[unroll]] for (uint i = 0; i < 4; i++)''')
s = s.replace('''				coopMatLoad(fb[j], sb[buf], (wn * 32 + j * 16) * PITCH + ks, PITCH, gl_CooperativeMatrixLayoutColumnMajor);
''', '''				coopMatLoad(fb[j], sb[buf], (wn * 32 + j * 16) * PITCH + ks, PITCH, gl_CooperativeMatrixLayoutColumnMajor);
			}
''')
s = s.replace('''	uint buf = 0;''', '''	coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA> fa[4];
	coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB> fb[2];
	uint buf = 0;''')
assert s.count('(p.skip & 8)') == 1 and s.count('MatrixUseA> fa[4]') == 1
open('/root/gemmbench/gemm_ab.comp', 'w').write(s)
print('ok')
