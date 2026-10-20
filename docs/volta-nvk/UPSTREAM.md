# Bugs and fixes to send upstream

Status: **ready** = a clean commit exists; **needs work** = needs rebasing,
splitting or more testing first; **report** = file an issue, no fix yet.

## Mesa (gitlab.freedesktop.org/mesa/mesa)

Blocked on fork/MR permission for the gitlab.freedesktop.org account. Then run
`glab auth login --hostname gitlab.freedesktop.org`, fork mesa/mesa, and push.

| Issue | Fix | Status |
|---|---|---|
| NAK encodes IDP.4A with an immediate source (opcode 0x826). No such form exists, so it traps with ILLEGAL_INSTR_ENCODING on SM72 and nvdisasm rejects it on SM72/75/86. Hit by llama.cpp iq1_m `dp4a(x, 0x01010101)` | `6656192`; on main as stealth `nak-idp4a-imm` (2 commits incl. hw test) | **ready** |
| NAK puts only 1 stall cycle after BRA/BSYNC/EXIT/CALL/RET/BAR on Volta, where ptxas always uses ≥5 (≥4 after WARPSYNC). One warp occasionally never finishes, giving a job timeout (6/30 runs of k=129 permuted mat-vec) | `7ebf5a5` | **ready** (rebase onto main) |
| NAK pre-RA scheduler chains read-only (`MemOrder::Constant`) loads with other memory ops, and costs global loads at 32 cycles | `b1f2df0` | **needs work**: measure on Turing+ too; the latency value is a heuristic |
| NAK RA: loop-header phis of vector values (HMMA accumulators) don't see later-block vector uses, so they get misaligned and copied every iteration | `6e44b0e` | **needs work**: rebase, run shader-db |
| `nir_lower_explicit_io`: per-component bounds checks on bounded SSBO vectors stop the vectorizer (f16vec4 → 4 loads) | `302949c`, `5f1d6ca`, `e04f5cb` | **needs work**: affects other drivers; needs a CTS run |
| nak: cmat_muladd destination size overflow | `1e32f0c` | **ready** (small) |
| Volta cooperative matrices (HMMA.884 f16/f32, IMMA.8816, f16 accumulators, STEP pairing, latencies) | `2c498ed`..`e71e443` | **needs work**: large series, rebase onto main, CTS cooperative_matrix |
| nouveau: GV11B (0x15b) as Volta; tegra scanout/clear fixes | `18d4a25`, `5967863`, `a622087` | **needs work**: check against the current tegra/nouveau code on main |
| NAK does no immediate-offset folding for global loads: each load gets a full 64-bit address add where ptxas uses `[R+imm]`. Likely part of the remaining MMVQ gap | none yet | **report / implement** (see NEXT.md) |

## llama.cpp (github.com/ggml-org/llama.cpp)

| Issue | Fix | Status |
|---|---|---|
| test-backend-ops had no whole-tile cases for most quant types, so a broken `dequant_q5_0` went undetected | part of `7503f0d` | **needs work**: split out as a tests-only PR |
| Vulkan dequant shaders store f16 2 bytes at a time, 64 bytes apart (uncoalesced) | part of `7503f0d` | **needs work**: split out; helps all GPUs, needs measurements off Volta |
| rms_norm smem tree reduction (9 barriers) | `3e3e3d8` | **ready**: needs numbers on another GPU |
| MMVQ per-workgroup overhead on small K: multiple subgroups per WG on separate rows, no smem | `ad1c0b8` (gated to NVK GV1xx) | **needs work**: try on other vendors before widening the gate |
| MMVQ gate/up/SWIGLU fusion (CUDA has it, Vulkan didn't) | `854e05d` | **needs work**: only the NO_SHMEM path; non-NVK paths and MUL_MAT_ID not covered |
| Volta f16 GEMM path, int8 coopmat probe | `b4bb19c`, `005a3a5` | **needs work**: depends on Mesa Volta coopmat landing |

## Local / board (not upstream, keep in mind)

- The 6.18 and L4T kernels never mark slot b as successfully booted, so
  every reset after a failed boot burns a cboot retry. At zero the board
  falls back to Android. Fix: run the boot-control mark-successful step on
  Linux boot.
- cboot ignores `boot_b`'s cmdline and uses `vendor_boot_b`'s.
