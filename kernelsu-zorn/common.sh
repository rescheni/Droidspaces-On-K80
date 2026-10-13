#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# MODDIR is supplied by the entry script (or MODPATH during installation).
DS_BTF=/sys/kernel/btf/vmlinux
DS_KERNEL_RELEASE='6.1.138-android14-11-g965475777129-mi'
DS_BTF_SHA256=80a145adb8665abc5fc495250843144f5b006caf43c62a080ea34ca8fa0079bb
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

# The module's internal hooks use offsets compiled from one exact kernel build,
# so both the release string and the kernel BTF must match byte for byte.
ds_target_supported() {
    DS_REASON='仅支持 ARM64。'
    [ "$(uname -m)" = aarch64 ] || return 1
    DS_REASON="仅支持内核 $DS_KERNEL_RELEASE，当前为 $(uname -r)。"
    [ "$(uname -r)" = "$DS_KERNEL_RELEASE" ] || return 1
    DS_REASON='无法读取 /sys/kernel/btf/vmlinux。'
    [ -r "$DS_BTF" ] || return 1
    DS_REASON='内核 BTF 与已验证的构建不一致。'
    [ "$(sha256sum "$DS_BTF" | cut -d' ' -f1)" = "$DS_BTF_SHA256" ] || return 1
    DS_REASON=''
    return 0
}

ds_active() {
    [ "$(cat "$DS_MODULE_SYS/parameters/activate" 2>/dev/null)" = Y ] &&
    [ -e "$DS_PROC_NS/pid" ] && [ -e "$DS_PROC_NS/pid_for_children" ] && [ -e "$DS_PROC_NS/ipc" ]
}

ds_payload_valid() {
    [ -s "$MODDIR/kernel/droidspaces_ns.ko" ] &&
    [ -x "$MODDIR/bin/nsprobe" ] &&
    [ -s "$MODDIR/SHA256SUMS" ] &&
    (cd "$MODDIR" && sha256sum -c SHA256SUMS)
}

# Run the shipped probe: it creates SysV objects, a POSIX queue and a nested
# PID namespace through the replaced entries, so it reports real behaviour
# rather than the presence of the syscall slots.
ds_runtime_ok() {
    ds_probe=$("$MODDIR/bin/nsprobe" 2>&1)
    printf '%s\n' "$ds_probe" >> "$MODDIR/load.log"
    printf '%s\n' "$ds_probe" | grep -q '^FAIL ' && return 1
    printf '%s\n' "$ds_probe" | grep -q 'PASS pidns-init-is-pid1' || return 1
    return 0
}

ds_load() {
    if ! ds_target_supported; then
        ds_status "[不支持] ${DS_REASON}；未加载。"
        return 1
    fi
    if ds_active; then
        if ds_runtime_ok; then
            ds_status '[已加载] 本次开机 PID / IPC namespace 已激活，用户态检查通过；更换 .ko 需重启。'
        else
            ds_status '[部分加载] namespace 已激活，但用户态检查未通过；详见 load.log。'
        fi
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
    ds_status '[加载中] 正在激活 PID / IPC namespace。'
    "$DS_KSUD" insmod "$MODDIR/kernel/droidspaces_ns.ko" gki=1 activate=1
    ds_result=$?
    if [ "$ds_result" -ne 0 ]; then
        ds_status "[加载失败] ksud insmod 返回 ${ds_result}；详见模块目录 load.log。"
        return 1
    fi
    if ! ds_active; then
        ds_status '[加载失败] insmod 已返回成功，但 activate 或 PID / IPC namespace 检查未通过；详见 load.log。'
        return 1
    fi
    if ds_runtime_ok; then
        ds_status '[已加载] PID / IPC namespace 已激活，用户态检查通过；更换 .ko 需重启。'
    else
        ds_status '[部分加载] namespace 已激活，但用户态检查未通过；详见 load.log。'
    fi
}
