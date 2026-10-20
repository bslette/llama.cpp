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
