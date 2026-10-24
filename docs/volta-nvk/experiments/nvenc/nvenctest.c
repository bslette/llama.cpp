/*
 * NVENC H.264 bring-up harness (Tegra194, upstream tegra-drm channel UAPI).
 * nvenctest in.yuv WxH frames out.h264 [recon.yuv]
 * I420 in, Annex B out. SPS/PPS are written here; the engine writes the slices.
 * recon.yuv gets the engine's reconstructed pictures, which a decoder must
 * reproduce exactly.
 * Knobs (environment): QP, GOP, HIGH (High profile: CABAC + 8x8 transform),
 * CLASS (0x21 / 0x22), DUMP (print status of every frame).
 */
#include "host1x.h"
#include "nvenc_drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOST1X_CLASS_NVENC 0x21

#define M_APP_ID           0x200
#define M_EXECUTE          0x300
#define M_IN_REF_LUMA(i)   (0x400 + 4 * (i))
#define M_IN_REF_CHROMA(i) (0x440 + 4 * (i))
#define M_CONTROL_PARAMS   0x700
#define M_PICTURE_INDEX    0x704
#define M_IN_DRV_PIC_SETUP 0x710
#define M_OUT_ENC_STATUS   0x718
#define M_OUT_BITSTREAM    0x71c
#define M_IOHISTORY        0x720
#define M_IO_RC_PROCESS    0x724
#define M_IN_COLOC         0x728
#define M_OUT_COLOC        0x72c
#define M_OUT_REF_LUMA     0x730
#define M_IN_CUR_PIC       0x734
#define M_IN_MEPRED        0x738
#define M_OUT_MEPRED       0x73c
#define M_IN_CUR_CHROMA_U  0x740
#define M_IN_CUR_CHROMA_V  0x744
#define M_OUT_REF_CHROMA   0x74c

#define MAGIC 0xC5B70006u /* NVENC 7.0 (Xavier firmware nvhost_nvenc070) */
/* surfaces: block linear, blocks of 2 GOBs (16 rows); the engine's block_height field is 2 for that */
#define BLK 1
#define BLK_FIELD 2

static int env(const char *n, int def) {
    const char *s = getenv(n);
    return s ? (int)strtol(s, NULL, 0) : def;
}

/* --- bit writer for SPS/PPS --- */
struct bw { uint8_t buf[256]; int bits; };
static void put(struct bw *b, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--, b->bits++)
        if ((v >> i) & 1) b->buf[b->bits / 8] |= 0x80 >> (b->bits % 8);
}
static void ue(struct bw *b, uint32_t v) {
    int n = 0;
    while ((v + 1) >> (n + 1)) n++;
    put(b, 0, n);
    put(b, v + 1, n + 1);
}
static void se(struct bw *b, int v) { ue(b, v <= 0 ? -2 * v : 2 * v - 1); }
static void nal(FILE *f, int type, struct bw *b) {
    put(b, 1, 1);
    while (b->bits % 8) put(b, 0, 1);
    uint8_t out[300] = {0, 0, 0, 1, (uint8_t)(0x60 | type)};
    int n = 5, zeros = 0;
    for (int i = 0; i < b->bits / 8; i++) {
        if (zeros >= 2 && b->buf[i] <= 3) { out[n++] = 3; zeros = 0; }
        out[n++] = b->buf[i];
        zeros = b->buf[i] ? 0 : zeros + 1;
    }
    fwrite(out, 1, n, f);
}

/* Block linear: GOBs of 64 B x 8 rows (16Bx2 sectors), blocks of 2^bh GOBs */
static size_t tbl(unsigned pitch, unsigned bh, unsigned x, unsigned y) {
    unsigned rows = 8u << bh;
    size_t block = (size_t)(y / rows) * (pitch / 64) + x / 64;
    return block * (512u << bh) + ((y % rows) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

struct pic { struct t_bo luma, chroma; };

static int pic_new(struct t_dev *d, struct pic *p, unsigned pitch, unsigned h16) {
    return t_bo_new(d, &p->luma, (size_t)pitch * ALIGN(h16, 8u << BLK)) ||
           t_bo_new(d, &p->chroma, (size_t)pitch * ALIGN(h16 / 2, 8u << BLK));
}

/* Encoder controls as NVIDIA's L4T driver sets them (r35, presets ultrafast/default). */
static void controls(nvenc_h264_me_control_s *me, nvenc_h264_md_control_s *md, nvenc_h264_quant_control_s *q,
                     int high) {
    me->refinement_mode = 1;
    me->refine_on_search_enable = 1;
    me->fps_mvcost = 1;
    me->sps_mvcost = 1;
    me->sps_cost_func = 1;
    me->limit_mv.mv_limit_enable = 1;
    me->limit_mv.left_mvx_int = -2048;
    me->limit_mv.top_mvy_int = -512;
    me->limit_mv.right_mvx_int = 2047;
    me->limit_mv.bottom_mvy_int = 511;
    me->predsrc.self_temporal_search = 1;
    me->predsrc.self_temporal_enable = 1;
    me->predsrc.coloc_search = 1;
    me->predsrc.coloc_enable = 1;
    me->predsrc.self_spatial_stamp_l0 = 1;
    me->predsrc.self_spatial_search = 1;
    me->predsrc.self_spatial_enable = 1;
    me->predsrc.external_stamp_l0_refidx0_stamp = 1;
    me->predsrc.const_mv_stamp_l0 = 3;
    me->predsrc.const_mv_search = 1;
    me->predsrc.const_mv_enable = 1;
    nvenc_h264_me_stamp_s *shape[4] = {&me->shape0, &me->shape1, &me->shape2, &me->shape3};
    for (int i = 0; i < 4; i++) {
        shape[i]->bitmask[0] = i < 3 ? 0xff0c0c00 : 0xff1e0c0c;
        shape[i]->bitmask[1] = i < 3 ? 0x00000c0c : 0x000c0c1e;
        shape[i]->hor_adjust = shape[i]->ver_adjust = 1;
    }
    me->spatial_hint_pattern = 15;
    me->temporal_hint_pattern = 63;
    me->fbm_op_winner_num_p_frame = high ? 1 : 4;
    me->fbm_op_winner_num_b_frame_l0 = high ? 1 : 2;
    me->fbm_op_winner_num_b_frame_l1 = high ? 1 : 2;
    me->fbm_select_best_cu32_parttype_num = 3;
    me->sps_evaluate_merge_cand_num = 5;
    me->external_hint_order = 1;
    me->coloc_hint_order = 1;
    me->hint_type0 = 0; me->hint_type1 = 1; me->hint_type2 = 2; me->hint_type3 = 3; me->hint_type4 = 4;
    if (high) {
        me->me_predictor_mode = 1;
        me->lambda_mode = 1;
        me->Cu16partDecisionMadeByFPP = 1;
        me->fps_quad_thresh_hold = 50;
    }

    md->intra_luma4x4_mode_enable = 0x1ff;
    md->intra_luma8x8_mode_enable = high ? 0x1ff : 0;
    md->intra_luma16x16_mode_enable = 0xf;
    md->intra_chroma_mode_enable = 0xf;
    md->l0_part_16x16_enable = md->l0_part_16x8_enable = md->l0_part_8x16_enable = md->l0_part_8x8_enable = 1;
    md->l0_part_8x4_enable = md->l0_part_4x8_enable = md->l0_part_4x4_enable = 1;
    md->l1_part_16x16_enable = md->l1_part_16x8_enable = md->l1_part_8x16_enable = md->l1_part_8x8_enable = 1;
    md->bi_part_16x16_enable = md->bi_part_16x8_enable = md->bi_part_8x16_enable = md->bi_part_8x8_enable = 1;
    md->bdirect_mode = 2;
    md->bskip_enable = 1;
    md->pskip_enable = 1;
    md->intra_nxn_bias_multiplier = 24;
    md->intra_most_prob_bias_multiplier = 4;
    md->tempmv_wt_spread_threshold = 0xffff;
    md->tempmv_wt_distort_threshold = 0xffff;
    md->mv_cost_enable = 1;
    md->early_intra_mode_type_16x16dc = md->early_intra_mode_type_16x16h = md->early_intra_mode_type_16x16v = 1;
    md->ip_search_mode = 7;
    md->intra_ssd_cnt_4x4 = high ? 1 : 9;
    md->intra_ssd_cnt_8x8 = high ? 2 : 9;
    md->intra_ssd_cnt_16x16 = high ? 2 : 4;
    md->skip_evaluate_enable = 1;
    md->rdo_level = 1;
    md->tu_search_num = 4;

    static const unsigned short dz_yi[16] = {1023, 878, 820, 682, 878, 820, 682, 512, 820, 682, 512, 410, 682, 512, 410, 410};
    static const unsigned short dz_yp[16] = {682, 586, 546, 456, 586, 546, 456, 342, 546, 456, 342, 292, 456, 342, 292, 274};
    q->qpp_run_vector_4x4 = 1387;
    q->qpp_run_vector_8x8[0] = 43775;
    q->qpp_run_vector_8x8[1] = 65450;
    q->qpp_run_vector_8x8[2] = 15;
    q->qpp_luma8x8_cost = q->qpp_luma16x16_cost = q->qpp_chroma_cost = 15;
    if (!high) {
        q->quant_intra_sat_flag = q->quant_inter_sat_flag = 1;
        q->quant_intra_sat_limit = q->quant_inter_sat_limit = 2063;
    }
    memcpy(q->dz_4x4_YI, dz_yi, sizeof(dz_yi));
    memcpy(q->dz_4x4_YP, dz_yp, sizeof(dz_yp));
    q->dz_4x4_CI = 682;
    q->dz_4x4_CP = 342;
    for (int i = 0; i < 16; i++) { q->dz_8x8_YI[i] = 682; q->dz_8x8_YP[i] = 342; }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s in.yuv WxH frames out.h264 [recon.yuv]\n", argv[0]); return 2; }
    unsigned w, h;
    if (sscanf(argv[2], "%ux%u", &w, &h) != 2 || w % 2 || h % 2) return 2;
    int frames = atoi(argv[3]);
    int qp = env("QP", 28), gop = env("GOP", 30), dump = env("DUMP", 0), high = env("HIGH", 0);
    unsigned mbw = (w + 15) / 16, mbh = (h + 15) / 16, mbs = mbw * mbh, w16 = mbw * 16, h16 = mbh * 16;
    int profile = high ? 100 : 66, level = env("LEVEL", 40);

    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[4], "wb"), *rec = argc > 5 ? fopen(argv[5], "wb") : NULL;
    if (!in || !out) { perror("open"); return 1; }

    struct t_dev d;
    if (t_open(&d, "/dev/dri/renderD128", env("CLASS", HOST1X_CLASS_NVENC))) return 1;

    unsigned pitch = ALIGN(w16, 64);
    struct pic cur, ref[2];
    struct t_bo setup, status, bits, hist, rc, coloc[2], mepred[2];
    size_t bits_size = 4u << 20;
    if (t_bo_new(&d, &setup, 64 << 10) || pic_new(&d, &cur, pitch, h16) || t_bo_new(&d, &status, 8 << 20) ||
        t_bo_new(&d, &bits, bits_size) || t_bo_new(&d, &hist, ALIGN(mbs * 256, 4096)) ||
        t_bo_new(&d, &rc, 4096))
        return 1;
    for (int i = 0; i < 2; i++)
        if (pic_new(&d, &ref[i], pitch, h16) || t_bo_new(&d, &coloc[i], ALIGN(mbs * 64, 4096)) ||
            t_bo_new(&d, &mepred[i], ALIGN(mbs * 64, 4096)))
            return 1;
    memset(rc.ptr, 0, rc.size);
    /* rate control: BITRATE (bit/s) selects macroblock-row CBR (VBR=1: VBR), else constant QP */
    int bitrate = env("BITRATE", 0), fps = env("FPS", 30), rcmode = bitrate ? (env("VBR", 0) ? 17 : 18) : 0;
    double cpb = bitrate * (env("CPB_MS", 2560) / 1000.0), pixels = (double)w16 * h16;
    if (bitrate) { /* persistent RC state, initialised as NVIDIA's driver does */
        nvenc_persistent_state_s *ps = rc.ptr;
        ps->nal_cpb_fullness = ps->vcl_cpb_fullness = (int)(cpb * 0.2 * 16);
        int wpbi[3] = {24, 19, 48};
        for (int i = 0; i < 3; i++) {
            ps->Wpbi[i] = wpbi[i];
            for (int t = 0; t < 3; t++) ps->Whist[i][t] = wpbi[i];
        }
        ps->np = (gop - 1) * 16;
        ps->prevQS = 256;
        ps->prevQP = 26;
    }

    /* SPS / PPS */
    struct bw b = {0};
    put(&b, profile, 8);
    put(&b, profile == 66 ? 0xc0 : 0, 8);
    put(&b, level, 8);
    ue(&b, 0);                 /* sps id */
    if (profile == 100) {
        ue(&b, 1);             /* chroma_format_idc */
        ue(&b, 0); ue(&b, 0);  /* bit depths */
        put(&b, 0, 1); put(&b, 0, 1); /* qpprime bypass, scaling matrix */
    }
    ue(&b, 4);                 /* log2_max_frame_num_minus4 */
    ue(&b, 0);                 /* poc type 0 */
    ue(&b, 4);                 /* log2_max_pic_order_cnt_lsb_minus4 */
    ue(&b, 1);                 /* max_num_ref_frames */
    put(&b, 0, 1);             /* gaps */
    ue(&b, mbw - 1);
    ue(&b, mbh - 1);
    put(&b, 1, 1);             /* frame_mbs_only */
    put(&b, 1, 1);             /* direct_8x8_inference */
    if (w != w16 || h != h16) {
        put(&b, 1, 1);
        ue(&b, 0); ue(&b, (w16 - w) / 2); ue(&b, 0); ue(&b, (h16 - h) / 2);
    } else {
        put(&b, 0, 1);
    }
    put(&b, 0, 1);             /* vui */
    nal(out, 7, &b);
    memset(&b, 0, sizeof(b));
    ue(&b, 0); ue(&b, 0);
    put(&b, high, 1);          /* entropy_coding_mode */
    put(&b, 0, 1);             /* bottom_field_pic_order_in_frame_present */
    ue(&b, 0);                 /* slice groups */
    ue(&b, 0); ue(&b, 0);      /* num_ref_idx default */
    put(&b, 0, 1); put(&b, 0, 2);
    se(&b, 0); se(&b, 0); se(&b, 0); /* pic_init_qp/qs, chroma qp offset */
    put(&b, 1, 1);             /* deblocking_filter_control_present */
    put(&b, 0, 1); put(&b, 0, 1);
    if (high) {
        put(&b, 1, 1);         /* transform_8x8_mode */
        put(&b, 0, 1);         /* pic_scaling_matrix_present */
        se(&b, 0);             /* second_chroma_qp_index_offset */
    }
    nal(out, 8, &b);

    uint8_t *yuv = malloc(w * h * 3 / 2), *rl = malloc((size_t)w16 * h16 * 3 / 2);
    double total_ms = 0;
    size_t total_bytes = 0;
    int f;
    for (f = 0; f < frames; f++) {
        if (fread(yuv, 1, w * h * 3 / 2, in) != w * h * 3 / 2) break;
        /* input, padded to whole macroblocks by repeating the last row/column */
        uint8_t *dl = cur.luma.ptr, *dc = cur.chroma.ptr;
        for (unsigned y = 0; y < h16; y++)
            for (unsigned x = 0; x < w16; x++)
                dl[tbl(pitch, BLK, x, y)] = yuv[(y < h ? y : h - 1) * w + (x < w ? x : w - 1)];
        const uint8_t *u = yuv + w * h, *v = u + (w / 2) * (h / 2);
        for (unsigned y = 0; y < h16 / 2; y++)
            for (unsigned x = 0; x < w16 / 2; x++) {
                unsigned sy = y < h / 2 ? y : h / 2 - 1, sx = x < w / 2 ? x : w / 2 - 1;
                dc[tbl(pitch, BLK, 2 * x, y)] = u[sy * (w / 2) + sx];
                dc[tbl(pitch, BLK, 2 * x + 1, y)] = v[sy * (w / 2) + sx];
            }

        int n = f % gop, idr = n == 0;
        int o = f & 1, prev = o ^ 1;

        memset(setup.ptr, 0, 8192);
        nvenc_h264_drv_pic_setup_s *s = setup.ptr;
        const unsigned off_me = 512, off_md = 768, off_slice = 1024, off_q = 5120;
        nvenc_h264_slice_control_s *sl = (void *)((uint8_t *)setup.ptr + off_slice);
        nvenc_h264_me_control_s *me = (void *)((uint8_t *)setup.ptr + off_me);
        nvenc_h264_md_control_s *md = (void *)((uint8_t *)setup.ptr + off_md);
        nvenc_h264_quant_control_s *q = (void *)((uint8_t *)setup.ptr + off_q);

        s->magic = MAGIC;
        s->input_cfg.frame_width_minus1 = w16 - 1;
        s->input_cfg.frame_height_minus1 = h16 - 1;
        s->input_cfg.sfc_pitch = pitch;
        s->input_cfg.sfc_pitch_chroma = pitch;
        s->input_cfg.block_height = BLK_FIELD;
        s->refpic_cfg = s->outputpic_cfg = s->input_cfg;

        s->sps_data.profile_idc = profile;
        s->sps_data.level_idc = level;
        s->sps_data.chroma_format_idc = 1;
        s->sps_data.log2_max_frame_num_minus4 = 4;
        s->sps_data.log2_max_pic_order_cnt_lsb_minus4 = 4;
        s->sps_data.frame_mbs_only = 1;

        s->pps_data.entropy_coding_mode_flag = high;
        s->pps_data.transform_8x8_mode_flag = high;
        s->pps_data.deblocking_filter_control_present_flag = 1;

        nvenc_h264_rc_s *r = &s->rate_control;
        r->hrd_type = 2;
        r->QP[0] = r->QP[1] = r->QP[2] = qp;
        if (bitrate) {
            r->QP[0] = 28; r->QP[1] = 30; r->QP[2] = 25;
            r->buffersize = r->nal_cpb_size = (int)cpb;
            r->nal_bitrate = bitrate;
            r->R = (int)(bitrate / (double)fps / pixels * 256);
            r->Bmin = (int)(cpb / pixels * 256 / 5);
            r->minQP[0] = r->minQP[1] = r->minQP[2] = env("MINQP", 0);
            r->maxQP[0] = r->maxQP[1] = r->maxQP[2] = env("MAXQP", 51);
        }
        r->maxQP[0] = r->maxQP[1] = r->maxQP[2] = 51;
        r->maxQPD = 6;
        r->baseQPD = 3;
        r->rhopbi[0] = 286; r->rhopbi[1] = 307; r->rhopbi[2] = 256;
        r->framerate = fps << 8;
        r->gop_length = gop;
        r->Np = gop - 1;
        r->iSizeRatioX = 2;
        r->iSizeRatioY = 1;

        nvenc_h264_pic_control_s *pc = &s->pic_control;
        memset(pc->l0, 0xfe, sizeof(pc->l0));
        memset(pc->l1, 0xfe, sizeof(pc->l1));
        memset(pc->temp_dist_l0, 0xff, sizeof(pc->temp_dist_l0));
        memset(pc->temp_dist_l1, 0xff, sizeof(pc->temp_dist_l1));
        memset(pc->dist_scale_factor, 0xff, sizeof(pc->dist_scale_factor));
        if (!idr) {
            pc->l0[0] = 0;
            pc->temp_dist_l0[0] = 1;
            for (int i = 0; i < 8; i++) pc->dist_scale_factor[i][0] = 256;
        }
        pc->max_slice_size = mbs;
        pc->frame_num = n & 0xff;
        pc->pic_order_cnt_lsb = (2 * n) & 0xff;
        pc->idr_pic_id = (f / gop) & 0xffff;
        pc->slice_control_offset = off_slice;
        pc->me_control_offset = off_me;
        pc->md_control_offset = off_md;
        pc->q_control_offset = off_q;
        pc->hist_buf_size = hist.size;
        pc->bitstream_buf_size = bits_size;
        pc->pic_type = idr ? 3 : 0;
        pc->ref_pic_flag = 1;
        pc->cabac_zero_word_enable = high;
        pc->slice_stat_offset = 128;
        pc->mpec_stat_offset = 128 + 16 * (mbs + 3);

        sl->num_mb = mbs;
        sl->qp_avr = bitrate ? (idr ? 25 : 28) : qp;
        sl->qp_slice_min = 1;
        sl->qp_slice_max = 51;

        controls(me, md, q, high);

        uint32_t cp = 3 /* H.264 */ | (1u << 8) /* force out pic */ | (1u << 11) /* slice stats */ |
                      ((uint32_t)rcmode << 24);
        memset(status.ptr, 0xcd, 4096);

        struct t_job j;
        t_job_init(&j);
        t_mthd(&j, M_CONTROL_PARAMS, cp);
        t_mthd(&j, M_PICTURE_INDEX, f);
        t_mthd(&j, M_APP_ID, 1);
        t_mthd_addr(&j, M_IN_DRV_PIC_SETUP, &setup, 0);
        t_mthd_addr(&j, M_OUT_ENC_STATUS, &status, 0);
        t_mthd_addr(&j, M_OUT_BITSTREAM, &bits, 0);
        t_mthd_addr(&j, M_IOHISTORY, &hist, 0);
        t_mthd_addr(&j, M_IO_RC_PROCESS, &rc, 0);
        t_mthd_addr(&j, M_IN_CUR_PIC, &cur.luma, 0);
        t_mthd_addr(&j, M_IN_CUR_CHROMA_U, &cur.chroma, 0);
        t_mthd_addr(&j, M_IN_CUR_CHROMA_V, &cur.chroma, 0);
        if (!idr) {
            t_mthd_addr(&j, M_IN_MEPRED, &mepred[prev], 0);
            t_mthd_addr(&j, M_OUT_MEPRED, &mepred[o], 0);
            t_mthd_addr(&j, M_IN_REF_LUMA(0), &ref[prev].luma, 0);
            t_mthd_addr(&j, M_IN_REF_CHROMA(0), &ref[prev].chroma, 0);
            t_mthd_addr(&j, M_IN_COLOC, &coloc[prev], 0);
        }
        t_mthd_addr(&j, M_OUT_COLOC, &coloc[o], 0);
        t_mthd_addr(&j, M_OUT_REF_LUMA, &ref[o].luma, 0);
        t_mthd_addr(&j, M_OUT_REF_CHROMA, &ref[o].chroma, 0);
        t_mthd(&j, M_EXECUTE, 1u << 8);
        t_syncpt_incr(&j, &d);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int timedout = t_submit_wait(&d, &j, env("TIMEOUT", 2000), NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        total_ms += ms;

        nvenc_pic_stat_s *st = status.ptr;
        unsigned nbytes = st->total_bit_count / 8;
        if (dump || f < 2 || timedout || st->error_status != 2) {
            fprintf(stderr, "frame %d %s %.2f ms%s: err %u ucode_err 0x%x bytes %u slices %u avgQP %u intra %u inter %u\n",
                    f, idr ? "IDR" : "P", ms, timedout ? " TIMEOUT" : "", st->error_status, st->ucode_error_status,
                    nbytes, st->num_slices, st->avgQP, st->intra_mb_count, st->inter_mb_count);
            const uint8_t *bp = bits.ptr;
            fprintf(stderr, "  bits:");
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", bp[i]);
            fprintf(stderr, "\n");
        }
        if (timedout || st->error_status != 2 || nbytes == 0 || nbytes > bits_size) return 1; /* 2: done */
        fwrite(bits.ptr, 1, nbytes, out);
        total_bytes += nbytes;

        if (rec) { /* reconstructed picture, cropped to WxH, I420 */
            const uint8_t *sl_ = ref[o].luma.ptr, *sc = ref[o].chroma.ptr;
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++) rl[y * w + x] = sl_[tbl(pitch, BLK, x, y)];
            uint8_t *ru = rl + w * h, *rv = ru + (w / 2) * (h / 2);
            for (unsigned y = 0; y < h / 2; y++)
                for (unsigned x = 0; x < w / 2; x++) {
                    ru[y * (w / 2) + x] = sc[tbl(pitch, BLK, 2 * x, y)];
                    rv[y * (w / 2) + x] = sc[tbl(pitch, BLK, 2 * x + 1, y)];
                }
            fwrite(rl, 1, w * h * 3 / 2, rec);
        }
    }
    fprintf(stderr, "%d frames, avg %.2f ms/frame (incl. submit), %.1f kbit/s at %d fps\n", f, f ? total_ms / f : 0,
            f ? total_bytes * 8.0 * fps / f / 1000 : 0, fps);
    fclose(out);
    if (rec) fclose(rec);
    return 0;
}
