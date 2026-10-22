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

## Stage 3: H.265 (2026-10-22)

`hevcdec.c/h` (galen e291f30, `c2.rey.hevc.decoder`): Main profile, 8-bit
4:2:0, up to 4096x4096. Same channel, surfaces (TBL, 17 slots) and VIC path
as H.264; the shared parts moved to `nvdec_common.c/h` (VIC copy, CPU
detiler, `struct nvdec_picture`) and `nvdec_bits.h` (RBSP bit reader).
`nvdec_cli` decodes either codec by file extension.

Engine interface, from nvdec_drv.h `nvdec_hevc_pic_s` and the unmerged Mesa
NVK branch (gitlab dwlsalmeida/mesa `nvk-vulkan-video-h265`, h265.rs, WIP):

- APP_ID/codec 7. Methods: DRV_PIC_SETUP, IN_BUF_BASE, PICTURE_INDEX,
  COLOC_DATA, NVDEC_STATUS, PICTURE_LUMA/CHROMA_OFFSET0..16, plus
  HEVC_SET_SCALING_LIST_OFFSET 0x580, TILE_SIZES 0x584, FILTER_BUFFER 0x588,
  SAO_BUFFER 0x58c. EXECUTE with AWAKEN as for H.264.
- Bitstream: the picture's VCL NAL units with 00 00 01 start codes
  (`sw_start_code_e` 1), `stream_len` = their length.
- The engine parses the slice segment headers itself except the RPS syntax:
  `sw_hdr_skip_length` = RBSP bits from pic_output_flag through the
  long-term ref syntax (all of it, incl. lt_idx_sps/poc_lsb_lt/msb cycles),
  `num_bits_short_term_ref_pics_in_slice`, `num_delta_pocs_of_rps_idx`.
- References: engine picture index = surface index. `RefDiffPicOrderCnts[idx]`
  = POC(cur) - POC(ref), `longtermflag` bit 15-idx, `initreflistidxl0/l1` =
  RefPicListTemp0/1 (StCurrBefore/After/LtCurr, repeated to 16 entries),
  `num_ref_frames` = NumPicTotalCurr, `curr_pic_idx` = current surface.
- Collocated MVs: one block per surface, w64*h64/16 bytes each
  (`colMvBuffersize` in 256-byte units).
- Filter buffer: left deblock / SAO / BSD regions (offsets in the pic struct,
  256-byte units), above-row regions via v1 HevcFltAboveOffset/
  HevcSaoAboveOffset; sized generously from the HEVC_*_BUF_SIZE macros.
- Tile sizes: (width, height) u16 pairs in CTBs per tile, raster order; one
  pair for the whole picture without tiles (Hantro G2 layout).
- Scaling lists: 6 DC (16x16) + 2 DC (32x32), 8 bytes pad, then 6x 4x4,
  6x 8x8, 6x 16x16 (8x8 base), 2x 32x32, each converted from diagonal to
  raster order and written column by column (Hantro G2 layout); defaults
  when the SPS enables lists without data.
- `pattern_id` 2 (from the NVK trace), init_qp includes 6*(bitdepth-8).

Driver-side: POC (prevTid0Pic), RPS derivation and DPB marking, NoRaslOutput
handling (decode starts at the next IRAP after create/reset/EOS; its RASL
pictures are dropped), pic_output_flag. Reorder depth =
sps_max_num_reorder_pics of the highest sub-layer.

Results (x265 / kvazaar streams, ffmpeg references), all bit-exact:
352x288 intra, IP, IPB B-pyramid, default scaling lists, custom asymmetric
scaling lists; 720p WPP; 720p 3x2 tiles (kvazaar); 720p 4 slices; 1080p
open GOP; 640x360 open GOP keyint 30; the same cut at its second CRA
(64 AUs -> 60 pictures). ~7 ms engine time per 1080p picture.

Through MediaCodec (`mcdec.c`, NDK; needs a binder thread pool for the codec
callbacks, and SELinux keeps a shell-domain client from sharing buffers with
the vendor codec, so it ran briefly permissive from the root console):
c2.rey.hevc.decoder is chosen for video/hevc; 1080p, open GOP and CRA-start
MP4s bit-exact, H.264 still bit-exact. Not covered: long-term references,
ref_pic_list_modification, inter-RPS prediction in the slice header (code
paths written, no test stream), Main10.
