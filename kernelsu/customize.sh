#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
[ "$KSU" = true ] || abort '! 请在 KernelSU 管理器中安装。'
MODDIR=$MODPATH
. "$MODDIR/common.sh"
ds_target_supported || abort "! $DS_REASON"
[ -x "$DS_KSUD" ] || abort '! 未找到 ksud。'
"$DS_KSUD" insmod --help >/dev/null 2>&1 || abort '! 此 ksud 不支持 insmod。'
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/abi-check" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
ds_payload_valid || abort '! .ko 缺失或 SHA-256 校验失败。'
ui_print '- 作者：Shikochin'
ui_print "- 已选择 KMI：$DS_KMI"
ui_print '- 开机加载前检查内部 ABI；不匹配时会记录原因。'
ui_print '- 下次开机自动加载；管理器简介会显示本次加载状态。'
ui_print '- 当前已激活的 .ko 需重启才会移除；禁用/卸载 ZIP 后请重启。'
