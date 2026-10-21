#!/bin/bash
# Cross-GPU validation of the volta-nvk MMVQ changes.
# usage: xval-run.sh   (env: DEV = Vulkan device index, default 0)
# base = 7a5733b (before), head = a8f39f5, exp = head + GGML_VK_MMVQ_BDA / GGML_VK_MMVQ_Q6 knobs
set +e
cd ~/xval
M=~/xval/tl
export GGML_VK_VISIBLE_DEVICES=${DEV:-0}
run() { local t=$1; shift; env LD_LIBRARY_PATH=$HOME/xval/out/$t "$@"; }
EXP="GGML_VK_MMVQ_BDA=1 GGML_VK_MMVQ_Q6=1"

echo "== device"
run head out/head/llama-bench -m $M/tl-Q4_K_M.gguf -p 0 -n 1 -r 1 2>&1 | grep -E "^ggml_vulkan: [0-9] =" | head -2

echo "== correctness"
for cfg in "head:X=1" "exp:$EXP" "exp:$EXP GGML_VK_FORCE_MMVQ=1"; do
  t=${cfg%%:*}; e=${cfg#*:}
  for o in "MUL_MAT:n=[1-8]," "MUL_MAT_ID:n=[1-8]," "MUL_MAT_VEC_FUSION:"; do
    op=${o%%:*}; f=${o#*:}
    r=$(run $t env $e timeout 1800 out/$t/test-backend-ops test -b Vulkan0 -o $op ${f:+-p "$f"} 2>&1)
    echo "$t [$e] $op: $(echo "$r" | grep -E 'tests passed' | tail -1 | sed 's/\x1b\[[0-9;]*m//g' | xargs) fails=$(echo "$r" | grep -c FAIL)"
    echo "$r" | grep FAIL | head -3
  done
done

echo "== perf MUL_MAT 4096x14336 (us/run)"
for ty in q4_K q5_K iq4_xs q4_0 q8_0 q2_K q3_K q6_K iq1_m; do
  for n in 1 4; do
    line="$ty n=$n"
    for cfg in "base:X=1" "head:X=1" "exp:$EXP"; do
      t=${cfg%%:*}; e=${cfg#*:}
      us=$(run $t env $e timeout 300 out/$t/test-backend-ops perf -b Vulkan0 -o MUL_MAT -p "type_a=$ty,type_b=f32,m=4096,n=$n,k=14336" 2>&1 | grep -o "[0-9.]* us/run" | head -1 | cut -d' ' -f1)
      line="$line  $t=$us"
    done
    echo "$line"
  done
done

echo "== llama-bench pp512 / tg128"
for m in tinyllama-1.1b-chat-v1.0.Q4_0 tinyllama-1.1b-chat-v1.0.Q8_0 tl-Q4_1 tl-Q4_K_M tl-Q5_K_M tl-IQ4_XS; do
  for cfg in "base:X=1" "head:X=1" "exp:$EXP"; do
    t=${cfg%%:*}; e=${cfg#*:}
    r=$(run $t env $e out/$t/llama-bench -m $M/$m.gguf -fa 1 -p 512 -n 128 -r 3 2>&1 | grep -E '^\| llama' | awk -F'|' '{printf "%s ", $(NF-1)}')
    printf '%-32s %-5s %s\n' $m $t "$r"
  done
done
echo XVAL-RUN-DONE
