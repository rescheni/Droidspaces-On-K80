#!/bin/sh
# Load only onto the supported kernel. The active module requires reboot to remove.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module=${1:-"$root/out/android16-6.12-ns/droidspaces_ns.ko"}
test -f "$module"
adb devices -l
[ "$(adb get-state)" = device ]
adb shell su <<'DEVICE'
set -eu
[ "$(uname -r)" = '6.12.69-android16-6-g242e1a07f878-ab15865446-4k' ]
[ ! -d /sys/module/droidspaces_ns ] || { echo 'Module already loaded; reboot is needed to replace it.' >&2; exit 1; }
mkdir -p /data/local/tmp/droidspaces-lkm
DEVICE
adb push "$module" /data/local/tmp/droidspaces-lkm/droidspaces_ns.ko
adb shell su <<'DEVICE'
set -eu
/data/adb/ksud insmod /data/local/tmp/droidspaces-lkm/droidspaces_ns.ko activate=1
[ "$(cat /sys/module/droidspaces_ns/parameters/activate)" = Y ]
DS_NO_PROXY=1 /data/local/Droidspaces/bin/droidspaces check
getenforce
DEVICE
