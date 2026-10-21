#!/bin/bash
# summarize an xval-pp log: correctness, ROPE perf head->new, per-op logger
L=$1
sed -n '/== device/,/== perf ROPE/p' $L | grep -vE "^== perf"
echo "-- ROPE perf (us): shape  head -> new"
paste <(grep "^${A:-head}: " $L | sed -E 's/.*ne_a=\[([0-9,]+)\],n_dims=([0-9]+),mode=([0-9]+).*ff=([01]),v=([01]).*- +([0-9.]+) us\/run.*/\1 d=\2 m=\3 ff=\4 v=\5 \6/') \
      <(grep "^${B:-new}: " $L | sed -E 's/.*- +([0-9.]+) us\/run.*/\1/') | awk '{printf "%-14s %-6s %-5s %s %s %8s -> %8s  %+.0f%%\n",$1,$2,$3,$4,$5,$6,$7,($7/$6-1)*100}'
sed -n '/== per-op/,/== llama-bench/p' $L | grep -v "^== llama"
sed -n '/== llama-bench/,$p' $L
