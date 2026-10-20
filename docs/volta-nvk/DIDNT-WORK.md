# Things tried that did not work

Keep every failed attempt here with the reason, so it isn't repeated.

## MMVQ (token generation)

| Attempt | Result | Why |
|---|---|---|
| 32-thread (one-subgroup) workgroups for MMVQ | −6 to −15% | Volta allows 32 CTAs/SM, so 32-thread CTAs cap occupancy at 50% |
| 3–4 rows per subgroup (instead of 2) | no gain / slower | more registers, and too few workgroups for 2048-row matrices |
| Fused gate/up with 2 rows of each matrix per subgroup | same as 1 row | – |
| Fused gate/up for Q8_0 and IQ4_XS | about −2% (Q8_0 37.8 → 37.1, IQ4_XS 48.0 → 47.3) | extra registers and occupancy loss outweigh the saved dispatch; left disabled |
| Hypothesis: `ld.strong.gpu` loads slow MMVQ | wrong | only Mesa's meta copy shader emits them; MMVQ uses `LDG.E.CONSTANT.SYS`, same as CUDA |

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
