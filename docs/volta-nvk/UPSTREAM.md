# Bugs and fixes to send upstream

Status: **ready** = a clean commit exists; **needs work** = needs rebasing,
splitting or more testing first; **report** = file an issue, no fix yet.

## Mesa (gitlab.freedesktop.org/mesa/mesa)

Fork: https://gitlab.freedesktop.org/bslette/mesa (created 2026-10-21 with glab
on stealth, logged in as bslette). Branch `nak-idp4a-imm` is pushed there; no
merge request opened yet.

| Issue | Fix | Status |
|---|---|---|
| NAK encodes IDP.4A with an immediate source (opcode 0x826). No such form exists, so it traps with ILLEGAL_INSTR_ENCODING on SM72 and nvdisasm rejects it on SM72/75/86. Hit by llama.cpp iq1_m `dp4a(x, 0x01010101)` | `6656192`; on main as `nak-idp4a-imm` (2 commits incl. hw test), pushed to the fdo fork | **ready**: open the MR. Still unfixed on main 02f13375 (2026-10-25): every iq1_m MUL_MAT/MUL_MAT_ID case loses the device on Ada (RTX 4060) and Ampere (RTX 3050) |
| NAK puts only 1 stall cycle after BRA/BSYNC/EXIT/CALL/RET/BAR on Volta, where ptxas always uses ≥5 (≥4 after WARPSYNC). One warp occasionally never finishes, giving a job timeout (6/30 runs of k=129 permuted mat-vec) | `7ebf5a5` | **ready** (rebase onto main) |
| NAK pre-RA scheduler chains read-only (`MemOrder::Constant`) loads with other memory ops, and costs global loads at 32 cycles | `b1f2df0` | **needs work**: measure on Turing+ too; the latency value is a heuristic |
| NAK RA: loop-header phis of vector values (HMMA accumulators) don't see later-block vector uses, so they get misaligned and copied every iteration | `6e44b0e` | **needs work**: rebase, run shader-db |
| `nir_lower_explicit_io`: per-component bounds checks on bounded SSBO vectors stop the vectorizer (f16vec4 → 4 loads) | `302949c`, `5f1d6ca`, `e04f5cb` | **needs work**: affects other drivers; needs a CTS run |
| nak: cmat_muladd destination size overflow | `1e32f0c` (branch nak-cmat-muladd-size on main) | **ready** (small): only Volta lowering reaches it; no regression on Ada and Ampere NVK (RTX 4060, RTX 3050: MUL_MAT 2226, MUL_MAT_ID 1105, FA 5368 all pass, pp512 same), 2026-10-25 |
| Volta cooperative matrices (HMMA.884 f16/f32, IMMA.8816, f16 accumulators, STEP pairing, latencies) | `2c498ed`..`e71e443` | **needs work**: large series, rebase onto main, CTS cooperative_matrix |
| nouveau: GV11B (0x15b) as Volta; tegra scanout/clear fixes | `18d4a25`, `5967863`, `a622087` | **needs work**: check against the current tegra/nouveau code on main |
| **nouveau maps VM_BIND system memory volatile (L2-uncached)** (`nouveau_uvmm_vmm_map`: vol = 1 for non-VRAM). On Tegra iGPUs (no VRAM) the GPU L2 then caches nothing; L1-bypassing loads read at DRAM speed. Needs a kernel change (vol = 0 for system memory on iGPUs, or a VM_BIND flag) plus L2 maintenance by the submitter or kernel | kernel: `e7a8ffd` on bslette/android_kernel_nvidia_kernel `rey-nouveau-gv11b-6.18-l2` (module param); Mesa: `972938f` | **needs work**: for upstream, replace the param with a per-device default (iGPU) or a VM_BIND flag, and decide whether the kernel or NVK does L2 maintenance |
| NAK does no immediate-offset folding for SSBO loads: a constant inside the 32-bit offset add can't be folded without a no-wrap guarantee, so each load gets a full 64-bit address add where ptxas uses `[R+imm]` | opt-in `df97444` (`NAK_DEBUG=nowrap`) | **report/discuss**: worth +11–20% tg on K-quants on Volta; needs NIR nuw on SSBO offsets or apps using 64-bit addressing |

## llama.cpp (github.com/ggml-org/llama.cpp)

| Issue | Fix | Status |
|---|---|---|
| test-backend-ops had no whole-tile cases for most quant types, so a broken `dequant_q5_0` went undetected | branch `test-whole-tile-mulmat` (`1fd4708a7`, on master 74bca3950) | **ready**: 60/60 pass on six Vulkan GPUs (2026-10-25); open the PR |
| Vulkan dequant shaders store f16 2 bytes at a time, 64 bytes apart (uncoalesced) | part of `7503f0d` | **needs work**: split out; helps all GPUs, needs measurements off Volta |
| rms_norm smem tree reduction (9 barriers) | `3e3e3d8` + `c07d396` + `3a76e85` | **ready**: as committed `3e3e3d8` broke Intel (ANV picks the SIMD width); with full subgroups of a fixed size it passes on all six test GPUs. Send the three together |
| rope 8 rows per workgroup, up to two pairs per invocation (was one row per 512-invocation workgroup, mostly idle for 64/128-dim heads) | `2a404d7` + `cdaf6ad` + `8669818` | **ready**: pp rope op 1.4–9x faster on RDNA1/3, Ada (NVK + NVIDIA driver), Iris Xe, UHD 770, Volta; all ROPE tests pass |
| rms_norm 128-invocation variant for many rows | `e7112dc` + `3929f28` + `6e4b61f` | **ready, gated**: faster on NVK and Intel, slower on RADV and the NVIDIA driver |
| MMVQ per-workgroup overhead on small K: multiple subgroups per WG on separate rows, no smem | `ad1c0b8` (gated to NVK GV1xx; applies cleanly to master) | **ready, gated**: forced on elsewhere (2026-10-25) it is correct but tg is unchanged on RDNA3/Ada/Ampere/Intel and q8_0 kernels get slower; keep the Volta gate |
| MMVQ gate/up/SWIGLU fusion (CUDA has it, Vulkan didn't) | `854e05d` | **needs work**: only the NO_SHMEM path; non-NVK paths and MUL_MAT_ID not covered |
| CUDA backend: `volta_mma_available(cc)` requires `ggml_cuda_highest_compiled_arch(cc) == 700`. An sm_72 (Xavier) build reports 720, so Xavier never gets the MMA flash-attention kernel and uses `flash_attn_tile` (no tensor cores) even for pp512. Confirmed in the nsys profile | none yet: test `>= VOLTA && < TURING` on the board | **report** |
| MMVQ scalar loads: the q4_K/q5_K/iq4_xs/q8_1 reads were scalar 32-bit loads the compiler couldn't merge; vector views fix it on any driver | `39da5ea` | **ready**: validated 2026-10-20 on RTX 4060 (NVIDIA driver, q4_K −21–40%), RTX 3050 (NVK, −10–28%), RX 7800 XT (RADV, ±2%), RX 5700 XT (no MMVQ); all tests pass |
| iq1_m scales as one 8-byte load | part of `8f35297` | **optional**: −1–6% NVIDIA, +3% RDNA3 |
| MMVQ for 2-byte-aligned blocks (q2_K/q3_K/q6_K) through 64-bit pointers so constant offsets fold | `42d877e`, `7659de7`, `8f35297` (gated to NVK Volta) | **keep gated**: RDNA3 +10–24% slower, NVIDIA driver neutral, NVK Ampere mixed; correct everywhere |
| Distro NVK (Mesa 26.0.8) loses the device on llama.cpp iq1_m MMVQ on Ampere too (RTX 3050) | Mesa `6656192` (IDP.4A immediate) fixes it | adds weight to sending the Mesa fix |
| Volta f16 GEMM path, int8 coopmat probe | `b4bb19c`, `005a3a5` | **needs work**: depends on Mesa Volta coopmat landing |

## Rebase check (2026-10-25)

Against llama.cpp master 74bca3950 and Mesa main 02f13375 (trial cherry-picks):

- Still unfixed upstream: NAK IDP.4A immediate (`OpIDp4` legalize still allows an
  immediate src1); CUDA `volta_mma_available` (still `== GGML_CUDA_CC_VOLTA`).
- Upstream has not touched the rope/rms_norm/MMVQ/dequant shaders since b76b133,
  but our sets depend on each other: only `ad1c0b8` picks cleanly. rope
  (`cdaf6ad` sits on the reverted `7a97fe3`), rms_norm (`3a76e85`), MMVQ vector
  loads (`39da5ea`, sits on `ad1c0b8`), `854e05d` and `7503f0d` conflict: extract
  each as one squashed diff of its final file state instead of cherry-picking.
- The whole-tile MUL_MAT test loop is ours (added in `b4bb19c`); upstream has no
  such cases, so the tests-only PR adds the whole loop.
- Mesa: `1e32f0c`, `6e44b0e` and the nir_lower_explicit_io set pick cleanly;
  `7ebf5a5` (sm70.rs) and `b1f2df0` (scheduler) conflict.

## Local / board (not upstream, keep in mind)

- The 6.18 and L4T kernels never mark slot b as successfully booted, so
  every reset after a failed boot burns a cboot retry. At zero the board
  falls back to Android. Fix: run the boot-control mark-successful step on
  Linux boot.
- cboot ignores `boot_b`'s cmdline and uses `vendor_boot_b`'s.
