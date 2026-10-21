#!/bin/bash
# Cross-GPU check of the prompt-processing round (rope 8x32 workgroups, rms_norm rows
# variant; the Volta GEMM changes are gated to NVK GV1xx and inactive elsewhere).
# A, B = trees to compare (default head = a8f39f5, new = d4418c8; final run used new4c = 6e4b61f).  env: DEV = Vulkan device index,
# ICD = optional VK_ICD_FILENAMES
set +e
cd ~/xval
M=~/xval/tl
export GGML_VK_VISIBLE_DEVICES=${DEV:-0}
[ -n "$ICD" ] && export VK_ICD_FILENAMES=$ICD
run() { local t=$1; shift; env LD_LIBRARY_PATH=$HOME/xval/out/$t "$@"; }
A=${A:-head}; B=${B:-new}

echo "== device"
run $B out/$B/llama-bench -m $M/tl-Q4_K_M.gguf -p 0 -n 1 -r 1 2>&1 | grep -E "^ggml_vulkan: [0-9] =" | head -2

echo "== correctness ($B)"
for op in ROPE RMS_NORM RMS_NORM_MUL_ADD; do
  r=$(run $B timeout 1800 out/$B/test-backend-ops test -b Vulkan0 -o $op 2>&1)
  echo "$op: $(echo "$r" | grep -E 'tests passed' | tail -1 | sed 's/\x1b\[[0-9;]*m//g' | xargs) fails=$(echo "$r" | grep -c FAIL)"
  echo "$r" | grep FAIL | head -3
done

echo "== perf ROPE (us/run, perf cases)"
for t in $A $B; do
  run $t timeout 600 out/$t/test-backend-ops perf -b Vulkan0 -o ROPE 2>&1 | grep -E "us/run" | sed -E 's/ +/ /g' | awk -v t=$t '{print t": "$0}' | cut -c1-170
done

echo "== per-op pp512 (perf logger, Q4_0)"
for t in $A $B; do
  echo "-- $t"
  run $t env GGML_VK_PERF_LOGGER=1 out/$t/llama-bench -m $M/tinyllama-1.1b-chat-v1.0.Q4_0.gguf -fa 1 -p 512 -n 0 -r 1 2>&1 | grep -E "^(RMS_NORM|ROPE)" | sort | uniq -c | sort -rn | head -6
done

echo "== llama-bench pp512 / tg128"
for m in tinyllama-1.1b-chat-v1.0.Q4_0 tinyllama-1.1b-chat-v1.0.Q8_0 tl-Q4_K_M tl-IQ4_XS; do
  for t in $A $B; do
    r=$(run $t out/$t/llama-bench -m $M/$m.gguf -fa 1 -p 512 -n 128 -r 3 2>&1 | grep -E '^\| llama' | awk -F'|' '{printf "%s ", $(NF-1)}')
    printf '%-32s %-5s %s\n' $m $t "$r"
  done
done
echo XVAL-PP-DONE
