# NVK vs CUDA on Jetson Xavier NX (GV11B, SM72): project notes

Goal: close the gap between llama.cpp's Vulkan backend on NVK/NAK (Mesa) and
llama.cpp's CUDA backend under NVIDIA L4T, on the same board.

These files are the running record. **Update or append to them after every
pass through the measure → change → test → commit loop**, including failed
attempts.

| File | Contents |
|---|---|
| [LOG.md](LOG.md) | Dated activity log, newest last |
| [WORKED.md](WORKED.md) | Changes that worked, with measured gains and the commit and branch each lives on |
| [DIDNT-WORK.md](DIDNT-WORK.md) | Things tried that gave no gain or were wrong, and why |
| [UPSTREAM.md](UPSTREAM.md) | Bugs and fixes that should go to upstream projects (Mesa, llama.cpp) |
| [NEXT.md](NEXT.md) | Ranked plan of what to try next |
| [SETUP.md](SETUP.md) | Board, hosts, scripts and how to measure |

The master copy is `lineage/docs/nvk-perf/` on bazzite. The same files are
committed to `bslette/llama.cpp` branch `volta-nvk` under `docs/volta-nvk/`.
Drop that directory before sending any of that branch upstream.

## Current standing (2026-10-20)

TinyLlama-1.1B, `llama-bench -fa 1`, GPU 1109 MHz, EMC 1866 MHz, same
llama.cpp base (b76b133). Throughput is in t/s.

| | NVK start of 2026-10-19 | NVK now | CUDA (L4T R35.6.4, CUDA 11.4) | NVK / CUDA |
|---|---|---|---|---|
| pp512 Q4_0 | 776 | 778 | 1030 | 76% |
| pp512 Q8_0 | 769 | 768 | 1134 | 68% |
| tg128 Q4_0 | 42.1 | 52.0 | 62.8 | 83% |
| tg128 Q8_0 | 35.6 | 37.9 | 40.0 | 95% |
| tg128 Q4_K_M | 36.0 | 43.2 | – | |
| tg128 IQ4_XS | 28.8 | 47.9 | – | |

Branch heads:
- llama.cpp: `bslette/llama.cpp` `volta-nvk`, head `854e05d` (plus this docs commit)
- Mesa: `bslette/android_external_mesa` `volta-cmat-int8`, head `b1f2df0`
- Upstream Mesa candidate: stealth `~/nvk-check/mesa-main` branch `nak-idp4a-imm`
  (waiting for gitlab.freedesktop.org fork permission)
