/*
 * Decode an Annex B H.264 stream on the Tegra NVDEC engine through the upstream
 * tegra-drm channel UAPI, and write the frames in display order as I420.
 *
 * usage: nvdec_h264 in.h264 out.yuv [host1x class, default 0xf0]
 *
 * Progressive streams only (frame_mbs_only_flag), POC types 0 and 2, no scaling
 * matrices, 4:2:0 8-bit. Mirrors Mesa NVK's H.264 path (nvdec_drv.h picture setup
 * + NVC5B0 methods), sent through the THI method interface instead of a GPU channel.
 */
#include "host1x.h"
#include "nvdec_drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NVC5B0_SET_APPLICATION_ID          0x200
#define NVC5B0_EXECUTE                     0x300
#define NVC5B0_SET_CONTROL_PARAMS          0x400
#define NVC5B0_SET_DRV_PIC_SETUP_OFFSET    0x404
#define NVC5B0_SET_IN_BUF_BASE_OFFSET      0x408
#define NVC5B0_SET_PICTURE_INDEX           0x40c
#define NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET 0x410
#define NVC5B0_SET_COLOC_DATA_OFFSET       0x414
#define NVC5B0_SET_HISTORY_OFFSET          0x418
#define NVC5B0_SET_NVDEC_STATUS_OFFSET     0x424
#define NVC5B0_SET_PICTURE_LUMA_OFFSET0    0x430
#define NVC5B0_SET_PICTURE_CHROMA_OFFSET0  0x474
#define NVC5B0_H264_SET_MBHIST_BUF_OFFSET  0x500
#define APP_ID_H264 3
#define CODEC_H264 3

#define NSURF 17
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

static void die(const char *m) { fprintf(stderr, "error: %s\n", m); exit(1); }

/* ---- bit reader over RBSP (emulation prevention removed) ---- */
struct br { const uint8_t *p; size_t n, pos; };
static unsigned u1(struct br *b) {
    if (b->pos >= b->n * 8) die("bitstream overrun");
    unsigned v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static unsigned un(struct br *b, int n) { unsigned v = 0; while (n--) v = (v << 1) | u1(b); return v; }
static unsigned ue(struct br *b) {
    int z = 0;
    while (!u1(b)) if (++z > 31) die("bad exp-golomb");
    return ((1u << z) - 1) + un(b, z);
}
static int se(struct br *b) { unsigned k = ue(b); return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2); }
static int more_rbsp_data(struct br *b) {
    size_t last = b->n * 8;
    while (last > 0 && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) last--;
    return b->pos < last - 1;
}

static size_t unescape(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, z = 0;
    for (size_t i = 0; i < n; i++) {
        if (z >= 2 && in[i] == 3) { z = 0; continue; }
        z = in[i] == 0 ? z + 1 : 0;
        out[o++] = in[i];
    }
    return o;
}

/* ---- parameter sets ---- */
struct sps {
    int valid, profile_idc, chroma_format_idc, qpprime_bypass, log2_max_frame_num, poc_type,
        log2_max_poc_lsb, delta_pic_order_always_zero, max_num_ref_frames, width_mbs, height_map_units,
        frame_mbs_only, mbaff, direct_8x8;
};
struct pps {
    int valid, sps_id, entropy_coding_mode, bottom_field_pic_order_present, num_ref_idx_l0_default,
        num_ref_idx_l1_default, weighted_pred, weighted_bipred_idc, pic_init_qp_minus26,
        chroma_qp_index_offset, deblocking_filter_control_present, constrained_intra_pred,
        redundant_pic_cnt_present, transform_8x8_mode, second_chroma_qp_index_offset;
};
static struct sps spss[32];
static struct pps ppss[256];

static void skip_scaling_list(struct br *b, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next) next = (last + se(b) + 256) % 256;
        last = next ? next : last;
    }
}

static void parse_sps(struct br *b) {
    struct sps s = { .valid = 1, .chroma_format_idc = 1 };
    s.profile_idc = un(b, 8);
    un(b, 16); /* constraint flags, level */
    unsigned id = ue(b);
    if (id >= 32) die("sps id");
    int p = s.profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 ||
        p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        s.chroma_format_idc = ue(b);
        if (s.chroma_format_idc == 3) un(b, 1);
        if (ue(b) || ue(b)) die("only 8-bit streams are supported");
        s.qpprime_bypass = u1(b);
        if (u1(b)) die("scaling matrices are not supported");
    }
    if (s.chroma_format_idc != 1) die("only 4:2:0 is supported");
    s.log2_max_frame_num = ue(b) + 4;
    s.poc_type = ue(b);
    if (s.poc_type == 0) s.log2_max_poc_lsb = ue(b) + 4;
    else if (s.poc_type == 1) die("POC type 1 is not supported");
    s.max_num_ref_frames = ue(b);
    u1(b); /* gaps_in_frame_num_value_allowed_flag */
    s.width_mbs = ue(b) + 1;
    s.height_map_units = ue(b) + 1;
    s.frame_mbs_only = u1(b);
    if (!s.frame_mbs_only) die("interlaced streams are not supported");
    s.direct_8x8 = u1(b);
    spss[id] = s;
}

static void parse_pps(struct br *b) {
    struct pps s = { .valid = 1 };
    unsigned id = ue(b);
    if (id >= 256) die("pps id");
    s.sps_id = ue(b);
    s.entropy_coding_mode = u1(b);
    s.bottom_field_pic_order_present = u1(b);
    if (ue(b)) die("slice groups are not supported");
    s.num_ref_idx_l0_default = ue(b) + 1;
    s.num_ref_idx_l1_default = ue(b) + 1;
    s.weighted_pred = u1(b);
    s.weighted_bipred_idc = un(b, 2);
    s.pic_init_qp_minus26 = se(b);
    se(b); /* pic_init_qs_minus26 */
    s.chroma_qp_index_offset = se(b);
    s.deblocking_filter_control_present = u1(b);
    s.constrained_intra_pred = u1(b);
    s.redundant_pic_cnt_present = u1(b);
    s.second_chroma_qp_index_offset = s.chroma_qp_index_offset;
    if (more_rbsp_data(b)) {
        s.transform_8x8_mode = u1(b);
        if (u1(b)) die("scaling matrices are not supported");
        s.second_chroma_qp_index_offset = se(b);
    }
    ppss[id] = s;
}

/* ---- slice header ---- */
struct mmco { int op, diff, lt_pic_num, lt_idx, max_lt_idx; };
struct slice {
    int nal_ref_idc, idr, first_mb, type, pps_id, frame_num, poc_lsb, delta_poc_bottom;
    int no_output_prior, long_term_ref, adaptive, nmmco;
    struct mmco mmco[66];
};

static void parse_slice(struct br *b, int nal_type, int nal_ref_idc, struct slice *s) {
    memset(s, 0, sizeof(*s));
    s->nal_ref_idc = nal_ref_idc;
    s->idr = nal_type == 5;
    s->first_mb = ue(b);
    s->type = ue(b) % 5; /* 0 P, 1 B, 2 I, 3 SP, 4 SI */
    s->pps_id = ue(b);
    struct pps *pp = &ppss[s->pps_id];
    if (!pp->valid) die("slice refers to missing PPS");
    struct sps *sp = &spss[pp->sps_id];
    if (!sp->valid) die("slice refers to missing SPS");
    s->frame_num = un(b, sp->log2_max_frame_num);
    if (s->idr) ue(b); /* idr_pic_id */
    if (sp->poc_type == 0) {
        s->poc_lsb = un(b, sp->log2_max_poc_lsb);
        if (pp->bottom_field_pic_order_present) s->delta_poc_bottom = se(b);
    }
    if (pp->redundant_pic_cnt_present) ue(b);
    int P = s->type == 0 || s->type == 3, B = s->type == 1;
    int l0 = pp->num_ref_idx_l0_default, l1 = pp->num_ref_idx_l1_default;
    if (B) u1(b); /* direct_spatial_mv_pred_flag */
    if (P || B) {
        if (u1(b)) { l0 = ue(b) + 1; if (B) l1 = ue(b) + 1; }
    }
    /* ref_pic_list_modification */
    for (int list = 0; list < (B ? 2 : (P ? 1 : 0)); list++) {
        if (u1(b)) {
            unsigned idc;
            while ((idc = ue(b)) != 3) {
                if (idc > 5) die("bad modification_of_pic_nums_idc");
                ue(b);
            }
        }
    }
    /* pred_weight_table */
    if ((pp->weighted_pred && P) || (pp->weighted_bipred_idc == 1 && B)) {
        ue(b);
        ue(b); /* chroma_log2_weight_denom (ChromaArrayType != 0) */
        for (int list = 0; list < (B ? 2 : 1); list++) {
            for (int i = 0; i < (list ? l1 : l0); i++) {
                if (u1(b)) { se(b); se(b); }
                if (u1(b)) { se(b); se(b); se(b); se(b); }
            }
        }
    }
    if (nal_ref_idc) {
        if (s->idr) {
            s->no_output_prior = u1(b);
            s->long_term_ref = u1(b);
        } else if ((s->adaptive = u1(b))) {
            unsigned op;
            while ((op = ue(b)) != 0) {
                struct mmco *m = &s->mmco[s->nmmco++];
                m->op = op;
                if (op == 1 || op == 3) m->diff = ue(b) + 1;
                if (op == 2) m->lt_pic_num = ue(b);
                if (op == 3 || op == 6) m->lt_idx = ue(b);
                if (op == 4) m->max_lt_idx = ue(b);
                if (s->nmmco >= 64) die("too many MMCOs");
            }
        }
    }
}

/* ---- decoder state ---- */
struct ref { int used, surf, dpb, long_term, frame_num, lt_idx, poc; };
struct frame { int period, poc; uint8_t *yuv; };

static struct t_dev dev;
static struct t_bo surf[NSURF], bits, setup, offsets, status, coloc, mbhist, history;
static unsigned pitch, luma_h, chroma_h, luma_size, surf_size;
static struct ref refs[16];
static struct frame *frames;
static int nframes, cap_frames;
static int period = -1, prev_poc_msb, prev_poc_lsb, prev_frame_num, prev_frame_num_offset;
static unsigned pic_counter;

static void alloc_buffers(const struct sps *sp) {
    unsigned w = sp->width_mbs * 16, h = sp->height_map_units * 16;
    pitch = ALIGN(w, 64);
    luma_h = ALIGN(h, 32);
    chroma_h = ALIGN(h / 2, 32);
    luma_size = ALIGN(pitch * luma_h, 4096);
    surf_size = luma_size + ALIGN(pitch * chroma_h, 4096);
    for (int i = 0; i < NSURF; i++)
        if (t_bo_new(&dev, &surf[i], surf_size)) die("surface alloc");
    unsigned coloc_size = ALIGN(ALIGN(sp->height_map_units, 2) * sp->width_mbs * 64 - 63, 0x100);
    if (t_bo_new(&dev, &coloc, (size_t)coloc_size * NSURF) ||
        t_bo_new(&dev, &mbhist, ALIGN(sp->width_mbs * 104, 0x100)) ||
        t_bo_new(&dev, &history, ALIGN(sp->width_mbs * 0x300, 0x200)) ||
        t_bo_new(&dev, &bits, 8 << 20) || t_bo_new(&dev, &setup, sizeof(nvdec_h264_pic_s)) ||
        t_bo_new(&dev, &offsets, 4096) || t_bo_new(&dev, &status, 4096))
        die("buffer alloc");
}

/* KBL block linear, GOB 64 B x 8 rows, blocks of 2 GOBs (gob_height = 0) */
static inline size_t bl_offset(unsigned x, unsigned y) {
    size_t block = (size_t)(y / 16) * (pitch / 64) + x / 64;
    return block * 1024 + ((y % 16) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

static uint8_t *read_frame(int s, unsigned w, unsigned h) {
    uint8_t *out = malloc(w * h * 3 / 2), *base = surf[s].ptr;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            out[y * w + x] = base[bl_offset(x, y)];
    uint8_t *u = out + w * h, *v = u + w * h / 4, *c = base + luma_size;
    for (unsigned y = 0; y < h / 2; y++)
        for (unsigned x = 0; x < w / 2; x++) {
            u[y * (w / 2) + x] = c[bl_offset(2 * x, y)];
            v[y * (w / 2) + x] = c[bl_offset(2 * x + 1, y)];
        }
    return out;
}

static int surf_busy(int s) {
    for (int i = 0; i < 16; i++) if (refs[i].used && refs[i].surf == s) return 1;
    return 0;
}

static int pic_num(const struct ref *r, int cur_frame_num, int max_frame_num) {
    return r->frame_num > cur_frame_num ? r->frame_num - max_frame_num : r->frame_num;
}

static void mark_refs(const struct slice *s, const struct sps *sp, int cur_surf, int cur_poc) {
    int max_fn = 1 << sp->log2_max_frame_num;
    int cur_long = -1;
    if (s->idr) {
        memset(refs, 0, sizeof(refs));
        if (s->long_term_ref) cur_long = 0;
    } else if (s->adaptive) {
        for (int k = 0; k < s->nmmco; k++) {
            const struct mmco *m = &s->mmco[k];
            for (int i = 0; i < 16; i++) {
                struct ref *r = &refs[i];
                if (!r->used) continue;
                int st_hit = !r->long_term && pic_num(r, s->frame_num, max_fn) == s->frame_num - m->diff;
                switch (m->op) {
                case 1: if (st_hit) r->used = 0; break;
                case 2: if (r->long_term && r->lt_idx == m->lt_pic_num) r->used = 0; break;
                case 3:
                    if (r->long_term && r->lt_idx == m->lt_idx) r->used = 0;
                    break;
                case 4: if (r->long_term && r->lt_idx >= m->max_lt_idx) r->used = 0; break;
                case 5: r->used = 0; break;
                case 6: if (r->long_term && r->lt_idx == m->lt_idx) r->used = 0; break;
                }
            }
            if (m->op == 3)
                for (int i = 0; i < 16; i++)
                    if (refs[i].used && !refs[i].long_term &&
                        pic_num(&refs[i], s->frame_num, max_fn) == s->frame_num - m->diff) {
                        refs[i].long_term = 1;
                        refs[i].lt_idx = m->lt_idx;
                    }
            if (m->op == 5) fprintf(stderr, "warning: MMCO 5 POC reset not handled\n");
            if (m->op == 6) cur_long = m->lt_idx;
        }
    } else {
        int n = 0, oldest = -1;
        for (int i = 0; i < 16; i++) {
            if (!refs[i].used) continue;
            n++;
            if (!refs[i].long_term &&
                (oldest < 0 || pic_num(&refs[i], s->frame_num, max_fn) < pic_num(&refs[oldest], s->frame_num, max_fn)))
                oldest = i;
        }
        int max = sp->max_num_ref_frames ? sp->max_num_ref_frames : 1;
        if (n >= max && oldest >= 0) refs[oldest].used = 0;
    }
    /* the current picture becomes a reference in a free DPB slot */
    for (int i = 0; i < 16; i++) {
        if (refs[i].used) continue;
        refs[i] = (struct ref){ .used = 1, .surf = cur_surf, .dpb = i, .frame_num = s->frame_num,
                                .long_term = cur_long >= 0, .lt_idx = cur_long, .poc = cur_poc };
        return;
    }
    die("DPB full");
}

static void decode_picture(const uint8_t *bs, size_t bs_len, const unsigned *slice_off, int nslices,
                           const struct slice *s) {
    struct pps *pp = &ppss[s->pps_id];
    struct sps *sp = &spss[pp->sps_id];
    if (!surf[0].ptr) alloc_buffers(sp);

    /* POC */
    int poc;
    if (s->idr) period++;
    if (sp->poc_type == 0) {
        int max_lsb = 1 << sp->log2_max_poc_lsb, msb;
        if (s->idr) prev_poc_msb = prev_poc_lsb = 0;
        if (s->poc_lsb < prev_poc_lsb && prev_poc_lsb - s->poc_lsb >= max_lsb / 2) msb = prev_poc_msb + max_lsb;
        else if (s->poc_lsb > prev_poc_lsb && s->poc_lsb - prev_poc_lsb > max_lsb / 2) msb = prev_poc_msb - max_lsb;
        else msb = prev_poc_msb;
        poc = msb + s->poc_lsb;
        if (s->nal_ref_idc) { prev_poc_msb = msb; prev_poc_lsb = s->poc_lsb; }
    } else {
        int max_fn = 1 << sp->log2_max_frame_num, off;
        if (s->idr) off = 0;
        else if (prev_frame_num > s->frame_num) off = prev_frame_num_offset + max_fn;
        else off = prev_frame_num_offset;
        poc = s->idr ? 0 : 2 * (off + s->frame_num) - (s->nal_ref_idc ? 0 : 1);
        prev_frame_num = s->frame_num;
        prev_frame_num_offset = off;
    }
    int bottom_poc = poc + s->delta_poc_bottom;

    /* current surface: one no reference uses */
    int cur = -1;
    for (int i = 0; i < NSURF && cur < 0; i++) if (!surf_busy(i)) cur = i;
    if (cur < 0) die("no free surface");
    if (s->idr) memset(refs, 0, sizeof(refs));

    /* bitstream and slice offsets */
    if (bs_len + 64 > bits.size) die("picture too large");
    memcpy(bits.ptr, bs, bs_len);
    unsigned *so = offsets.ptr;
    memset(so, 0, 256 * 4);
    for (int i = 0; i < nslices; i++) so[i] = slice_off[i];
    so[nslices] = bs_len;

    nvdec_h264_pic_s *h = setup.ptr;
    memset(h, 0, sizeof(*h));
    static const uint8_t eos[16] = { 0, 0, 1, 0xb, 0, 0, 0, 0, 0, 0, 1, 0xb, 0, 0, 0, 0 };
    memcpy(h->eos, eos, 16);
    h->explicitEOSPresentFlag = 1;
    h->gptimer_timeout_value = 81000000;
    h->slice_count = nslices;
    h->stream_len = bs_len + 16;
    h->mbhist_buffer_size = mbhist.size;
    h->log2_max_pic_order_cnt_lsb_minus4 = sp->poc_type == 0 ? sp->log2_max_poc_lsb - 4 : 0;
    h->delta_pic_order_always_zero_flag = sp->delta_pic_order_always_zero;
    h->frame_mbs_only_flag = sp->frame_mbs_only;
    h->PicWidthInMbs = sp->width_mbs;
    h->FrameHeightInMbs = sp->height_map_units;
    h->tileFormat = getenv("NVDEC_TILE") ? atoi(getenv("NVDEC_TILE")) : 0; /* TBL: Tegra NVDEC4 ignores KBL */
    h->gob_height = getenv("NVDEC_GOBH") ? atoi(getenv("NVDEC_GOBH")) : 0;
    h->entropy_coding_mode_flag = pp->entropy_coding_mode;
    h->pic_order_present_flag = pp->bottom_field_pic_order_present;
    h->num_ref_idx_l0_active_minus1 = pp->num_ref_idx_l0_default - 1;
    h->num_ref_idx_l1_active_minus1 = pp->num_ref_idx_l1_default - 1;
    h->deblocking_filter_control_present_flag = pp->deblocking_filter_control_present;
    h->redundant_pic_cnt_present_flag = pp->redundant_pic_cnt_present;
    h->transform_8x8_mode_flag = pp->transform_8x8_mode;
    h->pitch_luma = pitch;
    h->pitch_chroma = pitch;
    h->luma_bot_offset = sp->width_mbs * 16;
    h->chroma_bot_offset = pitch / 2;
    h->HistBufferSize = history.size >> 8;
    h->MbaffFrameFlag = 0;
    h->direct_8x8_inference_flag = sp->direct_8x8;
    h->weighted_pred_flag = pp->weighted_pred;
    h->constrained_intra_pred_flag = pp->constrained_intra_pred;
    h->ref_pic_flag = s->nal_ref_idc != 0;
    h->log2_max_frame_num_minus4 = sp->log2_max_frame_num - 4;
    h->chroma_format_idc = sp->chroma_format_idc;
    h->pic_order_cnt_type = sp->poc_type;
    h->pic_init_qp_minus26 = pp->pic_init_qp_minus26;
    h->chroma_qp_index_offset = pp->chroma_qp_index_offset;
    h->second_chroma_qp_index_offset = pp->second_chroma_qp_index_offset;
    h->weighted_bipred_idc = pp->weighted_bipred_idc;
    h->CurrPicIdx = cur;
    h->CurrColIdx = cur;
    h->frame_num = s->frame_num;
    h->CurrFieldOrderCnt[0] = poc;
    h->CurrFieldOrderCnt[1] = bottom_poc;
    memset(h->WeightScale, 16, sizeof(h->WeightScale));
    memset(h->WeightScale8x8, 16, sizeof(h->WeightScale8x8));
    h->qpprime_y_zero_transform_bypass_flag = sp->qpprime_bypass;
    for (int i = 0; i < 16; i++) {
        struct ref *r = &refs[i];
        if (!r->used) continue;
        nvdec_dpb_entry_s *e = &h->dpb[r->dpb];
        int marking = r->long_term ? 2 : 1;
        e->index = r->surf;
        e->col_idx = r->surf;
        e->state = 3;
        e->top_field_marking = marking;
        e->bottom_field_marking = marking;
        e->is_long_term = r->long_term;
        e->FieldOrderCnt[0] = r->poc;
        e->FieldOrderCnt[1] = r->poc;
        e->FrameIdx = r->long_term ? r->lt_idx : r->frame_num;
    }
    memset(status.ptr, 0, sizeof(nvdec_status_s));

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, NVC5B0_SET_APPLICATION_ID, APP_ID_H264);
    unsigned max_refs = sp->max_num_ref_frames ? sp->max_num_ref_frames : 1;
    t_mthd(&j, NVC5B0_SET_CONTROL_PARAMS,
           CODEC_H264 | 1 << 4 /* gptimer */ | 1 << 6 /* err conceal */ |
           ((pic_counter % max_refs) & 0x3f) << 7 | 1 << 13 /* mbtimer */);
    t_mthd_addr(&j, NVC5B0_SET_DRV_PIC_SETUP_OFFSET, &setup, 0);
    t_mthd_addr(&j, NVC5B0_SET_IN_BUF_BASE_OFFSET, &bits, 0);
    t_mthd(&j, NVC5B0_SET_PICTURE_INDEX, pic_counter);
    t_mthd_addr(&j, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, &offsets, 0);
    t_mthd_addr(&j, NVC5B0_SET_COLOC_DATA_OFFSET, &coloc, 0);
    t_mthd_addr(&j, NVC5B0_SET_HISTORY_OFFSET, &history, 0);
    t_mthd_addr(&j, NVC5B0_SET_NVDEC_STATUS_OFFSET, &status, 0);
    for (int i = 0; i < NSURF; i++) t_mthd_addr(&j, NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i, &surf[i], 0);
    for (int i = 0; i < NSURF; i++) t_mthd_addr(&j, NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i, &surf[i], luma_size);
    t_mthd_addr(&j, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, &mbhist, 0);
    t_mthd(&j, NVC5B0_EXECUTE, 1 << 8); /* AWAKEN: on host1x the engine reports OP_DONE through it */
    t_syncpt_incr(&j, &dev);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (t_submit_wait(&dev, &j, 2000, NULL)) die("decode timed out");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    nvdec_status_s *st = status.ptr;
    int mbs = sp->width_mbs * sp->height_map_units;
    printf("pic %3u %c poc %4d frame_num %3d surf %2d: mbs ok %d/%d err %u status 0x%x slice_hdr_err 0x%x cycles %u, %.2f ms\n",
           pic_counter, "PBIPI"[s->type], poc, s->frame_num, cur, st->mbs_correctly_decoded, mbs,
           st->mbs_in_error, st->error_status, st->slice_header_error_code, st->cycle_count,
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

    if (pic_counter == 0 && getenv("NVDEC_DUMP")) {
        FILE *df = fopen(getenv("NVDEC_DUMP"), "wb");
        fwrite(surf[cur].ptr, 1, surf_size, df);
        fclose(df);
        printf("dumped surface %d: pitch %u, luma %u bytes, chroma at %u\n", cur, pitch, luma_size, luma_size);
    }
    if (nframes == cap_frames) frames = realloc(frames, (cap_frames = cap_frames * 2 + 16) * sizeof(*frames));
    frames[nframes++] = (struct frame){ period, poc, read_frame(cur, sp->width_mbs * 16, sp->height_map_units * 16) };
    if (s->nal_ref_idc) mark_refs(s, sp, cur, poc);
    pic_counter++;
}

static int cmp_frame(const void *a, const void *b) {
    const struct frame *x = a, *y = b;
    return x->period != y->period ? x->period - y->period : x->poc - y->poc;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.h264 out.yuv [class]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) die("open input");
    fseek(f, 0, SEEK_END);
    size_t n = ftell(f);
    rewind(f);
    uint8_t *in = malloc(n);
    if (fread(in, 1, n, f) != n) die("read input");
    fclose(f);
    if (t_open(&dev, "/dev/dri/renderD128", argc > 3 ? strtoul(argv[3], NULL, 0) : HOST1X_CLASS_NVDEC)) return 1;
    printf("NVDEC channel version 0x%x, sizeof(nvdec_h264_pic_s) = %zu\n", dev.version, sizeof(nvdec_h264_pic_s));

    /* split into NAL units */
    size_t starts[1 << 16], nn = 0;
    for (size_t i = 0; i + 3 <= n; i++)
        if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) { starts[nn++] = i + 3; i += 2; if (nn == 65536) die("too many NALs"); }

    uint8_t *pic = malloc(8 << 20), *rbsp = malloc(1 << 20);
    size_t pic_len = 0;
    unsigned slice_off[256];
    int nslices = 0;
    struct slice first = {0}, cur;
    for (size_t k = 0; k <= nn; k++) {
        size_t beg = k < nn ? starts[k] : n, end = k + 1 < nn ? starts[k + 1] - 3 : n;
        while (k + 1 < nn && end > beg && in[end - 1] == 0) end--; /* trailing zero (4-byte start code) */
        int type = k < nn ? in[beg] & 0x1f : -1;
        int is_slice = type == 1 || type == 5;
        if (is_slice) {
            size_t rn = unescape(in + beg + 1, end - beg - 1 < 512 ? end - beg - 1 : 512, rbsp);
            struct br b = { rbsp, rn, 0 };
            parse_slice(&b, type, (in[beg] >> 5) & 3, &cur);
        }
        /* a new picture starts at a slice with first_mb_in_slice == 0, or at the end */
        if (nslices && (k == nn || (is_slice && cur.first_mb == 0) || type == 7 || type == 8 || type == 9)) {
            decode_picture(pic, pic_len, slice_off, nslices, &first);
            pic_len = 0;
            nslices = 0;
        }
        if (k == nn) break;
        if (type == 7 || type == 8) {
            size_t rn = unescape(in + beg + 1, end - beg - 1, rbsp);
            struct br b = { rbsp, rn, 0 };
            if (type == 7) parse_sps(&b); else parse_pps(&b);
        } else if (is_slice) {
            if (nslices == 0) first = cur;
            if (nslices == 255) die("too many slices");
            slice_off[nslices++] = pic_len;
            static const uint8_t sc[3] = { 0, 0, 1 };
            memcpy(pic + pic_len, sc, 3);
            memcpy(pic + pic_len + 3, in + beg, end - beg);
            pic_len += 3 + end - beg;
        }
    }

    qsort(frames, nframes, sizeof(*frames), cmp_frame);
    FILE *o = fopen(argv[2], "wb");
    struct sps *sp = &spss[ppss[first.pps_id].sps_id];
    size_t fsz = sp->width_mbs * 16 * sp->height_map_units * 16 * 3 / 2;
    for (int i = 0; i < nframes; i++) fwrite(frames[i].yuv, 1, fsz, o);
    fclose(o);
    printf("wrote %d frames (%ux%u) in display order\n", nframes, sp->width_mbs * 16, sp->height_map_units * 16);
    return 0;
}
