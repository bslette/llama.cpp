# NVENC H.264 on the upstream kernel (2026-10-23)

Hardware H.264 encoding on the Xavier NX NVENC engines with no NVIDIA userspace:
the upstream tegra-drm channel UAPI, NVIDIA's published `nvenc_drv.h` (Mesa
`src/nouveau/headers/nvidia/video/`, NVENC 8.2 layout; the Xavier firmware
`nvidia/tegra194/nvenc.bin` = L4T `nvhost_nvenc070.fw` is NVENC 7.0 with magic
`0xC5B70006` and the same structure layout), and VIC for the input conversion.

Kernel: `drm/tegra: Add NVENC driver` (bslette/android_kernel_nvidia_kernel
`rey-nouveau-gv11b-6.18-v68-l2` 549bf09 for Android, `rey-nouveau-gv11b-6.18-l2-nvenc`
for the slot b Linux kernel). It is nvdec.c without the RISC-V path; the TFBIF
TRANSCFG register is at 0x1844 (L4T nvhost t194.c). Both engines (host1x classes
0x21, 0x22) probe and run jobs.

Encoder library: galen `codec2/nvdec/h264enc.c`; Codec2 component `c2.rey.avc.encoder`
(`codec2/C2NvencEnc.cpp`).

## Files

- `nvenctest.c`: bring-up harness (I420 file in, Annex B out, the engine's
  reconstructed pictures out; QP/CBR/VBR, Baseline/High). Build on the board next
  to `host1x.c`: `gcc -O2 -o nvenctest nvenctest.c host1x.c -I/usr/include/libdrm`.
- `enctest.c`: test of the galen library through a dma-buf, NV12 or RGBA, via VIC
  or the CPU fallback (`CPU=1`).
- `vicrgb.c`: VIC RGB -> NV12 block-linear experiments (format, pitch, colour matrix).
- `mcenc.c`: NDK MediaCodec encoder test (YUV420 flexible ByteBuffer input).
- `capture/nvcap.c`: LD_PRELOAD shim for L4T r35 that logs every nvhost channel
  submit (THI methods, relocations) and dumps the buffers selected methods point at
  (buffer handles are dma-buf fds on r35: mmap them). Used with the multimedia API
  sample `01_video_encode` to read NVIDIA's picture setup;
  `capture/*-setup.txt` are two of those setups, printed by `psd.sh` (gdb over
  `psdump.c`).

## What it took

- NVIDIA's driver (captured) uses, unlike the header's comments: block-linear
  input, reference and reconstructed surfaces (not tiled 16x16), `block_height = 2`
  meaning blocks of 2 GOBs (16 rows); separate luma/chroma buffers given through the
  `*_LUMA`/`*_CHROMA` methods with zero offsets in the surface configs; unused
  reference list entries -2. Guessing these hung the engine (it recovers by itself
  after the runtime-PM idle cycle).
- The status word `error_status` is 2 when a picture is done.
- Rate control: mode 18 (macroblock-row CBR) / 17 (VBR); `R` = bit/s / fps / pixels
  in 8.8, `Bmin` = CPB / pixels * 256 / 5, CPB = 2.56 s; the persistent RC buffer
  starts with fullness 0.2 CPB (28.4), `Wpbi` {24,19,48}, `np` (gop-1)*16,
  `prevQS` 256, `prevQP` 26.
- VIC: RGB input pitch is in pixels (`SlotLumaWidth`); NV12 block-linear output with
  `OutBlkHeight = 1`. Output colour matrix rows are Y, Cr, Cb, columns R, G, B,
  offset; with `r_shift` 8, 1.0 = 1 << 16 and offsets are 10-bit values << 8
  (BT.601 limited bars come out exact to +-1).

## Results (board, Linux 6.18)

- The engine's reconstruction equals ffmpeg's decode of the stream (bit-exact) for
  320x240 test bars, 640x360 (cropped) and 1080p, Baseline and High.
- QP 28: 640x360 45.2 dB at ~930 kbit/s; 1080p High 46.8 dB at 7.8 Mbit/s.
- Engine time: ~0.5 ms (320x240), ~1 ms (640x360), ~6 ms (1080p High) per picture,
  i.e. ~165 fps at 1080p on one of the two engines.
- CBR: 2 Mbit/s target -> 1.97, 6 -> 6.36 (1080p, 60 frames).
- Library: VIC NV12 path bit-identical to the CPU path; RGBA via VIC 44.3 dB
  against ffmpeg's BT.601 conversion; 1.2 ms per 640x360 picture including the VIC
  conversion (CPU fallback ~35 ms).
