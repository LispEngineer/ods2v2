#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
# uninit_diff.sh OUTDIR LO-HI [PROFILE] - S2 substitute for valgrind/MemorySanitizer (neither is installed on this host):
# the same seeds run on plain -O1 builds that differ only in how uninitialised memory looks:
#   A: -ftrivial-auto-var-init=pattern, MALLOC_PERTURB_=165     B: -ftrivial-auto-var-init=zero, MALLOC_PERTURB_=90
# Every seed must pass the whole harness on both, and the final volume (names, sizes, md5, free space) must be identical.
# A difference means the output depends on uninitialised stack or heap memory.   Run from the project root, in a scope.
out=$(realpath -m "${1:-uninit}"); range=${2:-1-40}; prof=${3:-mix}
mkdir -p "$out"
SRC="tools/ods2.c src/ods5_names.c src/ods5_header.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c src/ods2_time.c src/ods2_text_var.c src/ods2_records.c src/ods2_acl.c src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c"
gcc -O1 -g -Wall -Wextra -std=c11 -Iinclude -D_DEFAULT_SOURCE -Ithird_party/linenoise -ftrivial-auto-var-init=pattern $SRC -o "$out/ods2_A" || exit 1
gcc -O1 -g -Wall -Wextra -std=c11 -Iinclude -D_DEFAULT_SOURCE -Ithird_party/linenoise -ftrivial-auto-var-init=zero $SRC -o "$out/ods2_B" || exit 1
export STRESS_DIGEST=1
ODS2=$out/ods2_A MALLOC_PERTURB_=165 python3 -I tests/stress/stress.py --seeds "$range" --profile "$prof" --jobs 3 > "$out/A.txt" 2>&1
ODS2=$out/ods2_B MALLOC_PERTURB_=90 python3 -I tests/stress/stress.py --seeds "$range" --profile "$prof" --jobs 3 > "$out/B.txt" 2>&1
grep -o 'seed [0-9]* .*: PASS.*digest=.*' "$out/A.txt" | sed 's/ (.*digest=/ digest=/' | sort > "$out/A.d"
grep -o 'seed [0-9]* .*: PASS.*digest=.*' "$out/B.txt" | sed 's/ (.*digest=/ digest=/' | sort > "$out/B.d"
echo "A: $(grep -c PASS "$out/A.txt") pass, $(grep -c FAIL "$out/A.txt") fail; B: $(grep -c PASS "$out/B.txt") pass, $(grep -c FAIL "$out/B.txt") fail"
diff "$out/A.d" "$out/B.d" > "$out/diff.txt" && echo "digests identical on $(wc -l < "$out/A.d") seeds" || { echo "DIGESTS DIFFER:"; cat "$out/diff.txt"; }
