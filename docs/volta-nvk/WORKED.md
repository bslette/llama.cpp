# What worked, and where it is committed

All numbers are from Jetson Xavier NX (GV11B) under NVK, TinyLlama-1.1B, unless noted.

## llama.cpp: `bslette/llama.cpp` branch `volta-nvk` (base upstream b76b133)

| Commit | Change | Measured |
|---|---|---|
| `005a3a5` | int8 coopmat MMQ on non-RDNA devices (layout probe, TK != 16) | enables the int8 path on NVK |
| `a47598d` | MMVQ (q8_1 + IDP.4A) for all types on Volta/NVK | tg baseline for all types |
| `b4bb19c` | Volta prompt matmuls: dequantize to f16, then `mul_mm_volta_f16.comp` HMMA GEMM (128x128 tiles, BK 32, register prefetch, double-buffered smem). Off with `GGML_VK_DISABLE_VOLTA_GEMM=1` | pp512 Q4_0 466 → 747, Q8_0 395 → 740 |
| `3e3e3d8` | rms_norm reduces with subgroupAdd instead of a 9-step smem tree | 485 → 444 µs/call; pp +0.5% |
| `7503f0d` | coalesced 16-byte f16 dequant stores for every quant type; Volta GEMM for all types; whole-tile test cases | Q4_K_M pp 272 → 684, IQ4_XS 450 → 742 |
| `ad1c0b8` | MMVQ: 4 subgroups per workgroup, each on its own rows (`mmvq_row_groups`=4 on NVK GV1xx, `rm_stdq_int`=2, no smem/barrier) | tg Q4_0 42.1 → 51.7, Q8_0 35.6 → 37.9, IQ4_XS 28.8 → 48.0, Q4_K_M 36.0 → 42.3, Q5_K_M 32.3 → 36.9, Q4_1 44.8 → 49.8 |
| `854e05d` | fuse MUL_MAT(up) + MUL_MAT(gate) + SWIGLU into one MMVQ dispatch (spec const `FUSE_GLU`, push consts `a_offset_x`/`gate_delta`; needs both weights in one buffer, whole blocks apart) | Q4_K_M 42.3 → 43.2, Q5_K_M 36.9 → 37.7, Q4_0 neutral |

Correctness: PPL matches CPU/CUDA (Q4_0 23.84, Q8_0 23.10 wikitext 20x512).
test-backend-ops: full MUL_MAT 2929/0, MUL_MAT_ID 945/0, MUL_MAT_VEC_FUSION 1265/0.

## Mesa: `bslette/android_external_mesa` branch `volta-cmat-int8`

| Commit | Change | Measured |
|---|---|---|
| `18d4a25` | nouveau: treat GV11B (chipset 0x15b) as Volta | – |
| `302949c`, `e04f5cb`, `5f1d6ca` | NIR/NVK: one bounds check per vector for bounded SSBO access, vectorize before bounds-checked lowering on pre-Turing, branchless fallback for partly OOB vectors | restores vector loads under robustness |
| `1e32f0c` | nak: don't overflow the cmat_muladd destination size | bug fix |
| `2c498ed`, `c6773e3`, `4f1f86b` | HMMA.884 cooperative matrices on Volta; 16x16x16 f16; NVK exposes Volta tensor cores (opt-in) | – |
| `9fad561`, `509ee6d` | int8 cooperative matrices with IMMA.8816; 12 cycles after IMMA | – |
| `8d3184b` | loads and HMMA/IMMA share scoreboards | – |
| `390cff5` | f16 accumulators with HMMA.884.F16 | GEMM 1.24 → 2.21 TFLOPS |
| `6656192` | never encode IDP.4A with an immediate operand | fixes iq1_m GPU traps (upstream candidate) |
| `7ebf5a5` | stall 5 cycles after BRA/BSYNC/EXIT on Volta (4 after WARPSYNC) | fixes intermittent single-warp hangs |
| `ae1357e` | keep HMMA.884.F16 STEP0/STEP1 together; 4 cycles between HMMAs | pp Q4_0 748.7 → 763.5 |
| `6e44b0e` | RA: place phis for their vector use in later blocks | loop movs 124 → 16 per K tile; pp 763.5 → 777.7 |
| `e71e443` | model Volta HMMA/IMMA results as 24 cycles | neutral, correct model |
| `972938f` | L2 maintenance around every exec when nouveau caches sysmem (uvmm_sysmem_vol=0); lets Tegra use its GPU L2 | pp +12–13%, tg +11% (Q4_0 879/57.9, Q8_0 868/42.0) |
| `b1f2df0` | pre-RA scheduler: constant loads are not side effects; global ld latency 200 | Q6_K lm_head 2.10 → 1.58 ms, about +3% tg |

Upstream-Mesa-ready copy of the IDP.4A fix and its hardware test: stealth
`~/nvk-check/mesa-main` branch `nak-idp4a-imm` (on mesa main 335d569).

## Kernel: `bslette/android_kernel_nvidia_kernel` branch `rey-nouveau-gv11b-6.18-l2`

| Commit | Change | Measured |
|---|---|---|
| `e7a8ffd` | nouveau `uvmm_sysmem_vol` module param (0 = VM_BIND sysmem L2-cacheable), on top of the board's `rey-nouveau-gv11b-6.18` (977d70d) | with Mesa `972938f`: pp +12%, tg +11% |

## Tooling (bazzite `lineage/tools/`, not in git)

- `tools/jobs/jobs.sh`: `launch` / `wait` / `waitp`. Launch is detached and
  checks that the job started. Waits are deadline-bound and report
  DONE/FAILED/DIED/STALLED/OVERDUE/UNREACHABLE. `JOB_LAUNCH=systemd` is needed
  for root via su.
- `tools/slotb/flash-slotb.sh`: `l4t` | `linux618` | `restore` slot b images
  (boot_b, kernel-dtb_b, vendor_boot_b with `root=LABEL=...`), with readback verify.
- Board: `/var/tmp/build-mesa.sh`, `/var/tmp/mmvq-bench.sh TAG`, `/var/tmp/nakdump.sh`.
