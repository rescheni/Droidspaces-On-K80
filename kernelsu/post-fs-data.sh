#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
. "$MODDIR/common.sh"
# Only reset the description here; loading is left to non-blocking service stage.
ds_status '[待加载] 本次开机尚未加载；等待 service 阶段激活 PID / IPC namespace。'
