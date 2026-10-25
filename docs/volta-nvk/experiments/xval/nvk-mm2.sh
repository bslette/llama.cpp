#!/bin/bash
set +e
exec 9</dev/dri/renderD129
cd ~/xval
for v in main fix; do
  export VK_ICD_FILENAMES=$HOME/nvk-check/icd-1e32-$v/nouveau_icd.json
  for op in MUL_MAT MUL_MAT_ID; do
    r=$(LD_LIBRARY_PATH=out/m0 timeout 3600 out/m0/test-backend-ops test -b Vulkan0 -o $op -p '^(?!.*type_a=iq1_m,)' 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    echo "$v $op: $(echo "$r" | grep 'tests passed' | xargs) fails=$(echo "$r" | grep -c FAIL) lost=$(echo "$r" | grep -c DeviceLost)"
    echo "$r" | grep FAIL | head -5
  done
done
echo NVK-MM2-DONE
