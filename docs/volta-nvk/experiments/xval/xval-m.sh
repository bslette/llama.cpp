#!/bin/bash
# xval-m.sh: on one Vulkan device (env DEV, default 0):
#  1. the whole-tile MUL_MAT test cases (tree t1 = master + tests)
#  2. MMVQ row groups (tree g1 = master + ad1c0b8 + GGML_VK_MMVQ_ROW_GROUPS knob):
#     correctness and speed with 1, 2, 4 row groups
set +e
cd ~/xval
M=~/xval/tl
export GGML_VK_VISIBLE_DEVICES=${DEV:-0}
run() { local t=$1; shift; env LD_LIBRARY_PATH=$HOME/xval/out/$t "$@"; }
clean() { sed 's/\x1b\[[0-9;]*m//g'; }

echo "== device"
run m0 out/m0/llama-bench -m $M/tl-Q4_K_M.gguf -p 0 -n 1 -r 1 2>&1 | grep -E "^ggml_vulkan: [0-9] =" | head -1

echo "== 1. whole-tile MUL_MAT cases (t1)"
r=$(run t1 timeout 1800 out/t1/test-backend-ops test -b Vulkan0 -o MUL_MAT -p 'm=(256,n=128,k=256|512,n=256,k=1024|128,n=384,k=2048),' 2>&1 | clean)
echo "$r" | grep -E "tests passed" | tail -1; echo "$r" | grep -E "FAIL" | head -10
echo "$r" | grep -c "OK" | sed 's/^/cases OK: /'

echo "== 2a. MMVQ row groups correctness (g1)"
for rg in 1 4; do
  for o in "MUL_MAT:n=[1-8]," "MUL_MAT_ID:n=[1-8],"; do
    op=${o%%:*}; f=${o#*:}
    r=$(run g1 env GGML_VK_MMVQ_ROW_GROUPS=$rg timeout 1800 out/g1/test-backend-ops test -b Vulkan0 -o $op -p "$f" 2>&1 | clean)
    echo "rg=$rg $op: $(echo "$r" | grep -E 'tests passed' | tail -1 | xargs) fails=$(echo "$r" | grep -c FAIL)"
    echo "$r" | grep FAIL | head -3
  done
done

echo "== 2b. MUL_MAT 4096x14336 us/run (m0 / g1 rg=1,2,4)"
for ty in q4_0 q8_0 q4_K q6_K iq4_xs; do
  for n in 1 2 4; do
    line="$ty n=$n"
    for cfg in "m0:1" "g1:1" "g1:2" "g1:4"; do
      t=${cfg%%:*}; rg=${cfg#*:}
      us=$(run $t env GGML_VK_MMVQ_ROW_GROUPS=$rg timeout 300 out/$t/test-backend-ops perf -b Vulkan0 -o MUL_MAT -p "type_a=$ty,type_b=f32,m=4096,n=$n,k=14336" 2>&1 | grep -o "[0-9.]* us/run" | head -1 | cut -d' ' -f1)
      line="$line  $t/$rg=$us"
    done
    echo "$line"
  done
done

echo "== 2c. llama-bench tg128 t/s (m0 / g1 rg=1,2,4)"
for m in tinyllama-1.1b-chat-v1.0.Q4_0 tinyllama-1.1b-chat-v1.0.Q8_0 tl-Q4_K_M tl-IQ4_XS; do
  line="$m"
  for cfg in "m0:1" "g1:1" "g1:2" "g1:4"; do
    t=${cfg%%:*}; rg=${cfg#*:}
    ts=$(run $t env GGML_VK_MMVQ_ROW_GROUPS=$rg out/$t/llama-bench -m $M/$m.gguf -fa 1 -p 0 -n 128 -r 3 2>&1 | grep -E '^\| llama' | awk -F'|' '{print $(NF-1)}' | xargs)
    line="$line  $t/$rg=$ts"
  done
  echo "$line"
done
echo XVAL-M-DONE
