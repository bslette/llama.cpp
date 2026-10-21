#!/bin/bash
# perf only, 3 passes, base vs head
set +e
cd ~/xval
export GGML_VK_VISIBLE_DEVICES=${DEV:-0}
run() { local t=$1; shift; env LD_LIBRARY_PATH=$HOME/xval/out/$t "$@"; }
for pass in 1 2 3; do
for ty in q4_K q5_K iq4_xs q2_K q3_K iq1_m; do
  for n in 1 4; do
    line="p$pass $ty n=$n"
    for t in base head; do
      us=$(run $t timeout 300 out/$t/test-backend-ops perf -b Vulkan0 -o MUL_MAT -p "type_a=$ty,type_b=f32,m=4096,n=$n,k=14336" 2>&1 | grep -o "[0-9.]* us/run" | head -1 | cut -d' ' -f1)
      line="$line  $t=$us"
    done
    echo "$line"
  done
done
done
echo XPERF-DONE
