/*
 * h264enc library test: input from a dma-buf through VIC, like the Codec2 component.
 * enctest nv12|rgba in WxH frames out.h264
 *   nv12: in is I420 (written into a pitch-linear NV12 dma-buf)
 *   rgba: in is packed RGBA
 * Env: BITRATE, HIGH, GOP, MATRIX (1 = BT.709), FULL, CPU=1 (no dma-buf: CPU path)
 */
#include "h264enc.h"
#include "host1x.h"

#include <fcntl.h>
#include <drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

static int env(const char *n, int def) {
    const char *s = getenv(n);
    return s ? (int)strtol(s, NULL, 0) : def;
}

int main(int argc, char **argv) {
    if (argc < 6) return 2;
    int rgba = !strcmp(argv[1], "rgba");
    unsigned w, h;
    sscanf(argv[3], "%ux%u", &w, &h);
    int frames = atoi(argv[4]);
    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[5], "wb");
    if (!in || !out) return 1;

    struct h264enc_config cfg = {0};
    cfg.width = w;
    cfg.height = h;
    cfg.profile_idc = env("PROFILE", 100);
    cfg.level_idc = 41;
    cfg.fps = 30;
    cfg.bitrate = env("BITRATE", 0);
    cfg.qp = env("QP", 28);
    cfg.gop = env("GOP", 60);
    cfg.full_range = env("FULL", 0);
    cfg.matrix = env("MATRIX", 6);
    cfg.primaries = cfg.matrix == 1 ? 1 : 6;
    cfg.transfer = cfg.matrix == 1 ? 1 : 6;
    struct h264enc *e = h264enc_create("/dev/dri/renderD128", 0x21, &cfg);
    if (!e) { fprintf(stderr, "create failed\n"); return 1; }
    uint8_t hdr[256];
    int hn = h264enc_headers(e, hdr, sizeof(hdr));
    fwrite(hdr, 1, hn, out);

    /* input dma-buf, allocated on a second tegra-drm handle */
    struct t_dev d;
    if (t_open(&d, "/dev/dri/renderD128", 0x5d)) return 1;
    unsigned pitch = rgba ? (w * 4 + 255) / 256 * 256 : (w + 255) / 256 * 256;
    size_t chroma = (size_t)pitch * h, size = rgba ? chroma : chroma + pitch * h / 2;
    struct t_bo bo;
    if (t_bo_new(&d, &bo, size)) return 1;
    struct drm_prime_handle ph = {.handle = bo.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    if (ioctl(d.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)) { perror("prime"); return 1; }

    size_t fsize = rgba ? (size_t)w * h * 4 : (size_t)w * h * 3 / 2;
    uint8_t *buf = malloc(fsize), *outbuf = malloc(h264enc_max_output(e));
    double ms = 0;
    size_t total = 0;
    int f;
    for (f = 0; f < frames && fread(buf, 1, fsize, in) == fsize; f++) {
        uint8_t *p = bo.ptr;
        struct h264enc_frame fr = {0};
        fr.fd = env("CPU", 0) ? -1 : ph.fd;
        fr.size = size;
        fr.pitch = pitch;
        if (rgba) {
            for (unsigned y = 0; y < h; y++) memcpy(p + (size_t)y * pitch, buf + (size_t)y * w * 4, w * 4);
            fr.format = H264ENC_RGBA;
            fr.y = p;
            fr.y_stride = pitch;
        } else {
            for (unsigned y = 0; y < h; y++) memcpy(p + (size_t)y * pitch, buf + (size_t)y * w, w);
            const uint8_t *u = buf + w * h, *v = u + w * h / 4;
            for (unsigned y = 0; y < h / 2; y++)
                for (unsigned x = 0; x < w / 2; x++) {
                    p[chroma + y * pitch + 2 * x] = u[y * w / 2 + x];
                    p[chroma + y * pitch + 2 * x + 1] = v[y * w / 2 + x];
                }
            fr.format = H264ENC_NV12;
            fr.chroma_offset = chroma;
            fr.y = p;
            fr.u = p + chroma;
            fr.v = p + chroma + 1;
            fr.y_stride = fr.uv_stride = pitch;
            fr.uv_step = 2;
        }
        int idr = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int n = h264enc_encode(e, &fr, f == env("FORCE_IDR_AT", -1), outbuf, h264enc_max_output(e), &idr);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (n < 0) { fprintf(stderr, "frame %d: error %d\n", f, n); return 1; }
        ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (idr && f) fwrite(hdr, 1, hn, out);
        fwrite(outbuf, 1, n, out);
        total += n;
        if (f < 2 || idr) fprintf(stderr, "frame %d: %d bytes%s\n", f, n, idr ? " IDR" : "");
    }
    fprintf(stderr, "%d frames, %.2f ms/frame (convert + encode), %.0f kbit/s\n", f, ms / f, total * 8.0 * 30 / f / 1000);
    h264enc_destroy(e);
    return 0;
}
