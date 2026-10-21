#!/bin/bash
# brief.sh LOG [A B]: one-screen summary of an xval-pp log
L=$1; A=${2:-head}; B=${3:-new3}
grep -m1 "^ggml_vulkan: 0" $L | sed -E 's/ggml_vulkan: 0 = ([^|]*)\|.*/\1/'
grep -E "tests passed" $L | sed -n '/ROPE:/,$p' | head -3
paste <(grep "^$A: " $L | sed -E 's/.*ne_a=\[([0-9,]+)\].*- +([0-9.]+) us\/run.*/\1 \2/') <(grep "^$B: " $L | sed -E 's/.*- +([0-9.]+) us\/run.*/\1/') |
  awk '{r=$3/$2; s+=log(r); n++; if(r>mx){mx=r;wm=$1} if(mn==""||r<mn){mn=r;bm=$1}} END{printf "ROPE %d cases: geomean %.2fx time, best %.2fx (%s), worst %.2fx (%s)\n",n,exp(s/n),mn,bm,mx,wm}'
awk -v A=$A -v B=$B '/^-- /{t=$2} /RMS_NORM\(2048,512/{if(!(t in v)||$6<v[t])v[t]=$6} /^ +[0-9]+ ROPE: /{if(!(t in r)||$5<r[t])r[t]=$5} END{printf "pp512 per-op (faster of 2 runs): rms_norm %.1f -> %.1f us, rope %.1f -> %.1f us\n",v[A],v[B],r[A],r[B]}' $L
sed -n '/== llama-bench/,/XVAL-PP-DONE/p' $L | grep -E "^(tiny|tl-)" | awk '{printf "  %-34s %-5s pp %8s  tg %7s\n",$1,$2,$3,$6}'
