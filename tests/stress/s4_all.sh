#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# s4_all.sh - the S4 rebuild: make test, the 14 after-images (s4_images.py), their README (s4_readme.py) and a zip bundle,
# then, if UP2_ODS2 and UP2_REV are set, the batch made with the upstream-based build (s4_up2.py) and its bundle.
# Run from anywhere; it works in the project root. Hours of work and many GB of disk.
#
#   S4_HOME   scratch and results (default $HOME/tmp/ods5-stress; results in $S4_HOME/results/s4-REV, images and bundles in
#             $S4_HOME/vms-REV and $S4_HOME/s4-bundle-REV.zip, work in $S4_HOME/work)
#   REV       the revision label of the build under test (default: git rev-parse --short HEAD)
#   UP2_ODS2  ods2 binary of the upstream-based build for the second batch (optional; with UP2_READ, the reader binary,
#             default ./ods2), UP2_REV its revision label, UP2_START the starter volume (default $S4_HOME/work/up2/transfer.dsk)
#   S4_SCOPE  command that limits memory (default: systemd-run --user --scope with MemoryMax=6G, if available; "" for none)
set -e
cd "$(dirname "$0")/../.."
S4_HOME=${S4_HOME:-$HOME/tmp/ods5-stress}
mkdir -p "$S4_HOME/tmp"
export TMPDIR="$S4_HOME/tmp"
REV=${REV:-$(git rev-parse --short HEAD)}
R=$S4_HOME/results/s4-$REV; mkdir -p "$R"
if [ -z "${S4_SCOPE+x}" ]; then
    if command -v systemd-run >/dev/null 2>&1; then SC="systemd-run --user --scope --quiet -p MemoryMax=6G -p MemorySwapMax=0"; else SC=""; fi
else
    SC=$S4_SCOPE
fi

bundle() { # bundle <dir> <zip>
    python3 -I - "$1" "$2" <<'PY'
import zipfile, os, sys, hashlib
dd, p = sys.argv[1], sys.argv[2]
if os.path.exists(p):
    os.chmod(p, 0o644); os.unlink(p)
z = zipfile.ZipFile(p, 'w', zipfile.ZIP_DEFLATED, compresslevel=6)
for f in sorted(os.listdir(dd)):
    z.write(os.path.join(dd, f), f)
z.close(); os.chmod(p, 0o444)
print(p, os.path.getsize(p), hashlib.sha256(open(p, 'rb').read()).hexdigest(), len(os.listdir(dd)), 'files')
PY
}

make ods2 ods2_init >/dev/null
echo "make test start $(date +%T)" >> "$R/progress.txt"
$SC make test > "$R/maketest.txt" 2>&1 || true
echo "make test: $(grep -c '^PASS' "$R/maketest.txt") PASS, $(grep -c '^FAIL' "$R/maketest.txt") FAIL" >> "$R/progress.txt"
D=$S4_HOME/vms-$REV
rm -rf "$S4_HOME/work/s4" "$S4_HOME/work/stress"
echo "images start $(date +%T)" >> "$R/progress.txt"
STRESS_WORK="$S4_HOME/work" $SC python3 -I tests/stress/s4_images.py "$D" > "$R/s4.txt" 2>&1
for n in s4-vms5-heavy:S4VMS5 s4-vms2-heavy:S4VMS2; do
    f=${n%%:*}; l=${n##*:}
    chmod 644 "$D/$f.img"; ./ods2 "$D/$f.img" "SET VOLUME/LABEL=$l" >/dev/null; chmod 444 "$D/$f.img"
done
$SC python3 -I tests/stress/s4_readme.py "$D" "$REV" >> "$R/s4.txt" 2>&1
bundle "$D" "$S4_HOME/s4-bundle-$REV.zip"
echo "main bundle done $(date +%T)" >> "$R/progress.txt"

if [ -n "${UP2_ODS2:-}" ] && [ -n "${UP2_REV:-}" ]; then
    U=$S4_HOME/vms-up2-$UP2_REV
    rm -rf "$S4_HOME/work/s4up2"
    ODS2=$UP2_ODS2 ODS2_READ=${UP2_READ:-$(pwd)/ods2} STRESS_WORK="$S4_HOME/work" $SC python3 -I tests/stress/s4_up2.py "$U" \
        "${UP2_START:-$S4_HOME/work/up2/transfer.dsk}" > "$R/up2.txt" 2>&1
    $SC python3 -I tests/stress/s4_readme.py "$U" "$UP2_REV" "upstream ods2 with our fixes: build $UP2_REV (upstream allenpomeroy/ods2v2 plus the ports of our fixes)" >> "$R/up2.txt" 2>&1
    bundle "$U" "$S4_HOME/s4-bundle-up2-$UP2_REV.zip"
fi
echo "ALL DONE $(date +%T)" >> "$R/progress.txt"
