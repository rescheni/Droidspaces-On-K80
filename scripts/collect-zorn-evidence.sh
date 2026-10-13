#!/bin/bash
# Collect the device evidence that scripts/package-zorn.py verifies.
#
#   scripts/collect-zorn-evidence.sh OUTDIR [ADB]
#
# Requirements on the device: the module has been activated once and the probe
# has been copied to /data/local/tmp/nsprobe. The staged module tree used by
# the loader check is read from DS_STAGE (default /data/local/tmp/zornmod); see
# docs/zorn-6.1.md for how it is produced.
set -eu

OUT=${1:?usage: collect-zorn-evidence.sh OUTDIR [ADB]}
ADB=${2:-adb}
DS_RUN=${DS_DEVICE:-/data/local/Droidspaces/bin/droidspaces}
STAGE=${DS_STAGE:-/data/local/tmp/zornmod}
TMP=/data/local/tmp
HERE=$(cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "$OUT"

echo '== device facts =='
"$ADB" shell "su -c 'uname -r; uname -m; getconf PAGE_SIZE; getenforce; ls /proc/self/ns'" \
	> "$OUT/device-facts.txt"
cat "$OUT/device-facts.txt"

echo '== kernel BTF =='
"$ADB" shell "su -c 'cat /sys/kernel/btf/vmlinux'" > "$OUT/device.btf"
sha256sum "$OUT/device.btf"

echo '== droidspaces requirement check =='
"$ADB" shell "su -c '$DS_RUN check'" > "$OUT/device-check.txt" 2>&1
echo "captured $(wc -l < "$OUT/device-check.txt") lines"

echo '== namespace probe =='
"$ADB" shell "su -c '$TMP/nsprobe'" > "$OUT/device-probe.txt" 2>&1
cat "$OUT/device-probe.txt"

echo '== packaged loader =='
"$ADB" push "$HERE/tests/device/zorn-loader-check.sh" "$TMP/" > /dev/null
"$ADB" shell "su -c 'sh $TMP/zorn-loader-check.sh $STAGE'" > "$OUT/device-loader.txt" 2>&1
tail -4 "$OUT/device-loader.txt"

echo
echo "evidence written to $OUT"
