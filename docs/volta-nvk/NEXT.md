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

00. **Done 2026-10-20: MMVQ vector loads** (llama.cpp `39da5ea`): Q4_K_M tg
    45.1 → 52.7, Q5_K_M 38.9 → 48.7, IQ4_XS 52.1 → 57.1. Follow-ups:
    - Same treatment for the other MMVQ types: q2_K/q3_K/q6_K (16-bit
      loads now), Q4_0/Q4_1/Q5_x (QUANT_R 2 reads idx and idx+4 from B;
      restructure so they become one vector), iq1/iq2/iq3.
    - q6_K lm_head (1.45 ms) uses the non-MMVQ path; try MMVQ q6_K with
      vector loads.
    - Dequant and other mat-vec shaders: check NAK dumps for scalar loads.
    - `NAK_DEBUG=nowrap` (`df97444`) is now mostly redundant; nowrap on top
      of vector loads is slightly slower.

0. **Done 2026-10-20: GPU L2 caching for NVK on Tegra** (uvmm_sysmem_vol=0 +
   Mesa `972938f`): +12% pp, +11% tg. Follow-ups:
   - Recreate the nouveau 6.18 gv11b kernel source (lost with the bazzite
     build dir); make vol=0 the default for Tegra system memory upstream-style.
   - Re-profile every op with L2 caching on; the per-op gap table below is
     from before.
   - GEMM is still 2.94 vs 5.0 TFLOPS: with L2 on, re-run the ablation
     (global loads vs fragment loads) and try the tiled A/B layout again
     (+9–12% before).

1. ~~**NAK: fold constant offsets into global load addresses.**~~ Tested
   2026-10-20 with an unsafe experiment pass: −8% instructions, no tg gain
   (see LOG). For mat-vec, look at memory-level parallelism and DRAM
   locality instead of instruction count.
2. ~~**MMVQ instruction count per byte.**~~ Superseded by item 1's result.
3. **Q6_K lm_head (1.58 vs 1.15 ms).** Check rows per workgroup and the q6_K
   mat-vec unpack under the 4-subgroup layout. Try 1 row per subgroup.
4. **Small ops in tg:** fuse rms_norm into the following MMVQ quantize
   (q8_1) step, or cut workgroup overhead the way MMVQ did. FA with KV <
   256: one subgroup per head, no smem reduce.
5. **pp GEMM (largest pp gap).** Profile `mul_mm_volta_f16` with
   gemmbench (`recovered/.../gemmbench`): smem bank conflicts, ldmatrix-like
   access, stall reasons. Compare SASS of cutlass_70_tensorop_h884gemm
   128x128. Consider 256x128 tiles and deeper K prefetch.
6. **pp flash attention and rope/rms_norm.** NVK's pp FA already uses tensor
   cores (coopmat1, 80 HMMA), while CUDA's uses none (`flash_attn_tile`),
   yet NVK is slower (110 vs 84 ms). First try forcing `FA_SCALAR` for pp on
   NVK Volta. Then check coopmat FA occupancy and smem use. Apply the
   subgroup-reduction approach to rope and rms_norm.
7. ~~**Check memory attributes of NVK buffers on Tegra.**~~ Done: L2 was uncached (item 0).
8. **Upstreaming:** once freedesktop permission arrives, push
   `nak-idp4a-imm`, then the control-flow stall fix, then the rest (see
   UPSTREAM.md).

## Method reminders

- Same clocks for every comparison: GPU 1109 MHz, EMC 1866 MHz.
- Run tests before benchmarks: `MUL_MAT_VEC_FUSION`, filtered MUL_MAT n=1..8,
  and the full MUL_MAT suite before pushing anything that touches GEMM.
- Every long board job goes through `tools/jobs/jobs.sh launch` + `wait`
  with a deadline.
