/* Open an NVDEC channel and run a job that only increments a syncpoint. */
#include "host1x.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    struct t_dev d;
    uint32_t cls = argc > 1 ? strtoul(argv[1], NULL, 0) : HOST1X_CLASS_NVDEC;
    if (t_open(&d, "/dev/dri/renderD128", cls)) return 1;
    printf("class 0x%x: context %u, version 0x%x, syncpt %u\n", cls, d.context, d.version, d.syncpt);
    struct t_job j;
    t_job_init(&j);
    t_syncpt_incr(&j, &d);
    uint32_t fence;
    if (t_submit_wait(&d, &j, 2000, &fence)) return 1;
    printf("nop job done, syncpt %u reached %u\n", d.syncpt, fence);
    return 0;
}
