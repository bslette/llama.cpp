# What to try next (ranked)

## Android (2026-10-21)

The Volta stack runs on LineageOS slot a within 1-2% of Linux, and llama-server
+ ChatterUI works (LOG 2026-10-21). drm_hwcomposer fix e7a1d91 puts the
streaming chat's layers on planes (GPU composition -94%, tg +8%) but only runs
via a tmpfs bind mount until the vendor image is rebuilt and flashed. Next:
1. Rebuild vendor with e7a1d91 (+ libggml-cpu without dotprod) and flash slot a;
   (fix pushed: bslette/android_external_drm_hwcomposer-upstream rey-24).
2. The 12 failed test commits per 300 frames: find which layout Tegra rejects.
3. In-app tg 16.5 vs 19.6 static: the app's HWUI rendering on the same GPU.
   Try GPU context priority, or a lighter client (fewer redraws per token).
4. Short-prompt latency: 32-46 tokens take 1.0-2.8 s (pp512 is 330 t/s).
5. /metadata is full.

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

## Prompt processing plan (2026-10-20)

Status 2026-10-21: Q4_0 pp512 1030 = CUDA; Q8_0 1011 vs 1134. The generic
parts (rope, rms_norm) are validated on RDNA1/3, Ada (NVK and NVIDIA driver),
Iris Xe and UHD 770 (LOG 2026-10-21 night) and listed as ready in UPSTREAM.md.

Per pp512 eval, Q4_0, same clocks: NVK 580 ms vs CUDA 497 ms (1030 t/s).
NVK from GGML_VK_PERF_LOGGER (`/root/prof/pp-now-Q4_0.log`), CUDA from
`out/perf/cuda-l4t-20261019/nsys-kern-summary.txt` (totals / 4 evals).

| work | NVK | CUDA | gap |
|---|---|---|---|
| matmuls incl. dequant/convert | 421 | 355 (GEMM 202 at 5.0 TFLOPS + dequant 100 + f32/f16 converts 43 + 64x64 gemm 9) | **+66** |
| flash attention | 86 | 84 (flash_attn_tile + combine, no tensor cores) | +2 |
| rms_norm | 20 | 10 | +10 |
| rope | 11 | 6 | +5 |
| GLU + add | 24 | 23 | +1 |

NAK dump of the pp shaders: the Volta GEMM's fragment loads are already
LDS.128 (24 per K tile for 64 HMMA, 125 regs, 64 scalar STG.32 in the
epilogue, q4_0 A staged with 16-bit LDGs). The coopmat FA shader has 128
scalar `ld.shared.u16`.

Ranked:
1. ~~64x64 tiles~~ slower (DIDNT-WORK); split-K for the k/v projections
   (8 tiles on 6 SMs) is the remaining idea, ~ -5 ms. Separate A/B staging
   loops gave -6-8% on every GEMM (`edb5d1c`).
2. rope done (`2a404d7`, 497 -> 165 µs). rms_norm (470 µs/call vs CUDA 225):
   smaller workgroups, row kept in registers. ~ -10 ms.
3. ~~Coopmat FA u16 loads~~ done in NAK (`e943159`), no gain; FA is at CUDA's
   level now. Left: mask_cache in local memory; FA multi-row tiles slower before.
4. ~~Warp tile 64x64~~ done (`2263d43`, +1.5%). GEMM is ~2.6 TFLOPS on the big
   matrices vs cuBLAS 5.0; remaining ideas below.
5. GEMM main loop (ablation in LOG 2026-10-21): fragment LDS, staging STS and
   HMMA don't overlap. Ideas: fewer registers (245 -> <= 170 for 3
   workgroups/SM: smaller staging, fragments per A row), fragment double
   buffering inside the K tile, BK 64 with fewer barriers (needs smem).
   Epilogue done (`609ff7c`).
6. Q8_0 fused dequant again with aligned-window loads (34-byte blocks).
   Q8_0 only, ~ -10 ms.

Expected: 1-3 take Q4_0 pp512 ~900 -> ~950 t/s; 4 is what passes 1030.

## Plan

00. **Done 2026-10-20: MMVQ vector loads** (llama.cpp `39da5ea`): Q4_K_M tg
    45.1 → 52.7, Q5_K_M 38.9 → 48.7, IQ4_XS 52.1 → 57.1. Follow-ups:
    - Done for the other types: q6_K `42d877e`, q3_K `7659de7`, q2_K and
      iq1_m `8f35297` (see LOG). Not worth it: q4_0/q5_1/q8_0 (no fold
      gain), iq1_s/q5_0 (≤ 4%).
    - Q4_0 tg is now limited by its 2-byte loads (18-byte blocks) and
      QUANT_R 2 B reads (idx and idx+4); restructuring the thread mapping
      so each thread reads 16 contiguous bytes of q8_1 is the next idea.
    - Dequant (non-MMVQ) mat-vec shaders and the pp dequant shaders: check
      NAK dumps for scalar loads the same way.
    - Cross-GPU check done 2026-10-20 (LOG): vector loads ready for an
      upstream PR; A_BDA stays gated (RDNA3 slower). Retest A_BDA for q6_K on
      NVK Ampere with fixed clocks (n=1 −17% but noisy).
    - Intel iGPU on bazzite (once enabled in firmware): upstream already uses
      q6_K MMVQ there; run `~/xval/xval-run.sh` with DEV set to the Intel device.

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
