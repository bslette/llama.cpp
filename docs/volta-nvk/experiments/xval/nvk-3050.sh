#!/bin/bash
# nvk-3050.sh: Mesa main 02f13375 vs + 1e32f0c (NVK) on the RTX 3050 under nouveau.
set +e
exec 9</dev/dri/renderD128   # keep the GPU awake
cd ~/xval; M=~/xval/tl
clean() { sed 's/\x1b\[[0-9;]*m//g'; }
for v in main fix; do
  export VK_ICD_FILENAMES=$HOME/nvk-check/icd-1e32-$v/nouveau_icd.json
  echo "== $v"
  LD_LIBRARY_PATH=out/m0 out/m0/llama-bench -m $M/tl-Q4_K_M.gguf -p 0 -n 1 -r 1 2>&1 | grep -E "^ggml_vulkan: 0 ="
  for op in MUL_MAT MUL_MAT_ID FLASH_ATTN_EXT; do
    r=$(LD_LIBRARY_PATH=out/m0 timeout 3600 out/m0/test-backend-ops test -b Vulkan0 -o $op -p '^(?!.*type_a=iq1_m,)' 2>&1 | clean)
    echo "$op: $(echo "$r" | grep -E 'tests passed' | tail -1 | xargs) fails=$(echo "$r" | grep -c FAIL) lost=$(echo "$r" | grep -c DeviceLost)"
    echo "$r" | grep FAIL | head -5
  done
  LD_LIBRARY_PATH=out/m0 out/m0/llama-bench -m $M/tinyllama-1.1b-chat-v1.0.Q4_0.gguf -fa 1 -p 512 -n 0 -r 3 2>&1 | grep -E '^\| llama'
done
for v in icd-1e32-main main-fixed; do
  export VK_ICD_FILENAMES=$HOME/nvk-check/$v/nouveau_icd.json
  r=$(LD_LIBRARY_PATH=out/m0 timeout 600 out/m0/test-backend-ops test -b Vulkan0 -o MUL_MAT -p 'type_a=iq1_m,' 2>&1 | clean)
  echo "iq1_m $v: $(echo "$r" | grep 'tests passed' | xargs) lost=$(echo "$r" | grep -c DeviceLost)"
done
echo NVK-3050-DONE
