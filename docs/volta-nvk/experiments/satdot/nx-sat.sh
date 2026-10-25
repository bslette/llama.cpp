#!/bin/bash
# nx-sat.sh TAG: sat-dot correctness + speed on the Xavier NX (NVK, as root)
. /root/nvkenv.sh
exec 9</dev/dri/renderD129
cd /home/slettela/satdot
echo "== $1 $(git -C /root/mesa-nvk log -1 --format=%h) $(git -C /root/mesa-nvk diff --stat | tail -1)"
./satdot satdot.spv | tail -1
for r in 1 2; do echo "$(./satbench satbench.spv 0 | tail -1) | $(./satbench satbench.spv 1 | tail -1)"; done
for r in 1 2; do (cd /home/slettela/vkpeak-src/b && timeout 600 ./vkpeak 0 int8-dotprod 2>&1 | grep -E "int8-dotprod ="); done
