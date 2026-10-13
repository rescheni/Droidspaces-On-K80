#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# MODDIR is supplied by the entry script (or MODPATH during installation).
DS_BTF=/sys/kernel/btf/vmlinux
DS_KERNEL_NOTES=/sys/kernel/notes
DS_MODULE_SYS=/sys/module/droidspaces_ns
DS_PROC_NS=/proc/self/ns
DS_KSUD=/data/adb/ksud
[ -x "$DS_KSUD" ] || DS_KSUD=/data/adb/ksu/bin/ksud
export KSU_MODULE=droidspaces_lkm

# KernelSU clears --temp configuration at every boot, including for disabled modules.
# The file fallback also covers a missing/older ksud, so failures remain visible.
ds_status() {
    ds_message=$1
    printf '%s\n' "$ds_message"
    printf '%s\n' "$ds_message" > "$MODDIR/status.txt"
    if [ -x "$DS_KSUD" ] && "$DS_KSUD" module config set --temp override.description "$ds_message"; then
        return 0
    fi
    DS_DESCRIPTION="$ds_message" awk '
        !/^description=/ { print }
        END { print "description=" ENVIRON["DS_DESCRIPTION"] }
    ' "$MODDIR/module.prop" > "$MODDIR/module.prop.tmp" &&
        cat "$MODDIR/module.prop.tmp" > "$MODDIR/module.prop"
    rm -f "$MODDIR/module.prop.tmp"
}

# KMI generation comes from the kernel release, not the Android userspace version.
ds_target_supported() {
    DS_REASON='仅支持 ARM64 GKI。'
    [ "$(uname -m)" = aarch64 ] || return 1
    ds_release=$(uname -r)
    ds_family=$(printf '%s\n' "$ds_release" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\)\.[0-9][0-9]*-android[0-9][0-9]*-.*/\1/p')
    ds_android=$(printf '%s\n' "$ds_release" | sed -n 's/^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*-\(android[0-9][0-9]*\)-.*/\1/p')
    DS_REASON="无法从内核版本识别 GKI KMI：$ds_release"
    [ -n "$ds_family" ] && [ -n "$ds_android" ] || return 1
    DS_KMI="$ds_android-$ds_family"
    DS_REASON="安装包未包含匹配的 KMI：$DS_KMI"
    [ -r "$MODDIR/kernel/manifest.tsv" ] || { DS_REASON='缺少 KMI 清单。'; return 1; }
    ds_row=$(awk -F '|' -v kmi="$DS_KMI" '$1 == kmi { row=$0; count++ } END { if (count==1) print row; else exit 1 }' "$MODDIR/kernel/manifest.tsv") || return 1
    IFS='|' read -r ds_selected DS_PAYLOAD DS_ABI DS_NOTES ds_extra <<MANIFEST
$ds_row
MANIFEST
    DS_REASON='KMI 清单格式无效。'
    [ "$ds_selected" = "$DS_KMI" ] && [ -z "$ds_extra" ] || return 1
    for ds_name in "$DS_PAYLOAD" "$DS_ABI"; do
        case "$ds_name" in ''|.*|*[!a-zA-Z0-9._-]*) return 1;; esac
    done
    case "$DS_NOTES" in .*|*[!a-zA-Z0-9._-]*) return 1;; esac
    DS_REASON=''
    return 0
}

ds_active() {
    [ "$(cat "$DS_MODULE_SYS/parameters/activate" 2>/dev/null)" = Y ] &&
    [ -e "$DS_PROC_NS/pid" ] && [ -e "$DS_PROC_NS/pid_for_children" ] &&
    [ -e "$DS_PROC_NS/ipc" ]
}

ds_payload_valid() {
    [ -s "$MODDIR/kernel/$DS_PAYLOAD" ] &&
    [ -s "$MODDIR/kernel/$DS_ABI" ] &&
    [ -x "$MODDIR/bin/abi-check" ] &&
    { [ -z "$DS_NOTES" ] || [ -s "$MODDIR/kernel/$DS_NOTES" ]; } &&
    (cd "$MODDIR/kernel" && sha256sum -c SHA256SUMS)
}

# KMI is a routing key; internal namespace hooks also require matching host ABI.
ds_abi_compatible() {
    if [ -r "$DS_BTF" ]; then
        "$MODDIR/bin/abi-check" "$MODDIR/kernel/$DS_ABI" "$DS_BTF"
        return $?
    fi
    # Old GKI builds may omit BTF. Only the exact validated build is accepted.
    if [ -n "$DS_NOTES" ] && [ -r "$DS_KERNEL_NOTES" ] &&
        cmp -s "$DS_KERNEL_NOTES" "$MODDIR/kernel/$DS_NOTES"; then
        printf 'Kernel BTF unavailable; exact validated kernel notes matched.\n'
        return 0
    fi
    printf 'Kernel BTF unavailable and validated kernel notes do not match.\n'
    return 1
}

ds_load() {
    if ! ds_target_supported; then
        ds_status "[不支持] ${DS_REASON}；未加载。"
        return 1
    fi
    if ds_active; then
        ds_status "[已加载] ${DS_KMI}：PID / IPC namespace 已激活（本次开机已加载）；更换 .ko 需重启。"
        return 0
    fi
    if [ -d "$DS_MODULE_SYS" ]; then
        ds_status '[加载失败] droidspaces_ns 已存在但未激活，或 namespace 接口不完整；请重启后重试。'
        return 1
    fi
    if [ ! -x "$DS_KSUD" ]; then
        ds_status '[加载失败] 找不到可执行的 ksud；请检查 KernelSU，日志见模块目录 load.log。'
        return 1
    fi
    if ! ds_payload_valid; then
        ds_status '[加载失败] .ko 缺失或 SHA-256 校验失败；请重新安装完整模块 ZIP。'
        return 1
    fi
    if ! ds_abi_compatible; then
        ds_status "[不兼容] ${DS_KMI}：内核 ABI 或构建指纹校验失败，未加载；详见 load.log。"
        return 1
    fi
    ds_status "[加载中] ${DS_KMI}：正在激活 PID / IPC namespace。"
    "$DS_KSUD" insmod "$MODDIR/kernel/$DS_PAYLOAD" gki=1 activate=1
    ds_result=$?
    if [ "$ds_result" -ne 0 ]; then
        ds_status "[加载失败] ksud insmod 返回 ${ds_result}；详见模块目录 load.log。"
        return 1
    fi
    if ! ds_active; then
        ds_status '[加载失败] insmod 已返回成功，但 activate 或 PID / IPC namespace 检查未通过；详见 load.log。'
        return 1
    fi
    ds_status "[已加载] ${DS_KMI}：PID / IPC namespace 已激活；更换 .ko 需重启。"
}
