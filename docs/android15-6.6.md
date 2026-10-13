# Android 15 / Linux 6.6 GKI 实验构建

本分支提供独立的 6.6 移植源码、构建脚本和 QEMU 测试入口。
目标是验证在未启用 PID / IPC namespace 的 Android 15 GKI 基线上动态补充这些能力。
该产物尚未验证任何 6.6 真机，不属于 v0.1.0 的设备支持范围。

## 构建基线

| 项目 | 值 |
| --- | --- |
| DDK 标签 | `android15-6.6-20260313` |
| 内核 release | `6.6.127-4k-g46a034eca005-dirty` |
| 架构 / 页面大小 | ARM64 / 4 KiB |
| 工具链 | Android Clang 18，r510928 |
| 宿主配置 | PID_NS、IPC_NS、SYSVIPC、POSIX_MQUEUE 未启用；CFI_CLANG 启用 |
| 源码 / 输出 | `src/port66/` / `out/android15-6.6-ns/` |

固定镜像：

```text
ghcr.io/ylarod/ddk-min@sha256:51159bc935c75a282fc65bc87e02f16bd098d1cdf55dc558c4fdd0fcf7a132e2
```

这是该 DDK 提供的具体 GKI 构建，release 中的 `dirty` 后缀保留原值。
它不能代表所有 Android 15 / 6.6 内核。代码依赖内部结构、未导出符号与函数入口，
GKI 的模块接口兼容性不覆盖这些依赖。

## 复现

需要 Docker；首次下载 DDK 镜像和准备虚拟机工具需要较多时间和磁盘空间。

```sh
./scripts/build-ddk-6.6.sh -j4
./tests/vm/prepare-6.6.sh
./tests/vm/run-6.6.sh
```

产物为 `out/android15-6.6-ns/droidspaces_ns.ko`。
测试日志为 `out/vm-6.6/console.log`。QEMU 使用匹配的 DDK 内核、2 个 vCPU、
2 GiB 内存和无网络、无磁盘的 initramfs，SELinux 关闭。

模块要求 Device Tree compatible 为 `linux,dummy-virt`，且完整 release 匹配已记录的
测试内核（上述 DDK 基线或下文 PLC110 boot image），
PLC110 真机测试可显式传入 `plc110_test=1`，仍只接受记录的 6.6.118 完整 release；
其他情况在符号解析和入口修改前返回 `EOPNOTSUPP`。默认加载执行可卸载的核心自测；
`activate=1` 接入 namespace 路径后持续驻留到重启。

## 适配内容

- 从固定 6.6 源码移植 PID、IPC、fork/exit、proc、SysV IPC 和 POSIX mqueue 实现。
- 保留 6.6 的 Android vendor exit hook、sysctl 回调签名及 FUSE 初始化逻辑。
- FUSE 在统一的 RCU 释放回调归还 PID namespace 引用，覆盖原生编译器内联的
  `fuse_conn_put` 路径；初版测试发现的残留引用已修复。
- 接入 perf、BPF task iterator、cgroup-v1 pidlist 的 PID namespace 引用管理。
- 接入容器 reboot 和 memfd namespace 策略。
- 6.6 基线没有 pidfd namespace ioctl，移除对应 6.12 hook，测试明确标记跳过。
- `SHM_HUGETLB` 返回 `EOPNOTSUPP`：宿主 MM 在 `CONFIG_SYSVIPC=n` 下内联了
  不识别 SysV hugepage 的判断，当前尚未补齐该路径。普通 SysV 共享内存不受此限制。

原始输入文件的 SHA-256 与移植来源见 [`src/port66/ORIGIN`](../src/port66/ORIGIN)。

## 验证范围

匹配的 DDK 内核 QEMU 测试通过，最终 `DS_VM_RESULT=0`：

- 3 轮核心自测加载 / 卸载成功，再激活完整 namespace 实现。
- PID 1、嵌套 PID、procfs、孤儿回收、setns、权限检查和容器 reboot 通过。
- perf 继承与保留 FD、BPF iterator 保留 FD、cgroup pidlist、FUSE 释放通过。
- memfd 可执行策略、seal、父级策略限制和宿主隔离通过。
- SysV 消息队列、信号量、SEM_UNDO、共享内存与 namespace 隔离通过；
  `SHM_HUGETLB` 明确拒绝的行为通过检查。
- POSIX mqueue、挂载、优先级、超时、通知、隔离和 unlink 后 FD 生命周期通过。
- 最终 `pid_live=0`、`ipc_live=0`、`ipc_tasks=0`；激活后拒绝卸载符合预期。
- 日志未出现 BUG、WARNING、Oops 或 Kernel panic。

静态核对：683 个共享结构体 / 联合体布局一致，20 个替换入口函数签名一致，
411 个模块依赖符号全部存在。运行时入口预检及 20 个系统调用、6 个 BPF 回调的
CFI 检查通过。这些检查不证明其他构建或未覆盖路径兼容。

共享构建和测试入口修改后，6.12 版本重新编译及 QEMU 回归测试同样通过；
已有 v0.1.0 发布二进制的 SHA-256 保持不变。

## PLC110 实际内核离线验证

另使用用户提供的 `6.6.118-android15-8` boot image 进行 QEMU 验证，
并适配其未启用进程事件 connector 的配置差异。输入哈希、结果及复现命令见
[PLC110 验证记录](plc110-6.6-vm.md)。真机测试通过 `plc110_test=1` 显式开启，见
[PLC110 手动测试说明](plc110-device-test.md)。

## 限制与后续设备适配

没有运行 6.6 真机上的 `ksud insmod`、Droidspaces check 或完整容器启动，
没有验证 Android SELinux Enforcing、32 位兼容任务、长期高并发和 OOM。
本实验不生成 KernelSU 自动加载包。PLC110 手动测试包包含加载脚本和校验值，
不替换现有 6.12 发布资产。

适配实际 6.6 设备仍需完整 `uname -r`、内核配置、BTF/符号及实际内核 Image，
对照结构布局、函数签名与入口指令完成检查，再进行匹配内核和真机验证。
仅修改 vermagic 或放宽设备检查不构成适配。
