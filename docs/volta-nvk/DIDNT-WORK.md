# Things tried that did not work

Keep every failed attempt here with the reason, so it isn't repeated.

## MMVQ (token generation)

| Attempt | Result | Why |
|---|---|---|
| 32-thread (one-subgroup) workgroups for MMVQ | −6 to −15% | Volta allows 32 CTAs/SM, so 32-thread CTAs cap occupancy at 50% |
| 3–4 rows per subgroup (instead of 2) | no gain / slower | more registers, and too few workgroups for 2048-row matrices |
| Fused gate/up with 2 rows of each matrix per subgroup | same as 1 row | – |
| Fused gate/up for Q8_0 and IQ4_XS | about −2% (Q8_0 37.8 → 37.1, IQ4_XS 48.0 → 47.3) | extra registers and occupancy loss outweigh the saved dispatch; left disabled |
| Fold constant address offsets into LDG immediates (unsafe experiment, 78/121 loads) | tg Q4_0 −0.9%, Q8_0 +0.6%, Q6_K lm_head +17% slower | mat-vec is not ALU/issue bound; −8% instructions bought nothing | 
| ivec2 q8_1 loads for K_PER_ITER 8 (Q8_0 MMVQ) | tg 42.2 → 40.9 | dropped from `39da5ea` |
| `NAK_DEBUG=nowrap` on top of vector loads | Q4_K_M 52.7 → 51.3, Q5_K_M 48.7 → 48.0 | the fold no longer pays once loads are vectors |
| q6_K MMVQ with aligned 8-byte window loads through SSBOs (no pointers) | lm_head 1455 → 1665 µs | load count wasn't the cost; the per-load 64-bit address add was |
| q2_K with 8-byte window loads through pointers | 780 → 790 µs | blocks are 4-byte aligned already; realign ALU ate the gain (plain 4-byte loads through pointers: 689) |
| 64x64-tile, 2-subgroup Volta GEMM for small matrices | k/v proj 344 → 474 µs; everything small: pp512 Q4_0 659 | half the reuse per load; SM balance gain is smaller than that |
| q4_0 fused dequant in the Volta GEMM after `edb5d1c` | pp512 889 vs 929 unfused | 16-bit loads from 18-byte blocks while staging; opt-in now |
| FA row-max reading sfsh as vec4 | no codegen change | NIR already merged the component reads |
| FA P/V coopmat loads as 64-bit (NAK `e943159` + separate PMat/VMat) | FA 3829 → 3833 µs | the u16 loads were not FA's bottleneck |
| Volta GEMM 128×256 with 2×4 subgroups of 64×64 | wrong results, 4× slower | 60 KB shared memory > 48 KB default |
| Volta GEMM with swapped operand roles (D^T) so the epilogue is all 128-bit | 4599 → 4606 µs | epilogue is not a cost after `609ff7c` |
| Blanket address fold with the L2 on (`NAK_DEBUG=nowrap`) for every shader | q6_K mat-vec +12–15% slower, Q4_1 tg −3.5% | the gain is per shader; apply it only to q4_K/q5_K/iq4_xs |
| Hypothesis: `ld.strong.gpu` loads slow MMVQ | wrong | only Mesa's meta copy shader emits them; MMVQ uses `LDG.E.CONSTANT.SYS`, same as CUDA |

## Re-tested with the GPU L2 on (2026-10-20)

| Attempt | Result |
|---|---|
| Multi-row coopmat FA (GGML_VK_FA_BR 32/64) | pp512 880 → 814 / 769 |
| Fused dequant for Q8_0 | pp512 868 → 791 |
| Forcing scalar FA for all row counts | pp −12% (kept only for ≤ 8 rows) |
| GLU fusion for Q8_0/IQ4_XS | still slower (Q8_0 41.8 → 41.0) |
| Old MMVQ layout, 1 or 4 rows/subgroup, DISABLE_MMVQ, DISABLE_VOLTA_GEMM | all slower |
| GEMM variants (w64, pitch, nb, v4) | same ~15% gain as baseline; tiled layout best (3.13 TFLOPS) |

## NAK scheduler

| Attempt | Result | Why |
|---|---|---|
| Raise the GPR floor/target for the pre-RA scheduler so loads can be hoisted further | slower | fewer warps resident; Volta needs ≤32 GPRs for full occupancy |
| Constant-load chain fix alone (without the latency change) | no schedule change | the 32-register cliff target keeps the original order; the latency of 200 is what moves loads |
| Env knobs for latency/GPR floor | removed | they were for experiments only |

## Older (around 2026-10-17, from `jetson-root-notes.tgz`, results not recorded)

- `llama-fa-multirow-attempt.diff`: flash attention coopmat1 handling Br
  query rows as several 16-row tiles (`GGML_VK_FA_BR`). Not kept.
- `llama-volta-fused-dequant-attempt.diff`: dequantize q4_0/q8_0 while
  staging A into shared memory inside the Volta GEMM, instead of a separate
  dequant pass. Not kept. The separate coalesced dequant is about 3 ms per
  4096x14336 matrix, so the potential gain is small.
- `nak-sb-policy-experiments.diff`: shared-memory loads never share a
  scoreboard with global loads; scoreboard eviction avoids global loads.
  Not kept.
- `nak-f32-quad-experiment.diff`: HMMA f32 accumulator STEP0..3 as one op.
  Superseded by the f16-accumulator path.
- A post-scheduling "glue" pass to rejoin HMMA STEP pairs: 64/128 pairs
  stayed split because of RA copies, so it was replaced by `ae1357e`.

## Infrastructure

- kexec of the L4T 5.10 kernel from 6.18 failed three ways (DTB ignored by
  `kexec -l`, cbb-noc crash, hang after sdhci). Flashing slot b with
  `flash-slotb.sh` replaced it.
- `cmd | grep pattern` under `set -eo pipefail` fails the job when nothing matches. Append `|| true`.
- Full `test-backend-ops -o MUL_MAT` takes >10 min on the board (CPU reference).
  Filter mat-vec with `-p 'type_b=f32,m=[0-9]+,n=[1-8],'` (~2 min).
- Root `git push` on nx-linux hangs at a username prompt. Use the gh
  credential helper (see SETUP.md).

## Cross-GPU (2026-10-21)

- rope: remapping lanes so each run of 32 invocations covers 32 contiguous pairs
  of one row (`7a97fe3`) did not fix the AMD wide-row slowdown of the 8x32
  workgroups; the cause was column slices across workgroups (channel camping),
  fixed by whole-row workgroups.
- rope with one pair per invocation for every row width (up to 256 lanes per
  row): 40% slower on Volta (128-wide rows +76%); Volta wants two pairs each.
- rms_norm: summing all subgroup partials in a loop was not enough on Intel;
  the pipelines also need a fixed (full) subgroup size.
- rms_norm: pinning the subgroup size was not what made RDNA3 slower (19.3 vs
  19.3 us with the pin Intel-only); the register cache of the row was.
- nouveau on an Ada laptop GPU: after runtime suspend/resume every job timed
  out. Keep a DRM client open (render node fd) for the whole benchmark.
- Trusted OS on slot b (2026-10-23, reverted, original secure-os_b restored):
  tegraflash `signwrite` signs with an empty magic id, so MB2 rejects the TOS;
  single-partition `write` does not erase QSPI (result = old AND new);
  NVIDIA r35 ATF + OP-TEE under the r32 MB1/MB2 dies in RAS errors
  (MTS_CARVEOUT); Lineage ATF v2.12 SPD=opteed takes an EL3 exception when
  initialising BL32 (upstream ATF supports only Trusty on Tegra194).
- VP9 on NVDEC below 128x128: 8x8 decodes to zeros, 66x66 diverges after a
  few frames (NVIDIA documents 128x128 as the minimum). Rejected instead.
- VIC with an odd NV12 target size (VP9 282x173): the job completes and
  writes nothing. Those pictures use the CPU copy.
- VP9 mcomp_filter_type in libvpx/CUVID order: wrong for every frame with a
  fixed regular/smooth filter; the engine wants spec order. Swapping the
  switchable-filter counts the same way broke adaptation: those are libvpx order.
- Emitting several pictures per process() call with the running work's
  picture left on the work: Codec2 delivers it after older works finished
  in the same call, so output came out of order (frames 0,2,1,4,5,3...).
- NVENC configured from nvenc_drv.h's comments alone (2026-10-23): tiled-16x16
  reference pictures, block_height counted as log2 of 4 GOBs, -1 for unused
  reference entries, zeroed ME/MD controls: the engine wrote an early status
  (error_status 1) and never finished the job. NVIDIA's actual setup (captured on
  L4T) differs in all of those.
- Encoder input tiled with 4-GOB blocks while the engine read 2-GOB blocks: the
  reconstruction looked right (same scramble both ways), a decoder's output did
  not. Compare decode against recon, not recon against source.
- VIC RGB input with SlotLumaWidth in bytes: the job completes and writes nothing.
  It is in pixels.

