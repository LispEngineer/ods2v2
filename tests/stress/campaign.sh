#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
# campaign.sh OUTFILE [JOBS] [LO-HI] - run every stress profile over a seed range (run from the project root after `make ods2 ods2_init`)
out=${1:-stress-campaign.txt}; jobs=${2:-6}; range=${3:-1-30}
export TMPDIR=${TMPDIR:-$HOME/tmp/ods5-st/tmp}
echo "start $(date '+%F %T') load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$out"
for pr in mix names links bigfile fill frag shatter bigdir deep indexf; do
    echo "== $pr seeds $range" >> "$out"
    ops=60; [ $pr = bigfile ] && ops=25
    python3 -I tests/stress/stress.py --seeds "$range" --profile $pr --jobs "$jobs" --ops $ops --keep 2>&1 | grep -v ': PASS' | cut -c1-900 >> "$out"
done
echo "end $(date '+%F %T') load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$out"
