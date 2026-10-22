/*
 * h264dec_cli in.h264 out.yuv [render node]: decode with h264dec, feeding one
 * access unit per call like MediaCodec, and write I420 frames in display order.
 */
#include "h264dec.h"
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
typedef struct { int version, numFds, numInts, data[0]; } nh_t;
extern const nh_t *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *);
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct frame { int period, poc; uint8_t *yuv; };
static struct frame *frames;
static int nframes, cap;
static size_t frame_size;
static unsigned out_w, out_h;
static double cb_ms;
static struct h264dec *g_dec;
static int use_vic;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int on_picture(void *opaque, const struct h264dec_picture *pic) {
    (void)opaque;
    double t = now_ms();
    if (!frame_size) {
        out_w = pic->width;
        out_h = pic->height;
        frame_size = (size_t)out_w * out_h * 3 / 2;
    }
    if (nframes == cap) frames = realloc(frames, (cap = cap * 2 + 16) * sizeof(*frames));
    uint8_t *yuv = malloc(frame_size);
    uint8_t *u = yuv + out_w * out_h, *v = u + out_w * out_h / 4;
#ifdef __ANDROID__
    if (use_vic) {
        /* through a gralloc NV12 buffer, as the codec component does */
        AHardwareBuffer_Desc desc = { pic->width, pic->height, 1, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,
                                      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 0, 0, 0 };
        AHardwareBuffer *b;
        if (AHardwareBuffer_allocate(&desc, &b)) { fprintf(stderr, "AHB alloc failed\n"); return 1; }
        AHardwareBuffer_Planes pl;
        AHardwareBuffer_lockPlanes(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &pl);
        unsigned pitch = pl.planes[0].rowStride;
        unsigned coff = (uint8_t *)pl.planes[1].data - (uint8_t *)pl.planes[0].data;
        AHardwareBuffer_unlock(b, NULL);
        const nh_t *nh = AHardwareBuffer_getNativeHandle(b);
        size_t size = (size_t)coff + pitch * pic->height / 2;
        int err = h264dec_vic_copy(g_dec, pic, nh->data[0], size, pitch, coff);
        if (err) { fprintf(stderr, "vic copy failed %d (pitch %u, chroma at %u)\n", err, pitch, coff); return 1; }
        AHardwareBuffer_lockPlanes(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &pl);
        for (unsigned r = 0; r < pic->height; r++)
            memcpy(yuv + r * out_w, (uint8_t *)pl.planes[0].data + (size_t)r * pl.planes[0].rowStride, out_w);
        for (unsigned r = 0; r < pic->height / 2; r++)
            for (unsigned x = 0; x < out_w / 2; x++) {
                u[r * out_w / 2 + x] = ((uint8_t *)pl.planes[1].data)[(size_t)r * pl.planes[1].rowStride + x * pl.planes[1].pixelStride];
                v[r * out_w / 2 + x] = ((uint8_t *)pl.planes[2].data)[(size_t)r * pl.planes[2].rowStride + x * pl.planes[2].pixelStride];
            }
        AHardwareBuffer_unlock(b, NULL);
        AHardwareBuffer_release(b);
    } else
#endif
    h264dec_copy_out(pic, yuv, out_w, u, v, out_w / 2, 1);
    frames[nframes++] = (struct frame){ pic->period, pic->poc, yuv };
    cb_ms += now_ms() - t;
    if (pic->mbs_in_error) fprintf(stderr, "picture poc %d: %u macroblocks in error\n", pic->poc, pic->mbs_in_error);
    return 0;
}

static int cmp_frame(const void *a, const void *b) {
    const struct frame *x = a, *y = b;
    return x->period != y->period ? x->period - y->period : x->poc - y->poc;
}

/* start of the access unit after the one at pos: the next AUD, SPS, PPS, or a
 * slice with first_mb_in_slice == 0 (first byte after the header >= 0x80) */
static size_t next_au(const uint8_t *in, size_t n, size_t pos) {
    int seen_slice = 0;
    for (size_t i = pos; i + 4 <= n; i++) {
        if (in[i] || in[i + 1] || in[i + 2] != 1) continue;
        int type = in[i + 3] & 0x1f;
        size_t sc = i > pos && in[i - 1] == 0 ? i - 1 : i;
        if (type == 1 || type == 5) {
            if (seen_slice && i + 4 < n && (in[i + 4] & 0x80)) return sc;
            seen_slice = 1;
        } else if (seen_slice && (type == 9 || type == 7 || type == 8 || type == 6)) {
            return sc;
        }
        i += 2;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.h264 out.yuv [render node [vic]]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t n = ftell(f);
    rewind(f);
    uint8_t *in = malloc(n);
    if (fread(in, 1, n, f) != n) return 1;
    fclose(f);
    use_vic = argc > 4 && !strcmp(argv[4], "vic");
    struct h264dec *dec = g_dec = h264dec_create(argc > 3 ? argv[3] : "/dev/dri/renderD128", 0xf0, on_picture, NULL);
    if (!dec) { fprintf(stderr, "cannot open NVDEC\n"); return 1; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int aus = 0;
    for (size_t pos = 0; pos < n; aus++) {
        size_t end = next_au(in, n, pos);
        int err = h264dec_decode(dec, in + pos, end - pos, aus);
        if (err) { fprintf(stderr, "decode error %d at access unit %d\n", err, aus); return 1; }
        pos = end;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("%d access units, %d pictures %ux%u, reorder depth %d, %.2f ms/picture, of which CLI callback %.2f\n",
           aus, nframes, out_w, out_h, h264dec_reorder_depth(dec), ms / (nframes ? nframes : 1), cb_ms / (nframes ? nframes : 1));
    qsort(frames, nframes, sizeof(*frames), cmp_frame);
    FILE *o = fopen(argv[2], "wb");
    for (int i = 0; i < nframes; i++) fwrite(frames[i].yuv, 1, frame_size, o);
    fclose(o);
    h264dec_destroy(dec);
    return 0;
}
