/*
 * mcdec in.mp4 out.yuv [codec name]: decode the first video track through
 * MediaCodec (ByteBuffer output) and write I420 frames, to test the Codec2
 * components end to end. With MCDEC_REALTIME=1 the input is paced by its
 * timestamps (as a player feeds it) and the decode latency per frame is
 * reported: output time minus the time its input was queued. With
 * MCDEC_SURFACE=1 the output goes to an AImageReader surface (no CPU copy, as a
 * player renders it) and nothing is written: decoder throughput.
 */
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include <android/hardware_buffer.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void on_image(void *ctx, AImageReader *r) {
    (void)ctx;
    AImage *img = NULL;
    while (AImageReader_acquireNextImage(r, &img) == AMEDIA_OK) AImage_delete(img);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

#define NQ 4096
static struct { int64_t pts; double t; } queued_at[NQ]; /* ring of input queue times */

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.mp4 out.yuv [codec]\n", argv[0]); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    /* the codec calls back over binder: start a thread pool (not in the NDK stubs) */
    void *binder = dlopen("libbinder_ndk.so", RTLD_NOW);
    bool (*set_max)(uint32_t) = binder ? dlsym(binder, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
    void (*start_pool)(void) = binder ? dlsym(binder, "ABinderProcess_startThreadPool") : NULL;
    if (!set_max || !start_pool) { fprintf(stderr, "no ABinderProcess in libbinder_ndk\n"); return 1; }
    set_max(4);
    start_pool();
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(argv[1]); return 1; }
    AMediaExtractor *ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) { fprintf(stderr, "extractor failed\n"); return 1; }
    AMediaFormat *fmt = AMediaExtractor_getTrackFormat(ex, 0);
    const char *mime;
    AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime);
    AMediaExtractor_selectTrack(ex, 0);
    AMediaCodec *codec = argc > 3 ? AMediaCodec_createCodecByName(argv[3]) : AMediaCodec_createDecoderByType(mime);
    if (!codec) { fprintf(stderr, "no codec for %s\n", mime); return 1; }
    char *name = NULL;
    AMediaCodec_getName(codec, &name);
    printf("%s: %s\n", mime, name);
    int surface = getenv("MCDEC_SURFACE") != NULL;
    ANativeWindow *window = NULL;
    if (surface) {
        int32_t fw = 0, fh = 0;
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &fw);
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &fh);
        AImageReader *reader = NULL;
        if (AImageReader_newWithUsage(fw, fh, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8,
                                      &reader) != AMEDIA_OK ||
            AImageReader_getWindow(reader, &window) != AMEDIA_OK) {
            fprintf(stderr, "no image reader\n");
            return 1;
        }
        static AImageReader_ImageListener l = {NULL, on_image};
        AImageReader_setImageListener(reader, &l);
    }
    if (AMediaCodec_configure(codec, fmt, window, NULL, 0) != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
        fprintf(stderr, "configure/start failed\n");
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    int in_eos = 0, frames = 0;
    int32_t w = 0, h = 0, stride = 0, slice = 0, color = 0, cl = 0, ct = 0, cr = -1, cb = -1;
    double t0 = now_ms(), last = t0;
    int queued = 0, iters = 0, realtime = getenv("MCDEC_REALTIME") != NULL, lat_n = 0, max_inflight = 0;
    double lat_sum = 0, lat_max = 0, first_out = 0;
    int64_t first_pts = -1;
    for (;; iters++) {
        if (now_ms() - last > 2000) {
            last = now_ms();
            printf("progress: %d queued, in_eos %d, %d frames out, %d iterations\n", queued, in_eos, frames, iters);
        }
        if (!in_eos) {
            int64_t next_pts = AMediaExtractor_getSampleTime(ex);
            if (realtime && next_pts >= 0) {
                if (first_pts < 0) first_pts = next_pts;
                double due = t0 + (next_pts - first_pts) / 1000.0;
                if (now_ms() < due) { usleep(1000); goto output; }
            }
            ssize_t i = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (i >= 0) {
                size_t cap;
                uint8_t *buf = AMediaCodec_getInputBuffer(codec, i, &cap);
                ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(codec, i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    in_eos = 1;
                } else {
                    int64_t pts = AMediaExtractor_getSampleTime(ex);
                    queued_at[queued % NQ].pts = pts;
                    queued_at[queued % NQ].t = now_ms();
                    media_status_t qs = AMediaCodec_queueInputBuffer(codec, i, 0, n, pts, 0);
                    if (qs != AMEDIA_OK) printf("queueInputBuffer: %d\n", qs);
                    queued++;
                    AMediaExtractor_advance(ex);
                }
            }
        }
    output:;
        AMediaCodecBufferInfo info;
        ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &info, realtime ? 0 : 10000);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &h);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride);
            AMediaFormat_getInt32(of, "slice-height", &slice);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
            if (!AMediaFormat_getRect(of, "crop", &cl, &ct, &cr, &cb)) { cl = ct = 0; cr = w - 1; cb = h - 1; }
            printf("format: %s\n", AMediaFormat_toString(of));
            AMediaFormat_delete(of);
        } else if (o >= 0 || o == AMEDIACODEC_INFO_TRY_AGAIN_LATER || o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            if (o < 0) continue;
            size_t size;
            uint8_t *buf = surface ? NULL : AMediaCodec_getOutputBuffer(codec, o, &size);
            if (info.size > 0 && (buf || surface)) {
                double t = now_ms();
                for (int k = queued - 1; k >= 0 && k >= queued - NQ; k--)
                    if (queued_at[k % NQ].pts == info.presentationTimeUs) {
                        double l = t - queued_at[k % NQ].t;
                        lat_sum += l;
                        lat_n++;
                        if (l > lat_max) lat_max = l;
                        break;
                    }
                if (!frames) first_out = t - t0;
                if (queued - frames > max_inflight) max_inflight = queued - frames;
                if (surface) {
                    frames++;
                    AMediaCodec_releaseOutputBuffer(codec, o, true);
                    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
                    continue;
                }
                int vw = cr - cl + 1, vh = cb - ct + 1;
                int s = stride ? stride : w, sh = slice ? slice : h;
                const uint8_t *y = buf + info.offset, *c = y + (size_t)s * sh;
                for (int r = 0; r < vh; r++) fwrite(y + (size_t)(ct + r) * s + cl, 1, vw, out);
                if (color == 21) { /* NV12 */
                    uint8_t *row = malloc(vw + 1);
                    for (int p = 0; p < 2; p++)
                        for (int r = 0; r < (vh + 1) / 2; r++) {
                            const uint8_t *src = c + (size_t)(ct / 2 + r) * s + cl;
                            for (int x = 0; x < (vw + 1) / 2; x++) row[x] = src[2 * x + p];
                            fwrite(row, 1, (vw + 1) / 2, out);
                        }
                    free(row);
                } else { /* I420 */
                    for (int p = 0; p < 2; p++) {
                        const uint8_t *pl = c + (size_t)p * (s / 2) * (sh / 2);
                        for (int r = 0; r < (vh + 1) / 2; r++)
                            fwrite(pl + (size_t)(ct / 2 + r) * ((s + 1) / 2) + cl / 2, 1, (vw + 1) / 2, out);
                    }
                }
                frames++;
            }
            AMediaCodec_releaseOutputBuffer(codec, o, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        } else {
            printf("dequeueOutputBuffer: %zd\n", o);
            break;
        }
    }
    double total = now_ms() - t0;
    printf("%d frames %dx%d (stride %d, slice %d, color %d) in %.0f ms, %.1f fps\n", frames, w, h, stride, slice, color,
           total, frames * 1000.0 / total);
    if (lat_n)
        printf("latency: avg %.1f ms, max %.1f ms, first output after %.1f ms, up to %d frames in flight\n",
               lat_sum / lat_n, lat_max, first_out, max_inflight);
    fclose(out);
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    return 0;
}
