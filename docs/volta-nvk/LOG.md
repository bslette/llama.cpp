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

## 2026-10-20 (later): prompt processing, steps 1-2

- Step 1 (64x64 tiles for small GEMMs) failed: a BM/BN-parametrized
  `mul_mm_volta_f16` with a 64x64, 2-subgroup variant made the 256x512x2048
  k/v projection slower (344 -> 474 us; all-small: pp512 Q4_0 659).
- But the parametrized shader stages A and B in separate loops, and that
  alone made every f16 GEMM call 6-8% faster (A/B against the committed
  shader: Q8_0 pp512 865 -> 912). The fused q4_0 path got 1.5% slower and is
  now behind the separate dequant + f16 GEMM (Q4_0 889 fused vs 929
  unfused), so fusion is opt-in (`GGML_VK_VOLTA_FUSED=1`). llama.cpp `edb5d1c`.
- Rope ran 1 row per 256-invocation workgroup; with 64-dim heads 224 of 256
  invocations were idle. 8 rows x 32 pairs: ROPE 497 -> 165 us (CUDA 245),
  rope+set_rows 75 -> 31 us. llama.cpp `2a404d7`.
- pp512 now: Q4_0 903 -> 940, Q8_0 865 -> 925, Q4_K_M 757 -> 805,
  IQ4_XS 833 -> 891 (CUDA Q4_0 1030, Q8_0 1134). tg unchanged. MUL_MAT,
  ROPE, ROPE_BACK, FLASH_ATTN_EXT pass; PPL unchanged.

## 2026-10-21: prompt processing, steps 2-4

- rms_norm (`e7112dc`): 128-invocation variant for >= 64 rows, row kept in
  registers. 2048x512: 471 -> 328 us (cache alone 455; 256 threads 354 but
  tg -1.3%; 128 threads for all rows tg -2.9%, hence the row-count switch).
  PPL Q4_0 26.4037 -> 26.4113, Q4_K_M 25.9262 -> 25.8961 (reduction order).
- FA (step 3): the "128 u16 shared loads" were the P·V coopmat operands, P
  column-major (A) and V row-major (B); NAK only had the mma.m8n8k4 .row.col
  fragment layout. Encodings from ptxas sm_72 (L4T CUDA 11.4 on /mnt/l4t):
  bit 73 = A column-major, bit 74 = B column-major. Mesa `e943159`: alternate
  fragment layouts for cmat variables that only see column-major A /
  row-major B loads (grouped across cmat_copy). With P/V in their own matrices
  in the FA shader (experiments/llama-fa-pmat-vmat-experiment.diff) the u16
  loads become 36 x b64, FLASH_ATTN_EXT 5332/5332, but FA time unchanged
  (3833 vs 3829 us): those loads were not FA's bottleneck. FA is at CUDA's
  level (CUDA tile + combine ~3.8 ms/call). FA keeps a 32-byte local array
  (mask_cache, loops over gl_WorkGroupSize not fully unrolled); minor.
- GEMM warp tile (`2263d43`): 2x2 subgroups of 64x64 (cuBLAS h884 shape)
  instead of 2x4 of 64x32: 252 registers, no spills; 5632x512x2048
  4678 -> 4538 us. 128x256 with 2x4 of 64x64 needs 60 KB shared memory
  (over 48 KB): garbage, dropped.
- pp512 now: Q4_0 970, Q8_0 956, Q4_K_M 825, IQ4_XS 918 (CUDA 1030 / 1134).

## 2026-10-21 (later): GEMM ablation and epilogue

Ablation of the real `mul_mm_volta_f16` (spec constant, experiments/
llama-volta-gemm-ablation.diff), 5632x512x2048 Q8_0 op time incl. dequant:
full 4649 us; no HMMA 3952; no global loads in the K loop 3830; no shared
staging stores 3736; no output stores 4315; none of HMMA/global/staging 2369
(fragment LDS + barriers + dequant + epilogue). The parts overlap poorly:
each alone is 0.7-0.9 ms, together 2.3 ms; HMMA at full rate would be about
1.7 ms of the 4.6 ms.

- Epilogue via shared memory + vec4 f32 stores (`609ff7c`): 4649 -> 4599 us,
  pp512 Q4_0 970 -> 976, Q8_0 956 -> 964.
- The coopMatStore of the f16 accumulators to shared memory was 128 scalar
  u16 stores per lane (the lane holds 8 consecutive N values, D is M-major).
  Swapping operand roles (compute D^T: activations as A) made every epilogue
  access 128-bit (40 STS.128, 64 STG.128) but changed nothing (4606 us):
  reverted. The epilogue is no longer a cost.
- HMMA issue order in NAK is fine (4 independent accumulators per k-chunk).
  245 registers -> 2 workgroups (8 warps) per SM; cuBLAS fits 3 at 168.
- pp512 now: Q4_0 976, Q8_0 964, Q4_K_M 832, IQ4_XS 923 (CUDA 1030 / 1134).
- Volta GEMM loads B fragments per K step, then each A fragment right before
  its four HMMAs (`d4418c8`): 229 registers (still 2 workgroups/SM), NAK
  overlaps the A loads with the previous HMMAs. 5632x512x2048 4599 -> 4271 us.
  **pp512 Q4_0 976 -> 1030 = CUDA (1030)**, Q8_0 964 -> 1013 (CUDA 1134),
  Q4_K_M 832 -> 869, IQ4_XS 923 -> 965. PPL unchanged.

## 2026-10-21 (night): cross-GPU check of the pp round, fixes

Mesa: forked mesa/mesa as gitlab.freedesktop.org/bslette/mesa (glab on
stealth) and pushed `nak-idp4a-imm` (IDP.4A immediate fix + hw test). No MR yet.

New test GPUs: Intel Iris Xe (aimini, ADL GT2, Mesa 26.0.8), Intel UHD 770
(bazzite, ADL-S GT1, Mesa 26.2.4), RTX 4060 Laptop on NVK (bazzite, nouveau
with AD107 GSP 570.144 firmware staged in /var, Mesa main-fixed build). aimini's
RTX 3050 is on the NVIDIA driver now. Script: experiments/xval/xval-pp.sh
(A/B trees), summaries with brief.sh.

What the cross-GPU run of the pp round (`d4418c8`) found and what was fixed:

- **rms_norm wrong on Intel** (UHD 770: RMS_NORM 23/53, RMS_NORM_MUL_ADD 8/37;
  upstream b76b133 passes). The subgroupAdd reduction (`3e3e3d8`, in every
  tree since base) ran at whatever SIMD width ANV picked. Summing all subgroup
  partials (`c07d396`) was not enough; requesting full subgroups of
  device->subgroup_size for the rms_norm pipelines (`3a76e85`) fixes it
  (53/53, 37/37 on UHD 770 and Iris Xe). No change on Volta.
- **rope 8x32 workgroups** (`2a404d7`) were 1.9-2x faster on most shapes on
  every GPU but up to 3-5x slower on RDNA1/3 for 128-dim views and 256/512-wide
  rows. Not coalescing (remapping lanes onto one row, `7a97fe3`, changed
  nothing on AMD): the rows were split into 64-element column slices across
  workgroups in grid y, so concurrent workgroups hit the same slice of many
  rows (memory channel camping). `cdaf6ad`: workgroups cover 8 whole rows,
  32 lanes looping (Volta: up to -26% more). Then 512-wide rows were still
  1.6-1.8x slower on AMD (8 passes per lane); one pair per lane for every
  width was 40% slower on Volta. `8669818`: at most two pairs per lane
  (32 lanes up to 128-wide rows, then 64/128/256), the host dispatches the
  matching rows per workgroup. Volta +3.5% vs `cdaf6ad`, AMD wide rows back
  to 1.03-1.11x of the old shader.
- **rms_norm rows variant** (`e7112dc`): pp rms_norm -31% on NVK Ada, -11%
  Iris Xe (-37% before the subgroup pin), -74% UHD 770, but +6-13% on RADV and
  the NVIDIA driver: gated off
  there (`3929f28`). The register cache of the row in the 512-wide shader
  made RDNA3 20% slower (16 -> 19 us) and did nothing on Volta: now only in
  the rows variant (`6e4b61f`).
- nouveau on the RTX 4060 runtime-suspended between runs; after the resume
  every job timed out (head too). Holding the render node open for the whole
  run (`exec 9</dev/dri/...render`) avoids it.

Final (`6e4b61f`) vs the last validated head (`a8f39f5`), pp512 TinyLlama
-fa 1; all ROPE / RMS_NORM / RMS_NORM_MUL_ADD tests pass on all six GPUs:

| GPU | rope op (us) | rms_norm op (us) | pp512 Q4_0 | Q8_0 | Q4_K_M | IQ4_XS |
|---|---|---|---|---|---|---|
| RX 7800 XT (RADV) | 35.9 -> 14.7 | 15.9 -> 16.0 | +1.2% | +1.0% | +1.0% | +0.8% |
| RX 5700 XT (RADV) | 74.2 -> 7.9 | 57.3 -> 48.9 | +1.0% | +0.8% | +0.9% | +1.1% |
| RTX 3050 (NVIDIA driver) | 98.4 -> 55.6 | 57.7 -> 56.9 | +0.9% | +1.2% | +0.8% | +1.6% |
| RTX 4060 (NVK) | 64.4 -> 22.9 | 40.5 -> 28.0 | +1.8% | +1.2% | +1.4% | +1.4% |
| Iris Xe (ANV) | 424 -> 115 | 157 -> 140 | +1.4% | +0.9% | +1.3% | +1.3% |
| UHD 770 (ANV) | 1169 -> 189 | 799 -> 210 | +4.1% | +2.7% | +3.3% | +3.0% |

tg128 unchanged everywhere. ROPE perf cases (60): geomean 0.32x (UHD 770) to
0.71x (RX 5700 XT) of head's time; the only case slower everywhere is the
2-row vision rope (80x16x2, about 1.5 us, launch-bound), 1.2-1.35x.
Xavier: pp512 1031 / 1011, tg128 58.6 / 42.1 (Q4_0 / Q8_0), unchanged.

The MMVQ round (`base` 7a5733b vs `head` a8f39f5) on the new GPUs: all
MUL_MAT / MUL_MAT_ID / MUL_MAT_VEC_FUSION tests pass (Iris Xe, UHD 770, RTX
4060 NVK). Iris Xe q4_K/q5_K mat-vec -4..-9%, neutral elsewhere; UHD 770
neutral. RTX 4060 NVK: q4_K n=4 299 -> 232 us, q5_K n=4 359 -> 275, iq4_xs
n=4 400 -> 226, q2_K n=4 238 -> 151 (tg at 230 t/s is not GPU bound there).
The opt-in BDA knobs (exp) help Intel tg (UHD 770 Q4_K_M 28.2 -> 36.4,
Q5_K_M 25.1 -> 31.6; Iris Xe +2-5%) through q6_K/q3_K MMVQ.

## 2026-10-21: LineageOS 24.0 with the Volta work, on slot a

Built on stealth (`~/android/lineage-24.0`, full `bacon`, 17 min) and written to
slot a from Linux on slot b with `experiments/android/flash-slota.sh`:

- Mesa `rey-nouveau-gv11b-24-volta` (08bdef6): `volta-cmat-int8` e943159 + the
  two Android build commits + c268c68 (NVK_L2_SYSMEM_MAINT through
  os_get_option, so the vendor property reaches apps; SELinux keeps apps from
  reading /sys/module/nouveau/parameters).
- Kernel `rey-nouveau-gv11b-6.18-v68-l2` (ff75efd): v68 + uvmm_sysmem_vol.
- galen `rey-24`: vendor_boot cmdline `nouveau.uvmm_sysmem_vol=0`, properties
  `vendor.mesa.nvk.volta.cmat=1`, `vendor.mesa.nvk.l2.sysmem.maint=1`, and
  llama.cpp (volta-nvk f35ec04, NDK r27c, Vulkan, shared libs) as prebuilts in
  /system_ext/bin + /system_ext/lib64.

super (SD card, mmcblk0p23) needed a new partition table: system_ext grew
467 -> 524 MB and vendor by 77 KB. The OTA zip is not usable here: it installs
to the inactive slot (b = Linux) and its postinstall runs
nv_bootloader_payload_updater. Linux 6.18 does not see the QSPI (SMD slot
state), so slot a was reached by letting slot b's retries run out.

Android /data is on NVMe p1 (ext4 with the quota feature): the slot b 6.18
kernel was rebuilt with QFMT_V2 (`experiments/android/build-kernel-618.inner.sh`)
so Linux can mount it rw; models staged in /data/local/tmp/llama with
security.selinux = shell_data_file.

Results on Android, same flags as Linux (`llama-bench -fa 1 -p 512 -n 128`),
UI up (lock screen):

| TinyLlama | Android | Linux 6.18 | CUDA L4T |
|---|---|---|---|
| Q4_0 pp512 / tg128 | 1016 / 57.5 | 1031 / 58.6 | 1030 / 62.8 |
| Q8_0 pp512 / tg128 | 1002 / 41.3 | 1011 / 42.1 | 1134 / 40.0 |

Perplexity Q4_0 wikitext 10 chunks: 26.4113 (Linux 26.4037).

- The first Android build used `-DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16`:
  Carmel has no dotprod (Features: fphp asimdhp asimdrdm atomics, no
  asimddp), so test-backend-ops died with SIGILL in ggml_vec_dot_q4_0_q8_0
  (the CPU reference). Fixed to `armv8.2-a+fp16`; llama-bench/perplexity
  were unaffected (all GPU).
- SurfaceFlinger at idle: all 3 layers CLIENT at dump time, but drm_hwcomposer
  stats since boot: 2012 frames, 97 flattened, composition efficiency 0.91;
  last vsync 51 s before the dump. Compositor work under GPU load is next.
- /metadata (2 MB) is full: apexd cannot write active.tmp. Boot unaffected.

## 2026-10-21 (afternoon): llama.cpp in an Android chat app, compositor planes

Stack: `llama-server` on the board (NVK, `-ngl 99 -fa 1 -c 4096 -np 1`, as the
shell user) with Qwen2.5-3B-Instruct Q4_K_M, and ChatterUI 0.10.1 (Remote,
Chat Completions, `http://127.0.0.1:8080/v1/chat/completions`), set up with
`adb shell input`. Correctness on Android first: MUL_MAT 2339/2339 and
FLASH_ATTN_EXT 5332/5332 pass.

| Qwen2.5-3B Q4_K_M | pp512 | tg |
|---|---|---|
| llama-bench, screen static | 330 | 19.6 (tg128) |
| ChatterUI, stock drm_hwcomposer | - | 15.2 / 15.4 (100 / 256 tokens) |
| ChatterUI, fixed drm_hwcomposer | - | 16.6 / 16.5 (256 / 256 tokens) |

Compositor: idle screens already cost nothing (no frames in 20 s). While a
reply streams, the app redraws at about 25-34 fps, and the stock composer
flattened every one of those frames to GPU composition: 403 frames in 12 s,
1018 M pixels, efficiency 0. Tegra194 gives head 0 a primary plane plus
overlays 43/53/56 (shared windows, possible_crtcs 0xf) and a cursor plane, so
planes were not the limit; drm_hwcomposer never tested a plane layout
(0 failed test commits).

Cause (found with a temporary per-frame log in GenericCompositionPlanner):
flatten reason kNoPerPlaneColorspaceSupport with layers `cs=2` (BT.709) and
`cs=200563880`. `LayerData::colorspace` had no initializer and
`HwcLayer::PopulateLayerData()` only writes it for a set dataspace, so UNKNOWN
layers (status bar, taskbar, most apps) carried heap garbage and every frame
looked like a colorspace mix. Fix e7a1d91 (stealth
`external/drm_hwcomposer-upstream`, branch `local-planes-colorspace`):
value-initialize colorspace/transfer_func and compare kDefault as BT.709 (as
ToColorGamut() already does). With it: 300 frames in 12 s, 0 flattened,
GPU composition 53 M pixels (-94%), efficiency 0.93; the four layers scan out
on planes 0/3/6/7. The rest of the gap to llama-bench is the app's own GPU
text rendering.

Testing a composer without reflashing: the serial root shell is in the
bootstrap mount namespace, so bind-mount with `nsenter -t 1 -m`; and init will
not transition into hal_graphics_composer_default from a nosuid mount (/data),
so copy the binary to a tmpfs first (label
hal_graphics_composer_default_exec), bind it over
`/apex/com.android.hardware.graphics.composer/bin/hw/...`, then `stop; stop
vendor.hwcomposer-3.drm_upstream; start ...; start`. Check `/proc/<pid>/exe`.
Reverts on reboot.

## 2026-10-21 (evening): failed plane tests, the app's share of the GPU

Failed test commits (12 per 300 frames): DRM atomic debug (drm.debug=0x10)
showed no kernel-side rejection; a temporary log in
HwcDisplay::TestComposition() showed every failure had one layer mid-fade
(alpha 0.55-0.99, window open / chat screen transition) that no plane accepts
(`planes=0000`): the tegra planes have no "alpha" property, so
DrmPlane::IsValidForLayer() rejects any non-opaque layer, the plan cannot be
built, and the whole frame fell back to the GPU. Fix d122bc5 (fork rey-24):
ICompositorDisplay::AnyPlaneSupportsLayer(), used by IsClientLayer(), so such a
layer alone is client composited. Since composer start incl. app launch,
transitions and two streamed replies: 2364 frames, 1 failed test, 0
flattened, efficiency 0.98.

In-app tg gap (Qwen2.5-3B Q4_K_M, 256 tokens):

| Case | tg t/s |
|---|---|
| server, no UI, non-streaming / streaming | 18.4-18.9 / 18.4-18.8 |
| ChatterUI, `-t 6 --poll 50` (default) | 16.4-16.5 |
| ChatterUI, `-t 2 --poll 0` | 16.7-16.8 |
| ChatterUI, `-t 1 --poll 0` | 16.7 |

- With all layers on the GPU, llama-server's 5 CPU workers still spin at
  42-49% each (ggml threadpool polling); `-t 2 --poll 0` frees them, +2% in
  the app, no change without UI.
- Streaming (one SSE event per token) costs nothing measurable.
- The app renders ~24 fps while streaming with ~1 ms GPU per frame (gfxinfo),
  yet costs ~5 ms per token: the cost is GPU channel switching between NVK
  compute and the app's GL context, not the drawing. NVK reports only MEDIUM
  global priority and nouveau has no runlist/TSG priority or timeslice
  control for Volta, so the real fix (compute channel priority / longer
  timeslice) is a kernel + NVK project. App side: fewer redraws per token.

Flashed (2026-10-21 15:42 build) to slot a with flash-slota.sh: composer from
the image is the fixed one (/proc/<pid>/exe a6109679), libggml-cpu 508ed318
(no dotprod). Streaming chat on the flashed image, 12 s: 354 frames, 0 failed
tests, 0 flattened, GPU composition 25 M pixels, efficiency 0.97; app tg 16.7.

Getting back to slot a from Linux: `systemctl reboot
--reboot-argument=bootloader` (6.18's tegra PMC driver sets SCRATCH0 bit 30,
cboot's fastboot flag) -> cboot fastboot on USB -> `fastboot set_active a;
fastboot reboot`: Android up 65 s later. Now `flash-slota.sh --reboot`. Do not
probe PMC SCRATCH_15 (0x0C390418, cboot's boot chain register) from Linux: the
AON scratch block is firewalled and the access panicked the kernel (CBB SLV
errors).

## 2026-10-21 (night): ChatterUI fork, redraw rate and motion

Fork bslette/ChatterUI, branch motion-settings (1b05e38 + 6e55e6f on upstream
1753a4a, AGPL-3.0). New settings: Chat Window > Streaming update interval
(ms, 0 = every token; Chats.useBuffer() throttles re-renders, the store still
takes every token) and Screen > Reduce Motion (ReducedMotionConfig Always,
no screen transitions, no chat bubble height animation, still typing dots, no
button fade or animated scroll jumps). Upstream re-rendered the Markdown on
every token and animated the bubble height with a JS-driven Animated.timing
after every change. Build: `experiments/android/build-chatterui.sh` (podman,
reactnativecommunity/react-native-android, APP_VARIANT=rey ->
com.Vali98.ChatterUI.rey next to upstream; Gradle needs -Xmx8g / 2g
metaspace).

Qwen2.5-3B Q4_K_M, llama-server -t 2 --poll 0, 256-token replies:

| ChatterUI fork setting | tg t/s | app frames / reply |
|---|---|---|
| 0 ms, motion on (= upstream) | 16.65 | 420 |
| 250 ms, motion on | 17.66 | 240 |
| 0 ms, reduce motion | 17.30 | 278 |
| 100 ms, reduce motion | 18.08 | 158 |
| 250 ms, reduce motion | 18.64 | 78 |
| server, no UI | 18.4-18.9 | - |

250 ms + reduce motion closes the whole in-app gap.

## 2026-10-21 (night): text flashing through the ChatterUI drawer

Report: opening the main menu (drawer) and expanding Data Sources flashed the
chat text behind it in front of the drawer. Drawer and chat are one app layer
(one buffer), so this is not plane z-order: the display was scanning out a
frame the GPU had not finished (the chat is drawn before the drawer covers it).
It only became visible once drm_hwcomposer put app layers on planes.

Cause: minigbm allocates every buffer on nouveau (card1) and tegra-drm (card0)
imports it. Mesa's nvc0 GL driver has no native fence fds, so apps queue
buffers with acquire fence -1 and implicit sync is the only sync. Tegra's
tegra_bo_import() gave imported objects their own empty reservation object,
so drm_gem_plane_helper_prepare_fb() found no fences and the commit never
waited for the render. GPU composition was safe because nouveau syncs
implicitly with itself.

Fix: kernel c9fa4e8 (`rey-nouveau-gv11b-6.18-v68-l2`) sets
`bo->gem.resv = buf->resv` on import, as drm_gem_prime_import_dev() does.
Flashed to slot a (build 672 s, backup /var/tmp/slota-backup-20261021-204245
on slot b). After: all four layers DEVICE, drawer + Data Sources renders
correctly, app GPU time ~1 ms/frame, no tegra/drm errors.

flash-slota.sh: the stage cleanup ran after `--reboot` and failed (exit 255,
board gone); it now runs before the reboot.

## 2026-10-21 (night): Paramount+ crash, YUV EGL images on nvc0

Paramount+ (com.cbs.app) aborted ~5 s after launch in hwui
(AutoBackendTextureRelease "Invalid GrBackendTexture 1920x1080") when its
ExoPlayer TextureView got the first frame from c2.android.avc.decoder (no
hardware codecs on this build): `skia: Could not create EGL image`.

Test binaries (scratchpad ahbegl.c / dmabufyuv.c, NDK r27c): RGBA
AHardwareBuffers import, YCbCr_420_888 and YV12 fail; EGL_LINUX_DMA_BUF_EXT
R8 ok, YVU420/NV12 EGL_BAD_ALLOC. Cause: nouveau_screen_bo_from_handle()
rejects any non-zero winsys offset, so no chroma plane can be imported, even
through the DRI frontend's per-plane YUV lowering.

Fix: Mesa c0e2314 (`rey-nouveau-gv11b-24-volta`): nv50_miptree_from_handle
places the miptree at the offset like a sub-allocated miptree (base.offset,
address = bo + offset); get_handle reports it. All YUV imports ok; with the
library bind-mounted (tmpfs + same_process_hal_file, framework restarted)
Paramount+ runs. Full build with it started.

Hardware decode: the board binds tegra-nvdec/nvjpg/vic (upstream host1x
channel UAPI, firmware only). Upstream Mesa main (2c16073) has NVK Vulkan
Video H.264 decode on NVIDIA's published nvdec_drv.h + clc5b0.h; the L4T
R35 multimedia stack (oldcrustylx ~/nvidia) can serve as a reference trace.

## 2026-10-21 (night): microG

With the YUV fix Paramount+ got further and then died in Java:
`LocationServices.API is not available on this device ... SERVICE_INVALID`
(no Play Services; only com.android.location.fused).

LineageOS 24 frameworks/base already has microG signature spoofing
(198fa728, only for apps signed with the microG key), so no framework patch.
Added vendor/partner_gms from a bslette fork of
lineageos4microg/android_vendor_partner_gms (rey-24 1361bd7): products/microg.mk
(GmsCore, FakeStore, GsfProxy; no F-Droid), and vendorsetup.sh fetches only
those APKs of microG v0.3.16.1.252432 pinned by SHA-256 (GmsCore is 108 MB,
over GitHub's file limit, so not committed). All three are signed by the
microG key Lineage trusts (cert SHA-256 9bd06727...d14165). galen rey-24
ab80de7 sets WITH_GMS := true, GMS_MAKEFILE := microg.mk in lineage.mk;
manifest zz-bslette-forks.xml has the project. Build 574 s: GmsCore and
FakeStore in /product/priv-app, GsfProxy in /product/app.

Location without GPS: `cmd location providers add-test-provider gps` +
`set-test-provider-location gps --location LAT,LONG` (shell needs
`appops set com.android.shell android:mock_location allow`).

Open: black dash corruption on Paramount+ artwork tiles (not video: no codec
running). CPU-written YV12/RGBA sampling and the hwui hardware-bitmap upload
path (glTexSubImage2D into an EGLImage, fence, second context) are exact in
isolation (scratchpad ahbsample.c, hwbitmap.c).

## 2026-10-21 (late night): NVDEC stage 1, bit-exact H.264 on the upstream kernel

`experiments/nvdec/`: standalone decoder on slot b (Linux 6.18.21-nx+) through
the upstream tegra-drm channel UAPI and NVIDIA's published nvdec_drv.h. Both NVDEC
instances bind (15480000 class 0xf0, 15140000 class 0xf5), firmware is L4T's
nvhost_nvdec040_ns.fw as nvidia/tegra194/nvdec.bin. Bit-exact vs ffmpeg on
I-only CAVLC, I/P CAVLC, I/P/B CABAC 320x240 and 720p High with B-pyramid;
1.7 ms per 720p frame. Fixes needed: syncpoint cond at bit 10, EXECUTE with
AWAKEN, TBL output (see README). NVDEC1 (0xf5) stayed wedged after an early
malformed job (cond at bit 8) until reboot.

Next (stage 2): Android Codec2 component: NVDEC into TBL surfaces, VIC (or CPU)
to pitch-linear NV12 gralloc buffers (minigbm/nouveau dma-bufs imported into
tegra-drm), then HEVC/VP9 (nvdec_drv.h has the structs).
