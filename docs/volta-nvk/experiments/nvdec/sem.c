/* NVDEC semaphore release: does the engine process methods and write to our memory? */
#include "host1x.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    struct t_dev d;
    uint32_t cls = argc > 1 ? strtoul(argv[1], NULL, 0) : HOST1X_CLASS_NVDEC;
    if (t_open(&d, "/dev/dri/renderD128", cls)) return 1;
    struct t_bo bo;
    if (t_bo_new(&d, &bo, 4096)) return 1;
    volatile uint32_t *p = bo.ptr;
    p[0] = 0xdeadbeef;
    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, 0x200, 3);                      /* SET_APPLICATION_ID H264 */
    t_mthd_reloc(&j, 0x240, &bo, 0, 32);       /* SEMAPHORE_A: address bits 39:32 */
    t_mthd_reloc(&j, 0x244, &bo, 0, 0);        /* SEMAPHORE_B: address bits 31:0 */
    t_mthd(&j, 0x248, 0x12345678);             /* SEMAPHORE_C: payload */
    t_mthd(&j, 0x304, 0);                      /* SEMAPHORE_D: release, one word */
    t_syncpt_incr(&j, &d);
    int r = t_submit_wait(&d, &j, 2000, NULL);
    printf("class 0x%x: submit/wait %s, memory word 0x%08x\n", cls, r ? "TIMED OUT" : "ok", p[0]);
    return 0;
}
