#!/bin/bash
# sat-dot lowering on the RTX 3050: correctness, vkpeak int8-dotprod, varying-source bench
exec 9</dev/dri/renderD128
for v in icd-idp icd-sat; do
  export VK_ICD_FILENAMES=$HOME/nvk-check/$v/nouveau_icd.json
  echo "== $v"
  (cd ~/satdot && ./satdot satdot.spv | tail -1)
  for r in 1 2; do (cd ~/vkpeak && timeout 300 ./vkpeak 0 int8-dotprod 2>&1 | grep "int8-dotprod ="); done
  for r in 1 2; do (cd ~/satdot && echo "$(./satbench satbench.spv 0 | tail -1) | $(./satbench satbench.spv 1 | tail -1)"); done
done
echo SAT3050-DONE
