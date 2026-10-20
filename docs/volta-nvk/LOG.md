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
