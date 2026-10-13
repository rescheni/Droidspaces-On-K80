#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
. "$MODDIR/common.sh"
# Keep only this boot's attempt and the previous one.
[ ! -f "$MODDIR/load.log" ] || mv -f "$MODDIR/load.log" "$MODDIR/load.log.previous"
exec >> "$MODDIR/load.log" 2>&1
printf 'Droidspaces LKM: %s\n' "$(date -u)"
printf 'Kernel: %s\n' "$(uname -r)"
printf 'Boot: %s\n' "$(cat /proc/sys/kernel/random/boot_id)"
ds_load
