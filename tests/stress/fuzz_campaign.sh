#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
# fuzz_campaign.sh OUTFILE ITERATIONS SEED - fuzz_write.py on several VMS-made images in parallel (project root)
out=${1:-fuzz-write}; iters=${2:-2000}; seed=${3:-1}
mkdir -p "$out"
W=${STRESS_WORK:-$HOME/tmp/ods5-st/work}
# at most 6 jobs at a time, each in a 4 GB scope (24 GB total); never uncapped
for im in m1-hl-after-asim12 m1-nohl-after-asim12 m1-ods2-after-asim12 m23-after-asim12 m2b-after-asim12 w1c-f5-after-asim12 w1c-f2-after-asim12 phvs2-ods2v-20261108 asim12-ods5t-20261108 asim12-ods2t-20261108; do
    echo "$im"
done | xargs -P 6 -I{} sh -c "systemd-run --user --scope --quiet -p MemoryMax=4G -p MemorySwapMax=0 python3 -I tests/stress/fuzz_write.py $HOME/tmp/ods5-cli/{}.img $iters $seed $W/fw-{}-$seed $FUZZ_OPTS > $out/{}.txt 2>&1"
echo "done $(date +%T)" > "$out/DONE"
