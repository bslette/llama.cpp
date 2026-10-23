#!/bin/bash
# runall.sh: decode every vector on the board (nx-linux, root via jroot.py with JROOT_PW set) and compare with libvpx official md5s
S=/tmp/claude-1000/-var-home-slettela-lineage/62a70a35-6a3e-4145-94cc-1186be71b087/scratchpad
cd $S/vp9 && scp -q codec2/nvdec/vp9dec.c codec2/nvdec/nvdec_common.c vp9test.c nx-linux:vp9/ &&
  ssh nx-linux 'cd ~/vp9 && gcc -std=gnu11 -O2 -Wall -Wno-unused-function -I/usr/include/libdrm -o vp9test vp9test.c vp9dec.c nvdec_common.c host1x.c' || exit 1
cd $S && : "${JROOT_PW:?export JROOT_PW (root password of nx-linux)}" && timeout 900 python3 -I jroot.py 'cd /home/slettela/vp9 && rm -rf out && mkdir out && for f in ivf/*.ivf; do b=$(basename $f .ivf); timeout 120 ./vp9test $f out/$b.yuv >out/$b.log 2>out/$b.err; done; chmod -R a+rX out' >/dev/null 2>&1
rm -rf $S/vp9/out && rsync -a nx-linux:vp9/out/ $S/vp9/out/
cd $S/vp9 && for f in vectors/ivf/*.ivf; do b=$(basename $f .ivf); r=$(ls vectors/official/$b.*.md5); printf "%-36s %-42s %s\n" $b "$(python3 -I cmp.py out/$b.yuv $r 2>&1 | tail -1)" "$(cut -c1-60 out/$b.log)"; done
