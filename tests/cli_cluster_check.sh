#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_cluster_check.sh - cluster factors up to 16,380 (LARGE_VOLUMES.md 4). HM2$W_CLUSTER is a word and HM2$W_IBMAPVBN = 4c + 1
# must fit one, so a volume can have a cluster factor up to 16,383; but VMS INITIALIZE/CLUSTER_SIZE takes 1 to 16,380 only (V8.4 and
# V7.3-2: CORPUS 36.2, 39), and ods2_init follows it. Volumes of 1 GiB with cluster factors 3 (the VAX default: not a power of
# two, which catches any "% c" assumption), 255, 256, 257, 4,095 and 16,380: make a directory and a file, relabel, delete,
# ods5check. 16,381 and above are refused; --cluster-over-limit allows 16,381 to 16,383 (V8.4 and V7.3-2 mount and write such a
# volume, CORPUS 36.1, 39: they are made, read, written and checked here) but not 16,384 (4c + 1 would not fit a word), and a
# refused command prints no warning. Called by `make test`; run from the project root.

TMP=samples/cli_cluster_tmp
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

rm -rf "$TMP"
mkdir -p "$TMP"
echo "cluster test" > "$TMP/f.txt"
for spec in 2:3 2:255 2:256 2:257 2:4095 2:16380 5:16380; do
    level=${spec%%:*}; c=${spec##*:}
    img="$TMP/c.img"
    rm -f "$img"
    if ! $INIT "$img" 1G --structure=$level --cluster=$c --label=CLUSTER >/dev/null 2>"$TMP/err"; then
        check "ODS-$level cluster $c: INITIALIZE" "made" "refused: $(head -c 100 "$TMP/err")"
        continue
    fi
    out=$($ODS2 "$img" 'CREATE/DIRECTORY [A]' 2>&1)
    out2=$($ODS2 "$img" "COPY $TMP/f.txt [A]X.TXT" 2>&1)
    check "ODS-$level cluster $c: a directory and a file are made" "1 1" "$(echo "$out" | grep -c CREATED) $(echo "$out2" | grep -c COPIED)"
    check "... TYPE gives the file back" "cluster test" "$($ODS2 "$img" 'TYPE [A]X.TXT' 2>&1 | tail -1)"
    out=$($ODS2 "$img" 'SET VOLUME/LABEL=RENAMED' 2>&1)
    copies=$((3 * c - 1))
    check "... relabelled: all $copies home block copies" "1" "$(echo "$out" | grep -c "RELABELED.*($copies home block")"
    check "... ods5check is clean, the label is RENAMED" "0 error(s), 0 warning(s) RENAMED" \
        "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1) $($ODS2 "$img" 'SHOW VOLUME' 2>&1 | grep -o RENAMED | head -1)"
    check "... DELETE" "1" "$($ODS2 "$img" 'DELETE [A]X.TXT' 2>&1 | grep -c DELETED)"
done
# P1 2: INITIALIZE/CLUSTER_SIZE accepts 1 to 16,380 on V8.4 (CORPUS 36.2) and V7.3-2 (CORPUS 39) and refuses 16,381 and above
# with %INIT-F-BADCLUSTER; ods2_init follows it. A volume with cluster 16,381 to 16,383 (HM2$W_IBMAPVBN = 4c + 1 still fits a word)
# is read and written by V8.4 and V7.3-2 (CORPUS 36.1, 39), so it must keep being read and written: --cluster-over-limit makes one.
for c in 16381 16383 16384 65535; do
    rm -f "$TMP/c.img"
    $INIT "$TMP/c.img" 1G --structure=2 --cluster=$c >/dev/null 2>"$TMP/err"
    check "a cluster factor of $c is refused (Alpha INITIALIZE accepts 16,380 at most; the text says it is Alpha's and that the VAX takes 16,381 and 16,382)" "1 no file 1 1" \
        "$(grep -c 'at most 16380' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file') $(grep -c 'Alpha V8.4 and V7.3-2' "$TMP/err") $(grep -c 'VAX V7.3.*16381 and 16382.*CORPUS 40.2' "$TMP/err")"
done
rm -f "$TMP/c.img"
$INIT "$TMP/c.img" 1G --structure=2 --cluster=16384 --cluster-over-limit >/dev/null 2>"$TMP/err"
check "--cluster-over-limit does not lift the word limit: 16,384 is still refused, with no warning of the flag" "1 no file 0" "$(grep -c 'at most 16383' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file') $(grep -c 'INITIALIZE take 16,380' "$TMP/err")"
for spec in 2:16381 2:16383 5:16383; do
    level=${spec%%:*}; c=${spec##*:}
    img="$TMP/c.img"
    rm -f "$img"
    $INIT "$img" 1G --structure=$level --cluster=$c --cluster-over-limit --label=OVER >/dev/null 2>"$TMP/err"
    check "ODS-$level cluster $c with --cluster-over-limit: made, with a warning that Alpha INITIALIZE refuses it, that the VAX takes 16,381 and 16,382 (and none of a minimum)" "1 0 1" "$(grep -c 'INITIALIZE take 16,380' "$TMP/err") $(grep -c 'refuses less' "$TMP/err") $(grep -c 'VAX V7.3.*16,381 and 16,382.*CORPUS 40.2' "$TMP/err")"
    out2=$($ODS2 "$img" "COPY $TMP/f.txt [000000]X.TXT" 2>&1)
    check "... a file is copied in and read back" "1 cluster test" "$(echo "$out2" | grep -c COPIED) $($ODS2 "$img" 'TYPE [000000]X.TXT' 2>&1 | tail -1)"
    check "... ods5check is clean" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$img" 2>&1 | tail -1)"
done
# P1 2 / review 31 N2: HP's INITIALIZE text (DCL Dictionary, /CLUSTER_SIZE) gives a minimum cluster factor, ceil(blocks / (65535 x 4096)),
# so that the storage bitmap fits in 65,535 blocks; VAX V7.3 INITIALIZE of 2,147,483,632 blocks refused 1-8 and 17 and up and accepted
# 9 to 16 (CORPUS 40.1; ods2_init enforces the lower bound only).
# ods2_init refuses less (cluster 8 there: 65,536 bitmap blocks); --cluster-over-limit makes the volume with a warning that says
# whether VMS mounts it is not measured; the default cluster is raised to the minimum where 16 is too small (above 4,294,901,760
# blocks). Sparse images; the small-cluster volume is read and written by ods2v2 and checked.
big=2147483632
rm -f "$TMP/c.img"
if $INIT "$TMP/c.img" $big --structure=2 --cluster=9 >/dev/null 2>&1; then skipbig=0; else
    echo "SKIP: $big blocks: the file system cannot hold a sparse image that large"
    skipbig=1
fi
rm -f "$TMP/c.img"
if [ $skipbig = 0 ]; then
$INIT "$TMP/c.img" $big --structure=2 --cluster=8 >/dev/null 2>"$TMP/err"
check "$big blocks, cluster 8: refused (the minimum is 9), no image, and the text says it is not measured, cites CORPUS 40 and no session name" "1 no file 1 1 1 0 0" \
    "$(grep -c 'at least 9' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file') $(grep -c 'storage bitmap' "$TMP/err") $(grep -c 'not been measured' "$TMP/err") $(grep -c 'CORPUS 40' "$TMP/err") $(grep -c 'ods5-vms3' "$TMP/err") $(grep -c 'SCB block would exceed' "$TMP/err")"
rm -f "$TMP/c.img"
$INIT "$TMP/c.img" $big --structure=2 --cluster=9 >/dev/null 2>"$TMP/err"
check "$big blocks, cluster 9: made, with no warning about the cluster" "0 file" \
    "$(grep -c 'INITIALIZE refuses' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file')"
rm -f "$TMP/c.img"
$INIT "$TMP/c.img" $big --structure=2 --cluster=9 --cluster-over-limit >/dev/null 2>"$TMP/err"
check "$big blocks, cluster 9 with --cluster-over-limit: made, and no below-minimum warning (the flag is not a reason to warn)" "0 file" \
    "$(grep -c 'INITIALIZE refuses' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file')"
rm -f "$TMP/c.img"
$INIT "$TMP/c.img" $big --structure=2 --cluster=8 --cluster-over-limit --label=MINCL >/dev/null 2>"$TMP/err"
check "$big blocks, cluster 8 with --cluster-over-limit: made, warned that VMS INITIALIZE refuses it and mounting is not measured (CORPUS 40, no session name)" "1 1 file 1 0 0" \
    "$(grep -c 'INITIALIZE refuses' "$TMP/err") $(grep -c 'not been measured' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file') $(grep -c 'INITIALIZE refuses.*CORPUS 40.1' "$TMP/err") $(grep -c 'ods5-vms3' "$TMP/err") $(grep -c 'SCB block would exceed' "$TMP/err")"
out2=$($ODS2 "$TMP/c.img" "COPY $TMP/f.txt [000000]X.TXT" 2>&1)
check "... a file is copied in and read back, SHOW VOLUME gives cluster 8" "1 cluster test 8" \
    "$(echo "$out2" | grep -c COPIED) $($ODS2 "$TMP/c.img" 'TYPE [000000]X.TXT' 2>&1 | tail -1) $($ODS2 "$TMP/c.img" 'SHOW VOLUME' 2>&1 | awk '/Cluster size:/ {print $3}')"
check "... ods5check is clean (a whole-volume check, the storage bitmap of 65,544 blocks included)" "0 error(s), 0 warning(s)" "$(python3 -I tests/ods5check.py "$TMP/c.img" 2>&1 | tail -1)"
rm -f "$TMP/c.img"
fi
# the default cluster: 16 up to 4,294,901,760 blocks (65,535 bitmap blocks), 17 above; an explicit 16 above is refused
for spec in "4294901760 16" "4294901761 17" "4294967280 17"; do
    set -- $spec
    rm -f "$TMP/c.img"
    if ! $INIT "$TMP/c.img" $1 --structure=5 >/dev/null 2>/dev/null; then
        echo "SKIP: $1 blocks: the file system cannot hold a sparse image that large"
        rm -f "$TMP/c.img"
        continue
    fi
    check "$1 blocks: the default cluster is $2" "$2" "$($ODS2 "$TMP/c.img" 'SHOW VOLUME' 2>&1 | awk '/Cluster size:/ {print $3}')"
done
rm -f "$TMP/c.img"
$INIT "$TMP/c.img" 4294901761 --structure=5 --cluster=16 >/dev/null 2>"$TMP/err"
check "4,294,901,761 blocks, cluster 16: refused (the minimum is 17)" "1 no file" "$(grep -c 'at least 17' "$TMP/err") $([ -e "$TMP/c.img" ] && echo file || echo 'no file')"
rm -f "$TMP/c.img"
rm -rf "$TMP"
if [ $fails -ne 0 ]; then
    echo "cli_cluster_check: $fails FAILED"
    exit 1
fi
echo "cli_cluster_check: all checks passed"
