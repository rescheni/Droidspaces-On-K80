# Droidspaces LKM v0.1.1 — PLZ110

为 PLZ110 指定 6.12.69 内核增加可动态加载的 devtmpfs，并与已验证的
PID / IPC namespace 载荷一起提供 KernelSU 开机安装包。

## 适用范围

- PLZ110 / `qcom,canoe`，ARM64，4 KiB 页面。
- 完整内核版本：`6.12.69-android16-6-g242e1a07f878-ab15865446-4k`。
- 安装和开机加载前检查完整内核版本及 BTF SHA256，其他内核拒绝加载。
- 不适用于 PLC110 6.6；此前 v0.2.0 的六组 GKI 产物保留，本版为独立设备维护版本。

## 下载与安装

优先下载 `Droidspaces-LKM-KernelSU-v0.1.1-PLZ110.zip`，在 KernelSU
管理器安装后重启。作者为 Shikochin；模块 ID 与旧版相同，可替换原安装包。
`versionCode=3` 高于此前 v0.2.0 安装包的值，便于现有设备安装本次维护版本。

安装包先加载 PID / IPC，再加载 devtmpfs。简介只有在两者均通过检查时
显示“已加载”，否则显示失败或部分加载，日志位于模块目录 `load.log`。
这是开机时的状态快照，不是持续监控。

独立文件供已核验目标的手动测试：

- `droidspaces_ns-6.12.69-canoe.ko`：沿用 v0.1.0 真机验证载荷。
- `droidspaces_devtmpfs-6.12.69-plz110.ko`：新增 devtmpfs 载荷。
- `SHA256SUMS`：两个 `.ko` 与 ZIP 的 SHA256 校验值。

独立加载顺序如下；已加载相同模块时不要重复执行：

```sh
/data/adb/ksud insmod ./droidspaces_ns-6.12.69-canoe.ko activate=1
/data/adb/ksud insmod ./droidspaces_devtmpfs-6.12.69-plz110.ko
```

## 验证

devtmpfs 经固定 GKI 和 PLZ110 原始内核的 QEMU 回归，并在 PLZ110 真机
保持 SELinux Enforcing 的现有 KernelSU 环境中通过测试：已有节点、
共享挂载、设备读写、8 轮热插拔、节点权限、删除及保留 FD 行为正常，
`ready=Y`、`errors=0`。Droidspaces 检查确认 PID、IPC 和 devtmpfs 可用。

模块保留已授权加载进程的凭据执行设备节点操作，不修改 SELinux 策略。
完整容器启动、长期压力及所有驱动路径尚未覆盖；激活后的模块固定到本次启动，
替换或移除必须重启。本版继续作为实验性预发布提供。

## 后续

本版不包含 Sandboxing（user namespace）；Droidspaces 的该检查仍会显示
不可用。该项留待 v0.1.2，在现有内核、纯 LKM 的限制下继续适配。
