/*
 * mcenc in.yuv WxH frames out.h264 [codec name]: encode I420 frames through
 * MediaCodec (ByteBuffer input, YUV420 flexible) to an Annex B stream, to test
 * the Codec2 encoders end to end. Env: MCENC_BITRATE (default 4000000),
 * MCENC_FPS (30), MCENC_PROFILE (8 = High, 1 = Baseline), MCENC_CBR=1,
 * MCENC_IDR_AT=<frame> (request a sync frame there), MCENC_PREPEND=1.
 */
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int env(const char *n, int def) {
    const char *s = getenv(n);
    return s ? atoi(s) : def;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s in.yuv WxH frames out.h264 [codec]\n", argv[0]); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    void *binder = dlopen("libbinder_ndk.so", RTLD_NOW);
    bool (*set_max)(uint32_t) = binder ? dlsym(binder, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
    void (*start_pool)(void) = binder ? dlsym(binder, "ABinderProcess_startThreadPool") : NULL;
    if (!set_max || !start_pool) { fprintf(stderr, "no ABinderProcess in libbinder_ndk\n"); return 1; }
    set_max(4);
    start_pool();

    int w, h;
    if (sscanf(argv[2], "%dx%d", &w, &h) != 2) return 1;
    int frames = atoi(argv[3]);
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[4], "wb");
    if (!in || !out) { perror("open"); return 1; }

    AMediaCodec *codec = argc > 5 ? AMediaCodec_createCodecByName(argv[5]) : AMediaCodec_createEncoderByType("video/avc");
    if (!codec) { fprintf(stderr, "no codec\n"); return 1; }
    char *name = NULL;
    AMediaCodec_getName(codec, &name);
    printf("codec %s\n", name ? name : "?");

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7f420888); /* YUV420Flexible */
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, env("MCENC_BITRATE", 4000000));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, env("MCENC_FPS", 30));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, env("MCENC_GOP_S", 2));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PROFILE, env("MCENC_PROFILE", 8));
    AMediaFormat_setInt32(fmt, "bitrate-mode", env("MCENC_CBR", 1) ? 2 : 1);
    if (env("MCENC_PREPEND", 0)) AMediaFormat_setInt32(fmt, "prepend-sps-pps-to-idr-frames", 1);
    media_status_t st = AMediaCodec_configure(codec, fmt, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (st != AMEDIA_OK) { fprintf(stderr, "configure failed %d\n", st); return 1; }
    if (AMediaCodec_start(codec) != AMEDIA_OK) { fprintf(stderr, "start failed\n"); return 1; }

    size_t fsize = (size_t)w * h * 3 / 2;
    unsigned char *yuv = malloc(fsize);
    int fps = env("MCENC_FPS", 30), idr_at = env("MCENC_IDR_AT", -1);
    int sent = 0, got = 0, keyframes = 0;
    bool in_eos = false, out_eos = false;
    size_t total = 0;
    double t0 = now_ms(), lat = 0, queued[4096];
    while (!out_eos) {
        if (!in_eos) {
            ssize_t ix = AMediaCodec_dequeueInputBuffer(codec, 2000);
            if (ix >= 0) {
                size_t cap;
                uint8_t *buf = AMediaCodec_getInputBuffer(codec, ix, &cap);
                if (sent >= frames || fread(yuv, 1, fsize, in) != fsize) {
                    AMediaCodec_queueInputBuffer(codec, ix, 0, 0, (uint64_t)sent * 1000000 / fps,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    in_eos = true;
                } else {
                    /* the input image is planar I420 for YUV420Flexible ByteBuffers */
                    AMediaFormat *ifmt = AMediaCodec_getInputFormat(codec);
                    int32_t stride = w, sliceh = h;
                    AMediaFormat_getInt32(ifmt, "stride", &stride);
                    AMediaFormat_getInt32(ifmt, "slice-height", &sliceh);
                    AMediaFormat_delete(ifmt);
                    size_t need = (size_t)stride * sliceh * 3 / 2;
                    if (cap < need) { fprintf(stderr, "input buffer %zu < %zu\n", cap, need); return 1; }
                    for (int y = 0; y < h; y++) memcpy(buf + (size_t)y * stride, yuv + (size_t)y * w, w);
                    uint8_t *u = buf + (size_t)stride * sliceh, *v = u + (size_t)(stride / 2) * (sliceh / 2);
                    for (int y = 0; y < h / 2; y++) {
                        memcpy(u + (size_t)y * (stride / 2), yuv + (size_t)w * h + (size_t)y * (w / 2), w / 2);
                        memcpy(v + (size_t)y * (stride / 2), yuv + (size_t)w * h * 5 / 4 + (size_t)y * (w / 2), w / 2);
                    }
                    if (sent == idr_at) {
                        AMediaFormat *p = AMediaFormat_new();
                        AMediaFormat_setInt32(p, "request-sync", 0);
                        AMediaCodec_setParameters(codec, p);
                        AMediaFormat_delete(p);
                    }
                    queued[sent % 4096] = now_ms();
                    AMediaCodec_queueInputBuffer(codec, ix, 0, need, (uint64_t)sent * 1000000 / fps, 0);
                    sent++;
                }
            }
        }
        AMediaCodecBufferInfo info;
        ssize_t ox = AMediaCodec_dequeueOutputBuffer(codec, &info, 2000);
        if (ox >= 0) {
            size_t cap;
            uint8_t *buf = AMediaCodec_getOutputBuffer(codec, ox, &cap);
            if (info.size > 0) fwrite(buf + info.offset, 1, info.size, out);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                printf("codec config %d bytes\n", info.size);
            } else if (info.size > 0) {
                int idx = (int)(info.presentationTimeUs * fps / 1000000);
                lat += now_ms() - queued[idx % 4096];
                total += info.size;
                if (info.flags & 1 /* key frame */) {
                    keyframes++;
                    printf("frame %d: key frame, %d bytes\n", idx, info.size);
                }
                got++;
            }
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) out_eos = true;
            AMediaCodec_releaseOutputBuffer(codec, ox, false);
        }
    }
    double dt = now_ms() - t0;
    printf("%d frames in, %d out, %d key frames, %.1f fps, avg latency %.1f ms, %.0f kbit/s at %d fps\n", sent, got,
           keyframes, got * 1000.0 / dt, got ? lat / got : 0, got ? total * 8.0 * fps / got / 1000 : 0, fps);
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    fclose(out);
    return 0;
}
