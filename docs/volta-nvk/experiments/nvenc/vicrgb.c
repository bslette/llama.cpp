/*
 * VIC RGB(A) pitch-linear -> NV12 block linear (2 GOBs per block), the NVENC
 * input layout. Prints Y/U/V of 8 colour bars for each output colour matrix tried.
 * vicrgb [FMT] [MATRIX: 0 none, 1 fixed S?.SHIFT test]  (SHIFT via env)
 */
#include "host1x.h"
#include "vic42.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIC_APP_ID                       0x200
#define VIC_EXECUTE                      0x300
#define VIC_SET_CONTROL_PARAMS           0x704
#define VIC_SET_CONFIG_STRUCT_OFFSET     0x708
#define VIC_SET_FILTER_STRUCT_OFFSET     0x70c
#define VIC_SET_OUTPUT_LUMA_OFFSET       0x720
#define VIC_SET_OUTPUT_CHROMA_U_OFFSET   0x724
#define VIC_SET_SURFACE0_LUMA_OFFSET     0x1200

static int env(const char *n, int def) {
    const char *s = getenv(n);
    return s ? (int)strtol(s, NULL, 0) : def;
}

static size_t tbl(unsigned pitch, unsigned bh, unsigned x, unsigned y) {
    unsigned rows = 8u << bh;
    size_t block = (size_t)(y / rows) * (pitch / 64) + x / 64;
    return block * (512u << bh) + ((y % rows) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

static void setm(MatrixStruct *m, const int c[3][4], int shift) {
    m->matrix_coeff00 = c[0][0] & 0xfffff; m->matrix_coeff01 = c[0][1] & 0xfffff;
    m->matrix_coeff02 = c[0][2] & 0xfffff; m->matrix_coeff03 = c[0][3] & 0xfffff;
    m->matrix_coeff10 = c[1][0] & 0xfffff; m->matrix_coeff11 = c[1][1] & 0xfffff;
    m->matrix_coeff12 = c[1][2] & 0xfffff; m->matrix_coeff13 = c[1][3] & 0xfffff;
    m->matrix_coeff20 = c[2][0] & 0xfffff; m->matrix_coeff21 = c[2][1] & 0xfffff;
    m->matrix_coeff22 = c[2][2] & 0xfffff; m->matrix_coeff23 = c[2][3] & 0xfffff;
    m->matrix_r_shift = shift;
    m->matrix_enable = 1;
}

int main(int argc, char **argv) {
    int fmt = argc > 1 ? atoi(argv[1]) : 31, mode = argc > 2 ? atoi(argv[2]) : 0;
    unsigned w = 256, h = 64, spitch = w * 4, opitch = 256;
    struct t_dev d;
    if (t_open(&d, "/dev/dri/renderD128", 0x5d)) return 1;
    struct t_bo cfg, filt, src, luma, chroma;
    if (t_bo_new(&d, &cfg, sizeof(ConfigStruct)) || t_bo_new(&d, &filt, 16384) || t_bo_new(&d, &src, spitch * h) ||
        t_bo_new(&d, &luma, opitch * h) || t_bo_new(&d, &chroma, opitch * h / 2))
        return 1;
    /* bars of 32 px: white yellow cyan green magenta red blue black (byte order R,G,B,A) */
    static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                       {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
    uint8_t *s = src.ptr;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *c = bars[x / 32];
            uint8_t *p = s + y * spitch + x * 4;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 255;
        }
    memset(luma.ptr, 0xee, luma.size);
    memset(chroma.ptr, 0xee, chroma.size);

    ConfigStruct *c = cfg.ptr;
    memset(c, 0, sizeof(*c));
    c->outputConfig.TargetRectRight = w - 1;
    c->outputConfig.TargetRectBottom = h - 1;
    c->outputConfig.BackgroundAlpha = 255;
    c->outputSurfaceConfig.OutPixelFormat = 67; /* NV12 */
    c->outputSurfaceConfig.OutBlkKind = env("OBK", 1);
    c->outputSurfaceConfig.OutBlkHeight = env("OBH", 1);
    c->outputSurfaceConfig.OutSurfaceWidth = w - 1;
    c->outputSurfaceConfig.OutSurfaceHeight = h - 1;
    c->outputSurfaceConfig.OutLumaWidth = opitch - 1;
    c->outputSurfaceConfig.OutLumaHeight = h - 1;
    c->outputSurfaceConfig.OutChromaWidth = opitch / 2 - 1;
    c->outputSurfaceConfig.OutChromaHeight = h / 2 - 1;
    if (mode == 1) {
        /* BT.601 limited range, RGB in 0..1 -> Y'CbCr in 0..1, offsets in 1/1023 units scaled */
        int sh = env("SHIFT", 16);
        double k[3][4] = {{0.2568, 0.5041, 0.0979, 16.0 / 255},
                          {-0.1482, -0.2910, 0.4392, 128.0 / 255},
                          {0.4392, -0.3678, -0.0714, 128.0 / 255}};
        int ci[3][4];
        double oscale = env("OSCALE", 1023);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) ci[i][j] = (int)(k[i][j] * (1 << sh) + (k[i][j] < 0 ? -0.5 : 0.5));
            ci[i][3] = (int)(k[i][3] * oscale * (1 << sh) / env("ODIV", 1) + 0.5);
        }
        setm(&c->outColorMatrixStruct, ci, env("RSHIFT", 0));
    }

    if (mode == 2) { /* M="c00 c01 c02 c03 c10 ... c23" raw coefficients */
        int ci[3][4] = {{0}};
        const char *m = getenv("M");
        for (int i = 0; m && i < 12; i++) ci[i / 4][i % 4] = (int)strtol(m, (char **)&m, 0);
        setm(&c->outColorMatrixStruct, ci, env("RSHIFT", 0));
    }
    SlotConfig *slot = &c->slotStruct[0].slotConfig;
    slot->SlotEnable = 1;
    slot->CurrentFieldEnable = 1;
    slot->PlanarAlpha = 255;
    slot->ConstantAlpha = 1;
    slot->SourceRectRight = (w - 1) << 16;
    slot->SourceRectBottom = (h - 1) << 16;
    slot->DestRectRight = w - 1;
    slot->DestRectBottom = h - 1;
    slot->SoftClampHigh = 1023;
    SlotSurfaceConfig *ss = &c->slotStruct[0].slotSurfaceConfig;
    ss->SlotPixelFormat = fmt;
    ss->SlotBlkKind = 0;
    ss->SlotCacheWidth = 2; /* 64Bx4 */
    ss->SlotSurfaceWidth = w - 1;
    ss->SlotSurfaceHeight = h - 1;
    ss->SlotLumaWidth = env("SPW", w) - 1;
    ss->SlotLumaHeight = h - 1;

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, VIC_APP_ID, 1);
    t_mthd(&j, VIC_SET_CONTROL_PARAMS, (sizeof(ConfigStruct) / 16) << 16);
    t_mthd_addr(&j, VIC_SET_CONFIG_STRUCT_OFFSET, &cfg, 0);
    t_mthd_addr(&j, VIC_SET_FILTER_STRUCT_OFFSET, &filt, 0);
    t_mthd_addr(&j, VIC_SET_OUTPUT_LUMA_OFFSET, &luma, 0);
    t_mthd_addr(&j, VIC_SET_OUTPUT_CHROMA_U_OFFSET, &chroma, 0);
    t_mthd_addr(&j, VIC_SET_SURFACE0_LUMA_OFFSET, &src, 0);
    t_mthd(&j, VIC_EXECUTE, 1u << 8);
    t_syncpt_incr(&j, &d);
    if (t_submit_wait(&d, &j, 1000, NULL)) { fprintf(stderr, "timeout\n"); return 1; }
    const uint8_t *L = luma.ptr, *C = chroma.ptr;
    if (env("OBK", 1) == 0) { printf("pitch Y:"); for (int b = 0; b < 8; b++) printf(" %u", L[20 * opitch + b * 32 + 8]); printf("\n"); }
    int bh = env("BH", 1);
    printf("fmt %d mode %d:", fmt, mode);
    for (int b = 0; b < 8; b++) {
        unsigned x = b * 32 + 8, y = 20;
        printf(" [%3u %3u %3u]", L[tbl(opitch, bh, x, y)], C[tbl(opitch, bh, x & ~1u, y / 2)],
               C[tbl(opitch, bh, (x & ~1u) + 1, y / 2)]);
    }
    printf("\n");
    return 0;
}
