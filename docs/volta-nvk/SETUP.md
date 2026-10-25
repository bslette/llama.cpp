# Setup and measurement procedure

## Hosts
- **nx-linux**: the Jetson, slot b running Linux 6.18.21-nx+ on the NVMe
  Ubuntu rootfs (p2). Root is reached via `su`; nouveau is blacklisted, so
  `modprobe nouveau` after each boot (GV11B is renderD129). Work trees are
  `/root/llama.cpp` (`volta-nvk`) and `/root/mesa-nvk` (`volta-cmat-int8`),
  installed to `/opt/mesa-nvk`.
- **nx-l4t**: the same board after `flash-slotb.sh --reboot l4t`: L4T R35.6.4
  (5.10.216) on NVMe p3, CUDA 11.4, nsys, ncu, cuobjdump, nvdisasm.
  Return with `flash-slotb.sh --reboot linux618`. Slot a is LineageOS 24.0.
- **stealth**: Lineage build host; `~/nvk-check/mesa-main` (upstream Mesa
  candidate branches).
- **aimini**: RTX 3050 VM with NVK, for cross-checks.

## Build and measure on nx-linux (as root)
- Mesa: `/var/tmp/build-mesa.sh` (needs `PATH=/root/.cargo/bin:$PATH`, about 45 s).
- tg/pp + last-token per-op perf: `/var/tmp/mmvq-bench.sh TAG`.
- NAK shader dump: `NAK_DEBUG=print MESA_SHADER_CACHE_DISABLE=true`. The final
  IR comes after `opt_instr_sched_postpass` (`/var/tmp/nakdump.sh`).
- Tests: `test-backend-ops -o MUL_MAT_VEC_FUSION`, and
  `test-backend-ops -o MUL_MAT -p 'type_b=f32,m=[0-9]+,n=[1-8],'`.
- Models: `/root/models/tinyllama/` (Q4_0, Q8_0, tl-Q4_K_M, tl-Q5_K_M, tl-IQ4_XS, tl-Q4_1).

## Pushing from the board as root
```
GH_CONFIG_DIR=/home/slettela/.config/gh git -c credential.helper= \
  -c 'credential.helper=!/usr/bin/gh auth git-credential' push fork volta-nvk
```
Commits are authored `bslette <bslettela@gmail.com>`.

## Boot stack (since 2026-10-24)
The board boots the r35 UEFI chain, not cboot: see `../uefi-r35/README.md`. Slots switch
through SMD, which AndroidLauncher applies: from Android the boot HAL
(`service call android.hardware.boot.IBootControl/default 9 i32 <slot>; reboot`), from
Linux on slot b `smd-slot set-active a && reboot`. The cboot fastboot route
(`--reboot-argument=bootloader`) is gone. `flash-slotb.sh` writes uncompressed kernels;
under Android push the images and run `write.sh` as root over serial.
