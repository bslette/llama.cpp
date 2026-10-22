# NVDEC H.264 on the upstream kernel (stage 1)

Hardware H.264 decode on the Xavier NX NVDEC engine with no NVIDIA userspace: the
upstream tegra-drm channel UAPI (`DRM_IOCTL_TEGRA_CHANNEL_*`, Linux 6.18) and the
picture setup struct from NVIDIA's published `nvdec_drv.h` (copy in Mesa,
`src/nouveau/headers/nvidia/video/nvdec_drv.h`, not included here).

- `host1x.h/.c`: channel open, GEM + channel map, THI method push (offset 0x10/0x11),
  relocations (address >> 8), syncpoint OP_DONE increment, submit + wait.
- `nvdec_h264.c`: Annex B parser (SPS/PPS/slice header incl. ref list modification,
  pred weight table, MMCO), POC types 0/2, reference marking (sliding window + MMCO),
  NVC5B0 method sequence as in Mesa NVK, TBL de-tiling, frames out in display order.
- `smoke.c`, `sem.c`: no-op job and semaphore-release probes. `cmp.py`: per-frame
  compare against an ffmpeg reference. `layout_search.py`: finds the output layout.

Build/run on the board (root, /dev/dri/renderD128 is tegra-drm):
`gcc -O2 -o nvdec_h264 nvdec_h264.c host1x.c && ./nvdec_h264 in.h264 out.yuv`

Results (2026-10-21, NVDEC0 = host1x class 0xf0, firmware
`nvidia/tegra194/nvdec.bin` = L4T `nvhost_nvdec040_ns.fw`): bit-exact against
ffmpeg for 320x240 I-only CAVLC (10/10), I/P CAVLC (30/30), I/P/B CABAC (30/30),
1280x720 High with B-pyramid (60/60), 1.70 ms per 720p frame including submit.

What it took (each one hung or garbled the decode):
- Syncpoint condition field is at bit 10 on Tegra186/194 engines (version
  0x18/0x19), bit 8 on Tegra210: `(OP_DONE << 10) | id`.
- `EXECUTE` needs `AWAKEN_ENABLE` (bit 8) or the engine never reports OP_DONE
  (Mesa NVK leaves it off: it fences with GPU semaphores).
- Output must be `tileFormat = 0` (TBL, Tegra block linear: 64 B x 8 GOBs of 16Bx2
  sectors, blocks of 2 GOBs with `gob_height = 0`); KBL and Tile16x16 are ignored.
- Reference: Mikko Perttunen's github.com/cyndis/vaapi-tegra-driver (2021).

## Stage 2: Android Codec2 component (2026-10-22)

`h264dec.c/h`: the decoder as a library (no exit paths, SPS cropping, VUI
reorder depth, callback per picture in decode order with POC/period), plus
`h264dec_vic_copy()`: VIC 4.2 (host1x class 0x5d, methods at C5B6 offsets,
ConfigStruct from libdrm `tests/tegra/vic42.h`) converts the TBL surface into
a pitch-linear NV12 dma-buf (gralloc buffer imported with PRIME into a VIC
channel on the same fd). Slot: Y8_U8V8_N420, BLK_KIND_GENERIC_16Bx2,
SlotBlkHeight 1, CacheWidth 32Bx8; output BLK_KIND_PITCH, chroma via
SET_OUTPUT_SURFACE_CHROMA_U_OFFSET (0x724). Bit-exact through real
AHardwareBuffers (320x240 IPB, 720p, 1080p with cropping).
`h264dec_cli in.h264 out.yuv [node [vic]]` exercises both paths.

CPU copy-out is the fallback only: tegra-drm maps surfaces write-combined and
uncached reads of freshly decoded frames run at ~145 MB/s (10 ms per 720p).

The component (`c2.rey.avc.decoder`, galen `codec2/`, commit 39eccc0) runs in
`android.hardware.media.c2-default-service` (AIDL IComponentStore/default,
mediacodec domain); rank 128 ahead of c2.android.* (0x200); its
media_codecs_c2.xml lives on odm.
