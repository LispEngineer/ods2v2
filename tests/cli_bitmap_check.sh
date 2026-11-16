#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_bitmap_check.sh - the storage bitmap of a write mount is read once and kept (LARGE_VOLUMES.md 3). Every allocation used
# to read all of it: on a 2 TiB volume with 16-block clusters (a 32 MiB bitmap) each file created read 32 MiB, and 300 files
# took 18 seconds (0.2 s now). The bound below is generous (10 s) so a slow host does not fail it, while the old code stays far
# above it on any host this runs on. Called by `make test`; run from the project root.

TMP=samples/cli_bitmap_tmp
INIT=./ods2_init
ODS2=./ods2
fails=0

check() { # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        echo "      expected: $2"
        echo "      got:      $3"
        fails=$((fails + 1))
    fi
}
free_blocks() { $ODS2 "$1" 'SHOW VOLUME' 2>&1 | awk '/Free blocks:/ {print $3}'; }

rm -rf "$TMP"
mkdir -p "$TMP/tree"
python3 -I - "$TMP/tree" <<'PY'
import sys
for i in range(300):
    open('%s/F%d.TXT' % (sys.argv[1], i), 'w').write('file %d\n' % i * 20)
PY
img="$TMP/h.img"
if ! $INIT "$img" 4294967280 --structure=2 --label=HUGE >/dev/null 2>&1; then
    echo "SKIP: the file system cannot hold a sparse 2 TiB image"
    rm -rf "$TMP"
    exit 0
fi
before=$(free_blocks "$img")
t0=$(date +%s)
out=$($ODS2 "$img" "IMPORT $TMP/tree [000000]" 2>&1)
t1=$(date +%s)
check "IMPORT of 300 files into a 2 TiB volume (32 MiB bitmap)" "1" "$(echo "$out" | grep -c 'IMPORTED, 300 files')"
elapsed=$((t1 - t0))
check "... in under 10 seconds (took $elapsed)" "yes" "$([ $elapsed -lt 10 ] && echo yes || echo no)"
rm -rf "$TMP/out"
$ODS2 "$img" "EXPORT [000000]*.* $TMP/out" >/dev/null 2>&1
same=$(for n in 0 1 150 299; do cmp "$TMP/out/F$n.TXT" "$TMP/tree/F$n.TXT" >/dev/null 2>&1 && echo ok; done | wc -l)
check "the files read back identical (4 sampled)" "4" "$same"
mid=$(free_blocks "$img")
used=$((before - mid))
check "the blocks taken are whole clusters of 16 (clusters for files and the directory: $used blocks)" "0" "$((used % 16))"
$ODS2 "$img" 'DELETE [000000]F*.TXT;*' >/dev/null 2>&1
after=$(free_blocks "$img")
check "deleting the 300 one-cluster files frees exactly 300 x 16 blocks (the rest of what was taken is INDEXF.SYS and the directory)" "4800" "$((after - mid))"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_bitmap_check: $fails FAILED"
    exit 1
fi
echo "cli_bitmap_check: all checks passed"
