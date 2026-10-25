#!/bin/bash
# nvk-idp.sh RENDER: NVK built from the rebased nak-idp4a-imm branch, full suites (iq1_m included).
set +e
exec 9</dev/dri/$1
cd ~/xval; M=~/xval/tl
export VK_ICD_FILENAMES=$HOME/nvk-check/icd-idp/nouveau_icd.json
LD_LIBRARY_PATH=out/m0 out/m0/llama-bench -m $M/tl-Q4_K_M.gguf -p 0 -n 1 -r 1 2>&1 | grep -E "^ggml_vulkan: 0 ="
for f in "MUL_MAT type_a=iq1_m," "MUL_MAT_ID type_a=iq1_m," "MUL_MAT ." "MUL_MAT_ID ." "FLASH_ATTN_EXT ."; do
  set -- $f
  r=$(LD_LIBRARY_PATH=out/m0 timeout 3600 out/m0/test-backend-ops test -b Vulkan0 -o $1 -p "$2" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
  echo "$1 [$2]: $(echo "$r" | grep 'tests passed' | tail -1 | xargs) fails=$(echo "$r" | grep -c FAIL) lost=$(echo "$r" | grep -c DeviceLost)"
  echo "$r" | grep FAIL | head -5
done
LD_LIBRARY_PATH=out/m0 out/m0/llama-bench -m $M/tinyllama-1.1b-chat-v1.0.Q4_0.gguf -fa 1 -p 512 -n 0 -r 3 2>&1 | grep -E '^\| llama'
echo NVK-IDP-DONE
