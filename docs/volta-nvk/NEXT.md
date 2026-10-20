# What to try next (ranked)

## Where the gap is (2026-10-20)

Token generation, Q4_0: NVK about 19.2 ms/token vs CUDA 16.0.
Prompt processing, Q4_0 pp512: NVK about 649 ms vs CUDA 481.

| Op | NVK | CUDA | Notes |
|---|---|---|---|
| FFN gate+up mat-vec (Q4_0, fused) | 357 µs | 275 µs | about 36 GB/s vs 44–47 GB/s |
| Other Q4_0 mat-vec (down, Q/K/V/O) | about 36 GB/s | about 45 GB/s | same kernel efficiency issue |
| Q6_K lm_head | 1.58 ms | 1.15 ms | |
| flash attention (tg) | 43 µs | 26 µs | |
| rms_norm (tg) | 15 µs | 8 µs | |
| pp GEMM | about 2 TFLOPS | about 5 TFLOPS | cuBLAS cutlass_70 h884 128x128 |
| pp flash attention | 110 ms | 84 ms | |
| pp rms_norm + rope | about 2x CUDA | | |

The CUDA mat-vec kernels are plain: 32-bit `LDG.E.CONSTANT` loads, IDP.4A,
128 threads/row, 39 regs, 68% occupancy. The difference is in code
generation and launch shape, not exotic instructions.

## Plan

1. **NAK: fold constant offsets into global load addresses.** Compare the
   NAK dump of mul_mat_vecq (Q4_0) against CUDA SASS
   (`out/perf/cuda-l4t-20261019`). Teach NAK to use `[R.64 + imm24]` for
   loads whose address is base + constant. Should cut ALU work and
   registers in every mat-vec. Measure lm_head and gate/up µs.
2. **MMVQ instruction count per byte.** Count instructions per 32-weight
   block in NAK output vs SASS: scale loads, nibble unpack, IDP.4A and
   address math. Look for 64-bit math, redundant conversions, and missed
   `PRMT`/`LOP3` folding.
3. **Q6_K lm_head (1.58 vs 1.15 ms).** Check rows per workgroup and the q6_K
   mat-vec unpack under the 4-subgroup layout. Try 1 row per subgroup.
4. **Small ops in tg:** fuse rms_norm into the following MMVQ quantize
   (q8_1) step, or cut workgroup overhead the way MMVQ did. FA with KV <
   256: one subgroup per head, no smem reduce.
5. **pp GEMM (largest pp gap).** Profile `mul_mm_volta_f16` with
   gemmbench (`recovered/.../gemmbench`): smem bank conflicts, ldmatrix-like
   access, stall reasons. Compare SASS of cutlass_70_tensorop_h884gemm
   128x128. Consider 256x128 tiles and deeper K prefetch.
6. **pp flash attention and rope/rms_norm:** apply the subgroup-reduction
   approach; check FA coopmat path occupancy.
7. **Upstreaming:** once freedesktop permission arrives, push
   `nak-idp4a-imm`, then the control-flow stall fix, then the rest (see
   UPSTREAM.md).

## Method reminders

- Same clocks for every comparison: GPU 1109 MHz, EMC 1866 MHz.
- Run tests before benchmarks: `MUL_MAT_VEC_FUSION`, filtered MUL_MAT n=1..8,
  and the full MUL_MAT suite before pushing anything that touches GEMM.
- Every long board job goes through `tools/jobs/jobs.sh launch` + `wait`
  with a deadline.
