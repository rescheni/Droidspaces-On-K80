#!/system/bin/sh
# Exercise the packaged KernelSU loader on the device it targets.
#
# The module must already be activated once (ksud insmod ... activate=1); this
# script only runs the loader logic from a staged copy of the ZIP contents, so
# it proves the gate, the payload check, the insmod path and the status text
# without reinstalling the package.
set -u
D=${1:-/data/local/tmp/zornmod}
cd "$D" || exit 1

echo "== staged module tree =="
ls -R "$D" | head -20

echo "== loader: target gate =="
DS_BTF_SHA256=0000000000000000000000000000000000000000000000000000000000000000 \
	sh -c ". $D/common.sh; ds_target_supported; echo \"gate_rc=\$?\"; echo \"gate_reason=\$DS_REASON\""

echo "== loader: payload check =="
MODDIR=$D sh -c ". $D/common.sh; ds_payload_valid; echo \"payload_rc=\$?\""

echo "== loader: service.sh =="
rm -f "$D/load.log"
sh "$D/service.sh"
echo "loader_rc=$?"
echo "--- load.log ---"
cat "$D/load.log" 2>/dev/null
echo "--- status.txt ---"
cat "$D/status.txt" 2>/dev/null
