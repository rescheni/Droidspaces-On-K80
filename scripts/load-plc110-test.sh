#!/system/bin/sh
# Manual PLC110 test loader. This script never installs a boot service.
set -u
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
mode=${1:-activate}
case "$mode" in activate|selftest) ;; *) echo 'Usage: sh load-plc110-test.sh [activate|selftest]' >&2; exit 2;; esac
[ "$(id -u)" = 0 ] || { echo 'Run from a root shell.' >&2; exit 1; }
log="$base/load.log"
: > "$log" || exit 1
say() { printf '%s\n' "$*" | tee -a "$log"; }
collect() { dmesg | tail -250 >> "$log" 2>&1; }
fail() { say "FAILED: $*"; collect; say "Log: $log"; exit 1; }
release='6.6.118-android15-8-ge58033dc8ea6-abogki498046332-4k'
model=$(getprop ro.product.model)
vendor_model=$(getprop ro.product.vendor.model)
actual=$(uname -r)
say "model=$model vendor_model=$vendor_model kernel=$actual mode=$mode"
[ "$model" = PLC110 ] || [ "$vendor_model" = PLC110 ] || fail 'Expected PLC110 model.'
[ "$actual" = "$release" ] || fail 'Kernel release does not match the tested boot image.'
[ "$(uname -m)" = aarch64 ] || fail 'Expected ARM64.'
[ ! -d /sys/module/droidspaces_ns ] || fail 'Module already loaded; reboot before replacing an active module.'
[ -r /sys/kernel/btf/vmlinux ] || fail 'Cannot read kernel BTF for compatibility check.'
actual_btf=$(sha256sum /sys/kernel/btf/vmlinux) || fail 'Cannot hash kernel BTF.'
[ "${actual_btf%% *}" = c12101816e70cd7581dd544850758b191c36240f4ce2e966f13f242252ce4484 ] || fail 'Kernel BTF differs from the provided boot image.'
module="$base/droidspaces_ns-6.6.118-plc110-test.ko"
[ -f "$module" ] && [ -f "$base/SHA256SUMS" ] || fail 'Keep the .ko and SHA256SUMS beside this script.'
(cd "$base" && sha256sum -c SHA256SUMS) >> "$log" 2>&1 || fail 'Payload checksum verification failed.'
ksud=${KSUD:-/data/adb/ksud}
[ -x "$ksud" ] || fail 'ksud not found; set KSUD to its executable path.'
active=1
[ "$mode" != selftest ] || active=0
say "Loading with plc110_test=1 activate=$active"
"$ksud" insmod "$module" plc110_test=1 "activate=$active" >> "$log" 2>&1
status=$?
collect
[ "$status" = 0 ] || fail "ksud returned $status; kernel diagnostics are in load.log."
[ "$(cat /sys/module/droidspaces_ns/parameters/plc110_test 2>/dev/null)" = Y ] || fail 'Loaded module did not expose the expected device-test parameter.'
if [ "$mode" = selftest ]; then
    [ "$(cat /sys/module/droidspaces_ns/parameters/activate)" = N ] || fail 'Unexpected activation state.'
    rmmod droidspaces_ns >> "$log" 2>&1 || fail 'Selftest completed, but module unload failed.'
    say 'PASS: core selftest completed and module unloaded.'
else
    [ "$(cat /sys/module/droidspaces_ns/parameters/activate)" = Y ] || fail 'Activation flag is not set.'
    for name in pid pid_for_children ipc; do
        [ -e "/proc/self/ns/$name" ] || fail "Missing namespace interface: $name"
    done
    say 'LOADED: namespace hooks are active; reboot is required to remove them.'
fi
say "Log: $log"
