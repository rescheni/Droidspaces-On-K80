#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
[ "$KSU" = true ] || abort '! 请在 KernelSU 管理器中安装。'
MODDIR=$MODPATH
. "$MODDIR/common.sh"
ds_target_supported || abort "! $DS_REASON"
[ -x "$DS_KSUD" ] || abort '! 未找到 ksud。'
"$DS_KSUD" insmod --help >/dev/null 2>&1 || abort '! 此 ksud 不支持 insmod。'
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/bin/nsprobe" 0 0 0755
ds_payload_valid || abort '! .ko 缺失或 SHA-256 校验失败。'
ui_print '- 作者：Shikochin'
ui_print "- 已匹配内核：$DS_KERNEL_RELEASE"
ui_print '- 已匹配内核 BTF 指纹；不匹配的内核会拒绝加载。'
ui_print '- 下次开机自动加载，并运行用户态检查后更新简介。'
ui_print '- 激活后的 .ko 必须重启才能替换或移除。'
