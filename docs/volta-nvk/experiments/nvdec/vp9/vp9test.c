/*
 * vp9test in.ivf out.yuv [node]: decode an IVF VP9 stream with vp9dec and
 * write the shown frames as I420, plus out.yuv.sizes ("w h" per frame) so
 * per-frame checksums can be compared with ffmpeg -f framemd5.
 */
#include "vp9dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct out { FILE *yuv, *sizes; int frames; };

static int on_picture(void *opaque, const struct nvdec_picture *pic) {
    struct out *o = opaque;
    unsigned w = pic->width, h = pic->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *buf = malloc((size_t)w * h + 2 * (size_t)cw * ch);
    nvdec_copy_out(pic, buf, w, buf + (size_t)w * h, buf + (size_t)w * h + (size_t)cw * ch, cw, 1);
    fwrite(buf, 1, (size_t)w * h + 2 * (size_t)cw * ch, o->yuv);
    fprintf(o->sizes, "%u %u%s\n", w, h, pic->mbs_in_error ? " err" : "");
    free(buf);
    o->frames++;
    return 0;
}

static unsigned le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.ivf out.yuv [node]\n", argv[0]); return 1; }
    FILE *in = fopen(argv[1], "rb");
    uint8_t hdr[32];
    if (!in || fread(hdr, 1, 32, in) != 32 || memcmp(hdr, "DKIF", 4)) { fprintf(stderr, "not IVF\n"); return 1; }
    fseek(in, hdr[6] | hdr[7] << 8, SEEK_SET);
    char sizes_name[4096];
    snprintf(sizes_name, sizeof(sizes_name), "%s.sizes", argv[2]);
    struct out o = { fopen(argv[2], "wb"), fopen(sizes_name, "w"), 0 };
    struct vp9dec *d = vp9dec_create(argc > 3 ? argv[3] : "/dev/dri/renderD128", 0xf0, on_picture, &o);
    if (!d || !o.yuv || !o.sizes) { fprintf(stderr, "setup failed\n"); return 1; }
    uint8_t fh[12];
    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    int packets = 0, errors = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (fread(fh, 1, 12, in) == 12) {
        size_t n = le32(fh);
        if (n > cap) buf = realloc(buf, cap = n);
        if (fread(buf, 1, n, in) != n) break;
        int err = vp9dec_decode(d, buf, n, packets);
        if (err) { fprintf(stderr, "packet %d: error %d\n", packets, err); errors++; }
        packets++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("%d packets, %d frames out, %d errors, %.1f ms (%.2f ms/packet)\n", packets, o.frames, errors, ms,
           packets ? ms / packets : 0);
    vp9dec_destroy(d);
    fclose(o.yuv);
    fclose(o.sizes);
    return errors ? 2 : 0;
}
