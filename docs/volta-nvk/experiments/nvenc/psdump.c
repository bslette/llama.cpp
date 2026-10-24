#include <stdio.h>
#include <stdlib.h>
#include "nvenc_drv.h"
unsigned char buf[65536];
nvenc_h264_drv_pic_setup_s *top = (void *)buf;
nvenc_h264_slice_control_s *sl;
nvenc_h264_me_control_s *me;
nvenc_h264_md_control_s *md;
nvenc_h264_quant_control_s *q;
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fread(buf, 1, sizeof(buf), f);
    sl = (void *)(buf + top->pic_control.slice_control_offset);
    me = (void *)(buf + top->pic_control.me_control_offset);
    md = (void *)(buf + top->pic_control.md_control_offset);
    q = (void *)(buf + top->pic_control.q_control_offset);
    return 0;
}
