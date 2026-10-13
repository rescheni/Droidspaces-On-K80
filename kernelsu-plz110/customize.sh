#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
[ "$KSU" = true ] || abort '! 请在 KernelSU 管理器中安装。'
MODDIR=$MODPATH
. "$MODDIR/common.sh"
ds_target_supported || abort '! 仅支持 PLZ110 / canoe 的指定 6.12.69 内核。'
ds_abi_compatible || abort '! 内核 BTF 与已验证设备不匹配。'
[ -x "$DS_KSUD" ] || abort '! 未找到 ksud。'
"$DS_KSUD" insmod --help >/dev/null 2>&1 || abort '! 此 ksud 不支持 insmod。'
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/bin/userns-check" 0 0 0755
ds_payload_valid || abort '! .ko 缺失或 SHA-256 校验失败。'
ui_print '- 作者：Shikochin'
ui_print '- PLZ110 v0.1.5：PID / IPC + devtmpfs + Sandboxing + 沙箱 FUSE + Docker 防火墙扩展。'
ui_print '- 已匹配内核版本、设备及 BTF 指纹。'
ui_print '- 下次开机自动加载；简介区分成功、失败和部分加载。'
ui_print '- 激活后的 .ko 必须重启才能替换或移除。'
