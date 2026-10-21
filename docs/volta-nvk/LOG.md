# Activity log

Append a dated entry after each pass. Say what was measured, what changed,
where it was committed, and what failed.

## Before 2026-10-19 (earlier sessions; project directory lost, reconstructed)

- Mesa `volta-cmat-int8`: Volta tensor cores for VK_KHR_cooperative_matrix
  (HMMA.884 f16/f32, IMMA.8816 int8), f16 accumulators, HMMA STEP pairing,
  RA phi placement, IDP.4A immediate fix, control-flow stall fix, NIR SSBO
  bounds-check vectorization. See WORKED.md.
- llama.cpp: int8 coopmat MMQ, MMVQ for all types on Volta, f16-dequant +
  HMMA GEMM for prompt processing, rms_norm subgroupAdd, coalesced dequant.
  pp512 went from 466/395 to 778/768 (Q4_0/Q8_0).
- Abandoned around 2026-10-17, results not recorded: multi-row flash
  attention, a GEMM that dequantizes while staging, and NAK scoreboard
  policy experiments. Diffs are in
  `lineage/recovered/llama-volta/jetson-root-notes.tgz`.
- The llama.cpp commits existed only on the board (`/root/llama.cpp`) and
  on stealth (an older copy). They were never pushed.

## 2026-10-19

- Recovered the project state from nx-linux `/root`, stealth, and GitHub.
  Backups are in `lineage/recovered/llama-volta/` (mbox of the 5 commits,
  notes tarball, iq1m-check source).
- Reauthored every `bringup <bringup@local>` commit as
  `bslette <bslettela@gmail.com>`. Force-pushed 26 branches across 12 forks,
  each push leased to the old SHA. Trees, messages and dates were verified
  identical. Pushed llama.cpp `volta-nvk` for the first time.
- L4T for the CUDA comparison:
  - kexec from 6.18 failed three times: `kexec -l` ignored the DTB (use
    `kexec -c`), then a crash in cbb-noc, then a hang after sdhci.
    Each reset burned a slot-b retry, and the board fell back to Android.
    Recovered with `setActiveBootSlot(1)` from the Android root console.
  - Replaced kexec with `tools/slotb/flash-slotb.sh` (`l4t` | `linux618` |
    `restore`). cboot ignores `boot_b`'s cmdline, so `root=` goes in
    `vendor_boot_b`. L4T R35.6.4 (5.10.216) boots from NVMe p3.
- CUDA profiling under L4T (llama-bench, nsys, ncu, cuobjdump/nvdisasm).
  Data is in `lineage/out/perf/cuda-l4t-20261019/`. Two jobs silently never
  ran (an ssh password prompt, and `sudo -n` without a tty). That led to
  `tools/jobs/jobs.sh` (launch with a start check, deadline-bound waits).
- Back on 6.18 (`flash-slotb.sh linux618`). NVK per-op profile is in
  `lineage/out/perf/nvk-618-20261019/`. The gap breakdown is in NEXT.md.

## 2026-10-20

- MMVQ was bound by per-workgroup overhead, not memory: NVK CPY reaches
  42–46 GB/s while MMVQ got 28–33 GB/s. At k=2048, 128 threads do only 2
  iterations each, so the prologue and the shared-memory reduce plus barrier
  dominate.
- llama.cpp `ad1c0b8`: four subgroups per workgroup, each on its own rows,
  with no shared memory or barrier (NVK + GV1xx only).
  tg Q4_0 42.1 → 51.7, Q8_0 35.6 → 37.9, IQ4_XS +66%, K-quants +14–17%.
- Mesa `b1f2df0`: constant (read-only) global loads leave the memory
  side-effect chain, and the global-load latency estimate goes from 32 to
  200. Q6_K lm_head 2.10 → 1.58 ms, about +3% tg.
- Pushed both commits.
- llama.cpp `854e05d`: fused MUL_MAT(up) + MUL_MAT(gate) + SWIGLU into one
  MMVQ dispatch. Gives +2% on K-quants and nothing on Q4_0. Enabled for 4/5-bit
  legacy quants and K-quants only. Tests pass: MUL_MAT_VEC_FUSION 1265/1265,
  MUL_MAT n=1..8 1505/1505. Pushed.
- Status line configured (context / 5-hour / weekly percentages).
- Wrote these notes (docs/nvk-perf) and committed them to `volta-nvk`.

## 2026-10-20 (later): tensor-core usage audit

- Counted HMMA/IMMA in NAK dumps of every shader used in a Q4_0 pp512 run
  (`/var/tmp/fadump.sh`), and compared with the CUDA nsys kernel list.
- pp weight GEMMs: both stacks use HMMA.884 with f16 accumulators (cuBLAS
  cutlass_70 h884 and volta_h884gemm_64x64, against NVK mul_mm_volta_f16).
  The 2 vs 5 TFLOPS gap is kernel quality, not tensor-core use.
- pp flash attention: NVK uses tensor cores (coopmat1, 80 HMMA) and CUDA
  does not (`flash_attn_tile`), yet NVK is slower (110 vs 84 ms).
  CUDA skips its MMA FA on Xavier because `volta_mma_available` checks
  arch == 700 and sm_72 reports 720 (see UPSTREAM.md).
- tg: neither stack uses tensor cores (IDP.4A mat-vec, scalar/tile FA).

## 2026-10-20 (later): does CUDA use cache/prefetch tricks?

- CUDA Q4_0 mat-vec SASS (`mul_mat_vec_q<type 2>`, sm_72): no prefetch or
  cache-control instructions (no CCTL, no eviction hints). Only plain
  `LDG.E.CONSTANT.SYS` (32-bit) and `LDG.E.U16.CONSTANT.SYS` (scales), the
  same load form NAK emits. Weights are read in place in GGUF block layout.
- What it does do: the loop is unrolled 2x and issues all ~20 loads for
  both blocks before the first IDP.4A, addressing them off a few base
  registers with immediate offsets (`[R4-0x486]`, `[R8+0x14]`). That is
  memory-level parallelism, not cache placement.
- The "next tile waiting" pattern exists only in the pp GEMMs, in registers
  and double-buffered shared memory (CUTLASS design). NVK's
  mul_mm_volta_f16 does the same.
- Unchecked difference: the GPU page cacheability/L2 attributes of NVK
  buffers in Tegra system memory vs CUDA allocations (see NEXT.md).

## 2026-10-20 (later): NAK immediate-offset folding experiment (negative)

- Confirmed: NAK computes a 32-bit offset per load, then a 64-bit add
  (iadd3 + iadd3.x) with the buffer base, and loads at [addr+0]. The
  constants (block member offsets like +2, +0x10) sit inside the 32-bit
  offset, under u2u64. NIR can't move them out because GLSL integer math may
  wrap (Mesa's TODO in nak_nir.c says the same).
- Experiment-only pass `NAK_UNSAFE_ADDR_FOLD` (no wrap check): walks the
  iadd tree under u2u64 and moves constants into the LDG immediate. The diff
  is in `experiments/nak-unsafe-addr-fold-experiment.diff`; not committed.
  - First version (top-level iadd only): 4 of 121 loads folded, no change.
  - Tree walk: 78/121 loads folded, MMVQ shader −8% instructions,
    iadd3.x 126 → 87.
  - Result: tg Q4_0 52.26 → 51.79, Q8_0 37.83 → 38.07. Plain Q4_0 mat-vec
    75.3 → 72.5 µs, fused gate/up 355.3 → 353.7 µs, Q6_K lm_head 1585 →
    1859 µs (worse).
- Conclusion: the mat-vec kernel is not issue/ALU bound; address math is
  not the gap. Remaining suspects: memory-level parallelism per warp (CUDA
  issues every load of 2 blocks first, at 39 regs / 68% occupancy) and DRAM
  locality (NVK: about 768 concurrent 2-row streams in 144 B pieces; CUDA:
  one row per 128-thread CTA, the whole row in one pass).
- Pivoting to the pp GEMM (largest absolute gap).

## 2026-10-20 (later): pp GEMM investigation → GPU L2 does not cache NVK buffers

- cuBLAS `cutlass_70_tensorop_h884gemm_128x128_tn_align8` (ncu, 2048x512x2048):
  855 µs = 5.0 TFLOPS, 128 threads (4 warps x 64x64), 168 regs, 32 KB smem,
  18.75% occupancy, L2 hit 50.6%, L2 throughput 71.5%, issue slots 29%.
- NVK `mul_mm_volta_f16`: 128 GPRs x 256 threads (2 CTAs/SM). The loop is
  lean (per warp per K tile: 64 HMMA, 24 LDS.128, 16 STS.32, 4 LDG.128, 1
  bar). HMMA steps are 2 + NOP 2 = 4 cycles apart, which is peak rate.
- gemmbench sweep of the lost session's variants (current Mesa): all 2.3–2.55
  TFLOPS: 64x64 warp tiles (w64, 192 GPRs), pitch 8/16/24, f16 vs f32 acc,
  single buffer. New: barrier without memory semantics (no membar) 2.56,
  uvec4 smem stores 2.54. No change.
- Ablation (gemm_h2/gemm_ab, 5632x512x2048): full 4.7–5.3 ms; no MMA 4.4;
  no MMA + no fragment loads 4.1; global loads only 4.07; smem stores +
  barrier only 0.94; empty loop 0.75. **The global loads alone take the
  whole kernel time**: 184 MB of tile traffic at about 45 GB/s.
- Tiled A/B layout (each 128x32 tile contiguous): +9–12% (2.54 → 2.77).
- TLB probe (2048 rows x 64 B, stride 128 B..256 KiB): flat, so no TLB effect.
- L2 probe with L1-bypassing (`ld.strong.gpu`) coalesced loads: **58.5 GB/s
  for every window from 64 KiB to 64 MiB**, which is DRAM speed. With
  `.constant` (L1-cached) loads, small windows read at 425 GB/s. So the GPU
  L2 does not cache NVK buffers; every L1 miss goes to DRAM. This explains
  the GEMM (no L2 reuse of A/B tiles) and probably hurts FA and others.
- Suspect: nouveau maps host (Tegra system) memory with the PTE VOL bit
  (`vol = target == NVKM_MEM_TARGET_HOST` in gf100_vmm_valid for unversioned
  map args, as VM_BIND uses), which makes the L2 treat it as uncached.
  Probes are in `experiments/` (l2c.comp, tlb.comp, mkab.py, mktiled.py).
- Confirmed in nouveau (`nouveau_uvmm_vmm_map`): VM_BIND sets `vol = 1` for
  every non-VRAM BO. The board's (lost-source) nouveau.ko has a module
  parameter `uvmm_sysmem_vol` (default 1, rw at runtime) to change it.
- vol=0 alone: +11–13% pp/tg but wrong results (MUL_MAT failures, PPL
  45790), because nobody flushes/invalidates the L2 for the CPU.
- Mesa `972938f` (local, volta-cmat-int8): nvkmd nouveau exec ctx wraps
  every DRM_NOUVEAU_EXEC with L2_FLUSH_DIRTY + L2_SYSMEM_INVALIDATE before
  and WFI + L2_FLUSH_DIRTY after, when uvmm_sysmem_vol reads 0
  (NVK_L2_SYSMEM_MAINT overrides). Result with vol=0:
  pp512 Q4_0 782 → 879, Q8_0 770 → 868, Q4_K_M 684 → 758;
  tg128 Q4_0 52.0 → 57.9, Q8_0 37.9 → 42.0 (CUDA 40.0), Q4_K_M 43.1 → 44.9.
  MUL_MAT n=1..8 1505/1505, whole tiles 40/40, PPL 26.4037 (identical).
  L2 probe 64 KiB window: 58.5 → 138 GB/s. GEMM 2.55 → 2.94 TFLOPS.
- To use: `echo 0 > /sys/module/nouveau/parameters/uvmm_sysmem_vol` after
  `modprobe nouveau` (or `modprobe nouveau uvmm_sysmem_vol=0`), before
  starting Vulkan apps (it only affects new mappings).
- Broad correctness with vol=0 + `972938f`: FLASH_ATTN_EXT 5332/5332,
  MUL_MAT (full) 2339/2339, MUL_MAT_ID 939/939, MUL_MAT_VEC_FUSION 1265/1265,
  RMS_NORM 53, ROPE 470, SOFT_MAX 214, GET_ROWS 240, SET_ROWS 319, CPY 254,
  ADD 105, MUL 94: all pass. (The GLU filter matched 0 tests, so that op
  wasn't run.) Pushed `972938f`. Board `/root/nvkenv.sh`
  now sets uvmm_sysmem_vol=0 when sourced.

## 2026-10-20 (later): nouveau uvmm_sysmem_vol recreated and pushed

- Not upstream: mainline `nouveau_uvmm_vmm_map()` still hardcodes vol = 1
  for non-VRAM; no parameter or Tegra exception exists.
- The board's 6.18 nouveau source was found as stealth's (work-tree-less)
  repo `kernel/platform/kernel-6.18/vendor/nvidia/kernel.git`, branch
  `local-rey-nouveau-gv11b-6.18-v68` = GitHub `rey-nouveau-gv11b-6.18`
  (977d70d). It did not contain uvmm_sysmem_vol (that was an uncommitted
  bazzite edit).
- Recreated as `e7a8ffd` "drm/nouveau/uvmm: add uvmm_sysmem_vol ..." on top
  of 977d70d; pushed to bslette/android_kernel_nvidia_kernel branch
  `rey-nouveau-gv11b-6.18-l2`. Same name, description, default and perms.
- Built on stealth (`~/nouveau-l2/build-nouveau.sh`: board /proc/config.gz,
  clang r536225 from the 23.2 tree, prebuilt flex/bison/m4); vermagic
  `6.18.21-nx+` matches. Loaded on the board in place of the old module:
  vol=0 MUL_MAT n=1..8 1505/1505, FLASH_ATTN_EXT 5332/5332, pp512/tg128
  Q4_0 871.7/57.7, PPL 26.4037. The installed /lib/modules nouveau.ko is
  unchanged (the rebuilt one is /var/tmp/nouveau-l2.ko).

## 2026-10-20 (later): old experiments re-tested with the L2 on (vol=0)

gemmbench, 5632x512x2048 (TFLOPS, was → now): hc 2.54 → 2.93, w64 2.45 → 2.93,
nb 2.56 → 2.97, nbv4 2.56 → 2.98, tiled A+B 2.77 → **3.13**, p0 1.83 → 1.94,
f32-acc gemm 1.38 → 1.41. Everything gained about 15%; no variant pulled
ahead. Ablation (gemm_ab): full 4.77, no MMA 3.99, no fragment loads 3.01,
global loads only 2.12 (was 4.07), smem stores + bar 0.85, empty 0.61 ms.
The remaining GEMM time is now split between global loads (~2.1 ms),
fragment LDS (~1.8 ms) and MMA (~0.8 ms).

llama-bench pp512 / tg128 with temporary knobs (shader cache off):

| config | Q4_0 | Q8_0 | Q4_K_M | IQ4_XS |
|---|---|---|---|---|
| base | 874 / 57.5 | 860 / 41.8 | 758 / 44.9 | 834 / 51.7 |
| GLU fusion for all types | 876 / 57.7 | 866 / 41.0 | 758 / 44.9 | 833 / 51.5 |
| GLU fusion off | 877 / 58.1 | 866 / 42.0 | 756 / 42.0 | 834 / 51.8 |
| old MMVQ layout (row groups 1) | 878 / 44.2 | 867 / 37.1 | 755 / 37.0 | 830 / 34.7 |
| 1 row per subgroup | 879 / 56.2 | 866 / 39.0 | 758 / 44.8 | 825 / 47.0 |
| 4 rows per subgroup | 877 / 55.6 | 866 / 39.2 | 758 / 44.8 | 830 / 50.6 |
| NAK_UNSAFE_ADDR_FOLD | 876 / 58.0 | 868 / 42.3 | 756 / **49.8** | 831 / **54.1** |
| FA_SCALAR forced | 768 / **59.3** | 759 / **42.7** | 675 / 45.7 | 734 / 52.9 |
| GGML_VK_DISABLE_MMVQ | 878 / 37.3 | 865 / 35.0 | 757 / 34.9 | 830 / 26.2 |
| GGML_VK_DISABLE_VOLTA_GEMM | 581 / 57.6 | 463 / 41.8 | 324 / 44.8 | 534 / 51.6 |

- **Address folding now pays**: +11% tg Q4_K_M, +5% IQ4_XS (was +0% with
  the L2 off). With DRAM no longer the limit, instruction count matters.
  Needs a correct (non-wrapping) implementation.
- **Scalar FA for tg**: +2–3% tg on all types (tg FA has 8 GQA rows, so it
  took the coopmat path); −12% pp. Make the choice by row count.
- Everything else confirms the current settings.
- Lost-session attempts with the L2 on: fused-dequant Volta GEMM Q4_0 pp512
  878 → 904 (+3%), Q8_0 868 → 791 (−9%); multi-row coopmat FA (BR 32/64)
  880 → 814/769 (worse; correct, 5332/5332). Address fold + scalar FA
  together, tg128: Q4_0 57.7 → 59.4, Q8_0 41.9 → 43.1, Q4_K_M 44.8 → 50.9,
  IQ4_XS 51.7 → 55.6.
- llama.cpp `7553d5c`: fused dequant for Q4_0 only. The first version broke
  split-K launches (Q4_0 128x384x2048: the regular GEMM read raw q4_0);
  fixed by dequantizing first when split_k > 1. MUL_MAT 2339/2339,
  MUL_MAT_ID 939/939.
- llama.cpp `e4e8abd`: scalar FA for ≤ 8 rows on NVK Volta (tg with GQA).
  FLASH_ATTN_EXT 5332/5332, PPL 26.4037.
- Now: pp512/tg128 Q4_0 902/58.1, Q8_0 860/42.2 (CUDA 1030/62.8, 1134/40.0).
- Address fold: a correct version needs a design decision. Options:
  (1) NVK opt-in "offsets don't wrap" flag; (2) llama.cpp K-quant loads via
  buffer_reference 64-bit pointers; (3) NIR nuw on SSBO access-chain
  offsets. See NEXT.md.

## 2026-10-20 (later): NAK_DEBUG=nowrap (option 1) to size option 2

Mesa `df97444`: the address fold as an opt-in NAK debug flag (`NAK_DEBUG=nowrap`,
part of the NVK shader-cache key). pp512 / tg128, shader cache off:

| model | off | nowrap | tg change |
|---|---|---|---|
| Q4_0 | 903 / 58.14 | 910 / 58.44 | +0.5% |
| Q8_0 | 864 / 42.22 | 867 / 42.54 | +0.8% |
| Q4_1 | 874 / 57.29 | 879 / 55.28 | **−3.5%** |
| Q4_K_M | 759 / 45.08 | 757 / 50.17 | **+11.3%** |
| Q5_K_M | 690 / 38.87 | 681 / 46.47 | **+19.6%** |
| IQ4_XS | 833 / 52.10 | 835 / 54.59 | +4.8% |

Per op (tg, µs per call, off → nowrap):

- q4_K mat-vec: 5632×2048 207 → 164, GLU 359 → 291, 2048×2048 84 → 69, MUL_MAT_ADD k=5632 202 → 164 (−19 to −21%)
- q5_K mat-vec: GLU 476 → 345, 2048×2048 102 → 79, k=5632 253 → 185 (−23 to −28%)
- iq4_xs mat-vec: −4 to −12%
- q4_0: −2%
- **q6_K mat-vec gets slower**: lm_head 1454 → 1682 µs (+15%), k=5632 344 → 386 (+12%)

Correctness with nowrap: MUL_MAT n≤8 1536/1536, MUL_MAT_ID n≤8 434/434,
FLASH_ATTN_EXT 5332/5332, GET_ROWS, RMS_NORM_MUL_ADD, SOFT_MAX, ROPE all
pass; PPL Q4_0 26.4037 and Q4_K_M 25.9262, identical to off.

Conclusion: the gain is concentrated in q4_K/q5_K (and iq4_xs) mat-vec, and
a blanket fold hurts q6_K and q4_1. A per-shader fix (option 2: 64-bit
buffer_reference addressing in those MMVQ paths only) is worth doing; it
would also avoid the q6_K regression, which costs Q4_K_M/Q5_K_M about
0.65 ms/token (lm_head + q6_K down projections).

## 2026-10-20 (later): option 2 as vector loads, llama.cpp `39da5ea`

NAK dump of the q4_K MMVQ shader: 181 `ld.global...b32` for 60 dot products.
Every load is scalar because the vectorizer can't prove 16-byte alignment
(the index goes through `iqs_k % 8`), so each word paid its own 64-bit
address add. Rather than buffer_reference pointers, read through vector
views (q4_K/q5_K `packed128`, iq4_xs 8-byte, q8_1_x4 `ivec4`). No wrap
assumption needed, and it removes the loads as well as the address adds.

q4_K shader: 181 → 58 loads (41 × b128). tg128 (pp512 unchanged):

| model | before | vectors | vectors + nowrap |
|---|---|---|---|
| Q4_K_M | 45.1 | **52.7** | 51.3 |
| Q5_K_M | 38.9 | **48.7** | 48.0 |
| IQ4_XS | 52.1 | **57.1** | 57.2 |
| Q4_0 | 58.1 | 58.3 | 58.2 |
| Q4_1 | 57.3 | 57.3 | 55.3 |
| Q8_0 | 42.2 | 42.2 (40.9 with an ivec2 B load) | |

Beats the blanket nowrap fold everywhere, and nowrap on top now hurts
(K-quants) so it stays off. The ivec2 q8_1 load for K_PER_ITER 8 (Q8_0)
cost 3% and was dropped. Per op: q4_K 5632×2048 207 → 158 µs, q5_K GLU
476 → 329 µs, iq4_xs 5632×2048 160 → 149 µs; q6_K unaffected (it doesn't
use MMVQ here). Tests: MUL_MAT n≤8 1536/1536, MUL_MAT_ID 434/434,
MUL_MAT_VEC_FUSION 1265/1265; PPL Q4_K_M 25.9262 (same), IQ4_XS 26.4761,
Q8_0 25.2937.

## 2026-10-20 (later): the other MMVQ types

q6_K (210-byte blocks, 2-byte aligned) can't use vector loads. Measured
first (Q4_K_M per call, lm_head 32000×2048 / down 2048×5632):

| q6_K path | lm_head | down | Q4_K_M tg |
|---|---|---|---|
| dequant mat-vec (before) | 1455 | 344 | 52.7 |
| MMVQ, 2-byte loads | 1554 | 303 | 53.8 |
| MMVQ + nowrap | 1260 | 332 | 54.2 |
| MMVQ, aligned 8-byte window loads (SSBO) | 1665 | 310 | 53.2 |
| window loads + nowrap | 1401 | 266 | 55.6 |
| **window loads through 64-bit pointers (`42d877e`)** | **1380** | **263** | **55.6** |

The cost is the 64-bit address add per load, not the load count; only
64-bit pointers (buffer_reference, A's address in push constants) let NAK
fold the constant offsets legally. tg128: Q4_K_M 52.7 → 55.6, Q5_K_M
48.7 → 51.3, Q4_0 58.3 → 58.4, IQ4_XS 57.1 → 57.3. PPL Q4_K_M unchanged.

Types without a model on the board, `test-backend-ops perf` MUL_MAT
4096×14336 n=1 (µs; nowrap = upper bound for folding):

| type | before | nowrap | now | change |
|---|---|---|---|---|
| q2_K | 780 | 676 | 689 | `8f35297` (pointers, 4-byte loads) |
| q3_K | 1951 | 1578 | 1576 | `7659de7` (pointers + window loads) |
| iq1_m | 1231 | 958 | 963 | `8f35297` (scales as one 8-byte load, no pointers) |
| iq1_s | 768 | 729 | 763 | not done (−4% max) |
| q5_0 | 1311 | 1258 | 1309 | not done (−3% max) |
| q4_0, q5_1, q8_0 | 917, 1338, 1708 | same | same | nothing to gain |

q2_K with 8-byte window loads gained nothing (790): its blocks are 4-byte
aligned and the extra realign ALU ate the gain; plain 4-byte loads through
pointers got the full fold.

Process note: two k23 runs benchmarked a stale binary because the job
ignored shader compile errors; jobs now stop on BUILD-FAILED.

## 2026-10-20 (later): cross-GPU validation of the MMVQ changes

Builds: base `7a5733b` (before the MMVQ load work), head `a8f39f5`, exp =
head + local env knobs `GGML_VK_MMVQ_BDA` (A_BDA variants on any device) and
`GGML_VK_MMVQ_Q6` (q6_K through MMVQ on any device); knobs not committed.
Built once on stealth in an ubuntu:24.04 container with the LunarG SDK glslc
(Ubuntu 24.04's glslc lacks GL_EXT_integer_dot_product, so it builds no MMVQ
shaders at all), no AVX2. Scripts: stealth `~/xval/` (xval-build.sh,
xval-run.sh, xval-perf.sh), models in `~/xval/tl` on every host.

| host | GPU | driver | int dot |
|---|---|---|---|
| stealth | RX 7800 XT (RDNA3) | RADV, Mesa 26.0.8 | yes |
| aimini | RTX 3050 (GA107) | NVK, Mesa 26.0.8 distro / `~/nvk-check/main-fixed` | yes |
| oldcrustylx | RX 5700 XT (RDNA1) | RADV | no (MMVQ never used) |
| bazzite | RTX 4060 Laptop (AD107) | NVIDIA 615.78 | yes |

Correctness (MUL_MAT n≤8 1536, MUL_MAT_ID n≤8 434, MUL_MAT_VEC_FUSION 1265),
head and exp (also with GGML_VK_FORCE_MMVQ): all pass on all four. On
aimini the distro NVK loses the device in MUL_MAT for base and head alike
(iq1_m, the NAK IDP.4A-immediate bug, Mesa `6656192`); with the fixed NVK
build everything passes.

`test-backend-ops perf` MUL_MAT 4096×14336, µs, base → head (exp where it
differs):

| type | RTX 4060 (NVIDIA) n=1 / n=4 | RTX 3050 (NVK) n=4, 3 runs | RX 7800 XT n=1 / n=4, 3 runs |
|---|---|---|---|
| q4_K | 78 → 62 / 191 → 114 | 830–873 → 603–624 | ±2% |
| q5_K | 165 → 165 / 233 → 172 | 760–860 → 639–693 | +1–2% / 0 |
| iq4_xs | 109 → 90 / 211 → 133 | noisy, ≈ −10% | −1.5% |
| q3_K | 118 → 102 / 226 → 153 | 708–744 → 522–650 | ±1% |
| q2_K | 50 → 48 / 141 → 115 | ≈ −10% | ±1% |
| iq1_m | 70 → 70 / 239 → 230 | 426 → 400 / 999 → 980 | **+3%** (63.4 → 65.7) |
| q6_K exp (BDA + MMVQ) | n=4 247 → 202 | n=1 −17%, n=4 +0–6% | **n=1 +20%** |
| q2_K/q3_K exp (BDA) | ≈ 0 | mixed | **n=1 +10–24%** |

RX 5700 XT: all within noise (no int dot, so no MMVQ). TinyLlama llama-bench:
no change beyond noise on any of them (k = 2048 mostly takes the DMMV path
on these vendors).

Conclusions:
- Vector loads (`39da5ea`) are a large win on both NVIDIA drivers (q4_K
  −21–40%, iq4_xs −18–37%, q3_K −14–32% from the q8_1 ivec4 loads) and
  neutral on RDNA3: fine to upstream ungated.
- iq1_m scale load (`8f35297`): −1–6% on NVIDIA, +3% on RDNA3. Small either
  way.
- A_BDA (gated to NVK Volta): clearly worse on RDNA3, neutral on the
  NVIDIA driver, mixed on NVK Ampere (q6_K n=1 −17%). Keep the gate; NVK on
  Turing/Ampere needs a quieter measurement before widening.
