#!/bin/sh
set -eu

# Respect ANDROID_SERIAL when several phones are connected.
adb devices -l >&2
if [ "$(adb get-state 2>/dev/null || true)" != device ]; then
	echo 'No uniquely selected online adb device; connect one or set ANDROID_SERIAL.' >&2
	exit 1
fi
adb shell su <<'DEVICE'
id
uname -a
getprop ro.product.device
getprop ro.build.version.release
getprop ro.kernel.version
cat /proc/version
ls -l /proc/self/ns
cat /proc/filesystems
wc -l /proc/modules
cat /proc/sys/kernel/modules_disabled
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz | grep -E '^(# )?CONFIG_(SYSVIPC|POSIX_MQUEUE|NAMESPACES|PID_NS|IPC_NS|UTS_NS|USER_NS|NET_NS|CGROUPS|CGROUP_DEVICE|MEMCG|CGROUP_PIDS|CFS_BANDWIDTH|DEVTMPFS|MODULES|MODULE_UNLOAD|MODVERSIONS|MODULE_SIG.*|KALLSYMS.*|CFI.*|LTO.*|ARM64_[0-9]+K_PAGES)(=| )'
fi
for ksud in /data/adb/ksud /data/adb/ksu/bin/ksud; do
    if [ -x "$ksud" ]; then
        "$ksud" --version
        "$ksud" insmod --help
    fi
done
if [ -x /data/local/Droidspaces/bin/droidspaces ]; then
    DS_NO_PROXY=1 /data/local/Droidspaces/bin/droidspaces check
fi
DEVICE
