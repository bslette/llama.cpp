/*
 * LD_PRELOAD shim for L4T r35: log every nvhost channel submit (host1x methods
 * written through THI, with relocations resolved to nvmap handles) and dump the
 * buffers that selected methods point at. Output in $NVCAP_DIR (default /tmp/nvcap).
 * Build: gcc -O2 -shared -fPIC -o nvcap.so nvcap.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define __packed __attribute__((packed))
struct nvhost_cmdbuf { __u32 mem, offset, words; } __packed;
struct nvhost_reloc { __u32 cmdbuf_mem, cmdbuf_offset, target, target_offset; };
struct nvhost_reloc_shift { __u32 shift; } __packed;
struct nvhost_submit_args {
    __u32 submit_version, num_syncpt_incrs, num_cmdbufs, num_relocs, num_waitchks, timeout, flags, fence;
    __u64 syncpt_incrs, cmdbuf_exts;
    __u32 checksum_methods, checksum_falcon_methods;
    __u64 pad[1];
    __u64 reloc_types, cmdbufs, relocs, reloc_shifts, waitchks, waitbases, class_ids, fences;
};
#define NVHOST_IOCTL_CHANNEL_SUBMIT _IOWR('H', 26, struct nvhost_submit_args)

struct nvmap_rw_handle {
    __u64 addr;
    __u32 handle;
    __u64 offset, elem_size, hmem_stride, user_stride, count;
};
#define NVMAP_IOC_READ _IOW('N', 7, struct nvmap_rw_handle)

static int (*real_ioctl)(int, unsigned long, ...);
static int nvmap_fd = -1;
static unsigned nsub;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* handles are dma-buf fds on r35; fall back to NVMAP_IOC_READ */
static int hread(uint32_t handle, uint64_t off, void *dst, size_t n) {
    off_t size = lseek((int)handle, 0, SEEK_END);
    if (size > 0 && (uint64_t)size > off) {
        if (off + n > (uint64_t)size) { memset(dst, 0, n); n = size - off; }
        void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, (int)handle, 0);
        if (p != MAP_FAILED) {
            memcpy(dst, (uint8_t *)p + off, n);
            munmap(p, size);
            return 0;
        }
    }
    struct nvmap_rw_handle rw = {(uintptr_t)dst, handle, off, n, n, n, 1};
    return nvmap_fd < 0 ? -1 : real_ioctl(nvmap_fd, NVMAP_IOC_READ, &rw);
}

/* methods whose target buffer gets dumped, and how many bytes */
static size_t dump_size(uint32_t method) {
    switch (method) {
    case 0x710: return 16384; /* drv pic setup */
    case 0x70c: return 4096;  /* rc data */
    case 0x724: return 4096;  /* rc process */
    case 0x50c: return 4096;  /* ucode state */
    case 0x200: return 0;
    default: return 0;
    }
}

static void log_submit(int fd, struct nvhost_submit_args *a) {
    const char *dir = getenv("NVCAP_DIR") ? getenv("NVCAP_DIR") : "/tmp/nvcap";
    mkdir(dir, 0777);
    char path[256];
    unsigned n = nsub++;
    if (n > (unsigned)(getenv("NVCAP_MAX") ? atoi(getenv("NVCAP_MAX")) : 400)) return;
    snprintf(path, sizeof(path), "%s/sub%04u.txt", dir, n);
    FILE *f = fopen(path, "w");
    if (!f) return;
    struct nvhost_cmdbuf *cb = (void *)(uintptr_t)a->cmdbufs;
    struct nvhost_reloc *rl = (void *)(uintptr_t)a->relocs;
    struct nvhost_reloc_shift *rs = (void *)(uintptr_t)a->reloc_shifts;
    uint32_t *cls = (void *)(uintptr_t)a->class_ids;
    fprintf(f, "fd %d version %u cmdbufs %u relocs %u incrs %u flags 0x%x\n", fd, a->submit_version, a->num_cmdbufs,
            a->num_relocs, a->num_syncpt_incrs, a->flags);
    for (unsigned i = 0; i < a->num_relocs; i++)
        fprintf(f, "reloc %u: cmdbuf %u +0x%x -> handle %u +0x%x shift %u\n", i, rl[i].cmdbuf_mem, rl[i].cmdbuf_offset,
                rl[i].target, rl[i].target_offset, rs ? rs[i].shift : 0);
    for (unsigned c = 0; c < a->num_cmdbufs; c++) {
        uint32_t words[8192];
        unsigned nw = cb[c].words > 8192 ? 8192 : cb[c].words;
        fprintf(f, "cmdbuf %u: mem %u off 0x%x words %u class 0x%x\n", c, cb[c].mem, cb[c].offset, cb[c].words,
                cls ? cls[c] : 0);
        if (hread(cb[c].mem, cb[c].offset, words, nw * 4)) { fprintf(f, "  read failed %d\n", errno); continue; }
        uint32_t method = 0;
        for (unsigned i = 0; i < nw; i++) {
            uint32_t w = words[i], op = w >> 28;
            if (op == 0) { fprintf(f, "  SETCLASS 0x%x\n", (w >> 6) & 0x3ff); continue; }
            if (op != 1 && op != 2) { fprintf(f, "  op %u word 0x%08x\n", op, w); continue; }
            unsigned reg = (w >> 16) & 0xfff, cnt = w & 0xffff;
            for (unsigned k = 0; k < cnt && i + 1 < nw; k++) {
                i++;
                unsigned r = op == 1 ? reg + k : reg;
                uint32_t v = words[i];
                uint32_t byteoff = cb[c].offset + i * 4;
                int ri = -1;
                for (unsigned x = 0; x < a->num_relocs; x++)
                    if (rl[x].cmdbuf_mem == cb[c].mem && rl[x].cmdbuf_offset == byteoff) ri = x;
                if (r == 0x10) { method = v << 2; continue; }
                if (r == 0x11) {
                    if (ri >= 0) {
                        fprintf(f, "  M 0x%03x = handle %u +0x%x (shift %u) size 0x%lx\n", method, rl[ri].target,
                                rl[ri].target_offset, rs ? rs[ri].shift : 0, (long)lseek((int)rl[ri].target, 0, SEEK_END));
                        size_t ds = dump_size(method);
                        if (ds) {
                            uint8_t *buf = malloc(ds);
                            if (!hread(rl[ri].target, rl[ri].target_offset, buf, ds)) {
                                char bp[300];
                                snprintf(bp, sizeof(bp), "%s/sub%04u_m%03x.bin", dir, n, method);
                                FILE *bf = fopen(bp, "wb");
                                if (bf) { fwrite(buf, 1, ds, bf); fclose(bf); }
                            } else {
                                fprintf(f, "  (dump read failed %d)\n", errno);
                            }
                            free(buf);
                        }
                    } else {
                        fprintf(f, "  M 0x%03x = 0x%08x\n", method, v);
                    }
                    continue;
                }
                fprintf(f, "  reg 0x%03x = 0x%08x%s\n", r, v, ri >= 0 ? " (reloc)" : "");
            }
        }
    }
    fclose(f);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (_IOC_TYPE(req) == 'N' && nvmap_fd < 0) nvmap_fd = fd;
    if (req == NVHOST_IOCTL_CHANNEL_SUBMIT) {
        pthread_mutex_lock(&lock);
        log_submit(fd, arg);
        pthread_mutex_unlock(&lock);
    }
    return real_ioctl(fd, req, arg);
}
