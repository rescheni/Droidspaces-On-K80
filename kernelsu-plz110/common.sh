#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
DS_RELEASE=6.12.69-android16-6-g242e1a07f878-ab15865446-4k
DS_BTF_SHA256=1870e245673b3eaae0d1fdafb0fd7027888b638018ed9561943a51dc2539d6af
DS_COMPATIBLE=/sys/firmware/devicetree/base/compatible
DS_BTF=/sys/kernel/btf/vmlinux
DS_MODULE_SYS=/sys/module/droidspaces_ns
DS_DEV_SYS=/sys/module/droidspaces_devtmpfs
DS_COMPAT_SYS=/sys/module/droidspaces_compat
DS_NETFILTER_SYS=/sys/module/droidspaces_netfilter
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

ds_target_supported() {
    [ "$(uname -m)" = aarch64 ] &&
    [ "$(uname -r)" = "$DS_RELEASE" ] &&
    [ -r "$DS_COMPATIBLE" ] &&
    tr '\000' '\n' < "$DS_COMPATIBLE" | grep -qx 'qcom,canoe'
}

ds_abi_compatible() {
    [ -r "$DS_BTF" ] || return 1
    ds_hash=$(sha256sum "$DS_BTF") || return 1
    [ "${ds_hash%% *}" = "$DS_BTF_SHA256" ]
}

ds_ns_active() {
    [ "$(cat "$DS_MODULE_SYS/parameters/activate" 2>/dev/null)" = Y ] &&
    [ -e "$DS_PROC_NS/pid" ] && [ -e "$DS_PROC_NS/pid_for_children" ] &&
    [ -e "$DS_PROC_NS/ipc" ]
}

ds_dev_active() {
    [ "$(cat "$DS_DEV_SYS/parameters/ready" 2>/dev/null)" = Y ] &&
    [ "$(cat "$DS_DEV_SYS/parameters/errors" 2>/dev/null)" = 0 ]
}

ds_docker_requested() {
    [ -e "$MODDIR/kernel/droidspaces_compat.ko" ] ||
    [ -e "$MODDIR/kernel/droidspaces_netfilter.ko" ]
}

ds_payload_valid() {
    if ds_docker_requested; then
        for ds_extra in droidspaces_compat droidspaces_netfilter; do
            [ -s "$MODDIR/kernel/$ds_extra.ko" ] || return 1
            grep -Eq "^[0-9a-f]{64}  kernel/$ds_extra[.]ko$" "$MODDIR/SHA256SUMS" || return 1
        done
    fi
    [ -x "$MODDIR/bin/userns-check" ] &&
    [ -s "$MODDIR/kernel/droidspaces_ns.ko" ] &&
    [ -s "$MODDIR/kernel/droidspaces_devtmpfs.ko" ] &&
    (cd "$MODDIR" && sha256sum -c SHA256SUMS)
}

ds_sandbox_available() {
    [ "$(cat "$DS_MODULE_SYS/parameters/userns_device_test" 2>/dev/null)" = Y ] &&
    [ -r "$DS_MODULE_SYS/parameters/userns_live" ] && [ -e "$DS_PROC_NS/user" ]
}

ds_docker_load() {
    ds_docker_requested || return 0
    for ds_extra in droidspaces_compat droidspaces_netfilter; do
        case "$ds_extra" in
            droidspaces_compat) ds_extra_sys=$DS_COMPAT_SYS ;;
            droidspaces_netfilter) ds_extra_sys=$DS_NETFILTER_SYS ;;
        esac
        if [ ! -d "$ds_extra_sys" ]; then
            if [ ! -x "$DS_KSUD" ] || ! "$DS_KSUD" insmod "$MODDIR/kernel/$ds_extra.ko"; then
                ds_status "[部分加载] namespace / devtmpfs 已激活；${ds_extra} 加载失败，详见 load.log。"
                return 1
            fi
        fi
    done
    if [ ! -r "$DS_COMPAT_SYS/parameters/balanced" ] || [ ! -d "$DS_NETFILTER_SYS" ]; then
        ds_status '[部分加载] Docker 扩展状态不完整；请查看 load.log 并重启。'
        return 1
    fi
    return 0
}

ds_health() {
    if ! "$MODDIR/bin/userns-check"; then
        ds_status '[部分加载] PID / IPC + devtmpfs 已加载；Sandboxing 功能检查失败，详见 load.log。'
        return 1
    fi
    if ds_docker_requested; then
        ds_status '[已加载] namespace / devtmpfs / 沙箱 FUSE 正常；PID 引用修复与 Docker 防火墙扩展已加载；更换 .ko 需重启。'
    else
        ds_status '[已加载] PID / IPC + devtmpfs 正常；Sandboxing 身份映射与权限检查通过，含沙箱 FUSE 支持；更换 .ko 需重启。'
    fi
}

ds_load() {
    if ! ds_target_supported; then
        ds_status '[不支持] 仅适配 PLZ110 / canoe 的指定 6.12.69 内核，未加载。'
        return 1
    fi
    if ! ds_abi_compatible; then
        ds_status '[不兼容] 内核 BTF 指纹与真机验证基线不同，未加载。'
        return 1
    fi
    if ! ds_payload_valid; then
        ds_status '[加载失败] 载荷缺失或 SHA-256 校验失败；请重新安装完整 ZIP。'
        return 1
    fi
    if [ -d "$DS_MODULE_SYS" ] && ! ds_ns_active; then
        ds_status '[加载失败] PID / IPC 模块存在但未激活；请重启后重试。'
        return 1
    fi
    if ds_ns_active && ! ds_sandbox_available; then
        ds_status '[需重启] 当前 PID / IPC 模块不含本版 Sandboxing；请重启以加载新版本。'
        return 1
    fi
    if [ -d "$DS_DEV_SYS" ] && ! ds_dev_active; then
        ds_status '[加载失败] devtmpfs 已存在但 ready/errors 检查未通过；请查看 load.log 并重启。'
        return 1
    fi
    if ds_ns_active && ds_dev_active; then
        ds_docker_load || return 1
        ds_health
        return $?
    fi
    if [ ! -x "$DS_KSUD" ]; then
        ds_status '[加载失败] 找不到可执行的 ksud；请检查 KernelSU。'
        return 1
    fi
    if ! ds_ns_active; then
        ds_status '[加载中] 正在激活 PID / IPC / user namespace。'
        "$DS_KSUD" insmod "$MODDIR/kernel/droidspaces_ns.ko" activate=1 userns_device_test=1
        ds_result=$?
        if [ "$ds_result" -ne 0 ] || ! ds_ns_active || ! ds_sandbox_available; then
            ds_status "[加载失败] PID / IPC 返回 ${ds_result} 或激活检查未通过；详见 load.log。"
            return 1
        fi
    fi
    if ! ds_dev_active; then
        ds_status '[加载中] PID / IPC / user namespace 已激活，正在加载 devtmpfs。'
        "$DS_KSUD" insmod "$MODDIR/kernel/droidspaces_devtmpfs.ko"
        ds_result=$?
        if [ "$ds_result" -ne 0 ] || ! ds_dev_active; then
            ds_status "[部分加载] namespace 已激活；devtmpfs 返回 ${ds_result} 或 ready/errors 检查未通过；请查看 load.log 并重启。"
            return 1
        fi
    fi
    ds_docker_load || return 1
    ds_health
}
