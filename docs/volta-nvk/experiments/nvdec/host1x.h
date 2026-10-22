/* Minimal Tegra DRM channel (host1x) helpers for engines driven through the THI method interface. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define HOST1X_CLASS_NVDEC  0xf0
#define HOST1X_CLASS_NVDEC1 0xf5

#define HOST1X_OPCODE_INCR(offset, count)    ((0x1u << 28) | (((offset) & 0xfff) << 16) | ((count) & 0xffff))
#define HOST1X_OPCODE_NONINCR(offset, count) ((0x2u << 28) | (((offset) & 0xfff) << 16) | ((count) & 0xffff))
#define THI_METHOD0 0x10            /* method offset >> 2, then data to METHOD1 */
#define SYNC_COND_OP_DONE 1

struct t_bo {
    uint32_t handle, mapping;
    size_t size;
    void *ptr;
};

struct t_dev {
    int fd;
    uint32_t context, version, syncpt;
};

struct t_job {
    uint32_t words[4096];
    unsigned n;
    struct { uint32_t mapping, gather_off; uint64_t target_off; uint32_t shift; } relocs[64];
    unsigned nrelocs;
    unsigned incrs;
};

int t_open(struct t_dev *d, const char *node, uint32_t host1x_class);
int t_bo_new(struct t_dev *d, struct t_bo *bo, size_t size);
void t_job_init(struct t_job *j);
void t_mthd(struct t_job *j, uint32_t method, uint32_t value);
void t_mthd_addr(struct t_job *j, uint32_t method, struct t_bo *bo, uint64_t offset); /* value = iova >> 8 */
void t_mthd_reloc(struct t_job *j, uint32_t method, struct t_bo *bo, uint64_t offset, unsigned shift); /* value = iova >> shift */
void t_syncpt_incr(struct t_job *j, struct t_dev *d);
int t_submit_wait(struct t_dev *d, struct t_job *j, int64_t timeout_ms, uint32_t *fence);
