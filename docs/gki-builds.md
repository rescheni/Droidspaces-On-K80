# GKI 构建与验证

本分支面向六组 ARM64、4 KiB GKI 基线。KMI 中的 Android 代际来自内核的
`uname -r`，不等于设备当前运行的 Android 系统版本。

| KMI | 独立源码 | 当前验证 |
| --- | --- | --- |
| android17-6.18 | `src/port618/` | 完整 QEMU 回归通过，含 namespace ID 和原生 pidfd ioctl 保留测试 |
| android16-6.12 | `src/port/` | 完整 QEMU 回归通过 |
| android15-6.6 | `src/port66/` | 完整 QEMU 回归通过 |
| android14-6.1 | `src/port61/` | 完整 QEMU 回归通过，含加载前已存在的 IPC namespace |
| android13-5.15 | `src/port515/` | 完整 QEMU 回归通过，保留 classic CFI / BTI / SCS |
| android12-5.10 | `src/port510/` | QEMU 回归通过；基线缺少 BTF，不运行 BPF iterator 用户态测试 |

所有 DDK 标签和不可变镜像摘要记录在 `build/gki-targets.json`。
每个源目录的 `ORIGIN` 记录原始实现及移植差异。

## 构建单个目标

依赖 Docker 和 Python 3.9 或更新版本。先准备下述测试镜像并运行
`python3 scripts/build-abi-check.py`，再构建和验证目标。以 5.15 为例：

```sh
python3 scripts/build-gki.py 5.15 -j2
python3 scripts/prepare-gki-vm.py 5.15
python3 tests/vm/run-gki.py 5.15
```

第一次运行 QEMU 测试前，需要按照 `tests/vm/Dockerfile` 构建
`droidspaces-lkm-vm:local` 测试镜像：

```sh
docker build --platform linux/arm64 -t droidspaces-lkm-vm:local tests/vm
```

每个目标独立输出到 `out/gki/<KMI>/`：

- `droidspaces_ns.ko`：该目标的模块。
- `build.json`：DDK 镜像、源文件哈希及模块哈希。
- `Image`、`kernel.btf`、`kernel.config`：对应 DDK 测试内核。
- `<KMI>.abi`：运行时需要满足的结构布局和函数原型约束。
- `<KMI>.audit.json`、`prepared.json`：静态审核及输入哈希。
- `vm/console.log`、`validation.json`：完整测试日志与模块、内核、日志哈希。

静态审核比较模块与原始内核的共享结构布局，并检查入口替换、BPF 回调及 syscall
包装函数的原型。运行测试验证创建、嵌套、切换、权限、回收、阻塞 IPC、消息通知、
保留 FD、FUSE 卸载及激活模块拒绝热卸载。目标基线不存在的 memfd namespace 策略
和 pidfd namespace ioctl 会明确标记为不适用。

6.1 基线原生启用了 IPC namespace 和 POSIX mqueue。此版本沿用原生对象、队列和
namespace FD，只补入 SysV IPC，并覆盖加载前已存在 namespace 的测试。
5.15 和 5.10 基线没有 PID namespace、SysV IPC 或 POSIX mqueue；模块实现全部相关后端。
这两个版本保留 classic CFI、BTI 和 SCS。5.10 基线无 BTF，不能加载 BPF iterator
用户态测试程序，因此该项明确跳过；perf、cgroup 和 FUSE 的生命周期仍实际测试。

## 运行时选择

KernelSU 模块根据内核版本解析 `androidXX-major.minor`，在 `kernel/manifest.tsv`
精确选择载荷。安装器检查 KMI 和文件校验值；启动时还会执行静态 ARM64 ABI 检查程序，
检查 4 KiB 页面大小，并将 `/sys/kernel/btf/vmlinux` 与对应约束比较。
无匹配 KMI、布局或原型不同均停止加载，并在管理器简介和 `load.log` 中记录。
BTF 不可读时，仅在 `/sys/kernel/notes` 与已验证基线逐字节相同时允许继续；
指纹匹配不会覆盖 BTF 检查失败。5.10 基线未编入 BTF，其参考类型由 DWARF 提取；
此构建在 VM 中验证了实际内核 notes 匹配的加载路径。

同一 KMI 保证的接口范围不涵盖本项目使用的所有内核内部实现。BTF 兼容只是一项
必要检查，不能证明厂商内核的行为完全一致，也不能替代真机稳定性测试。
直接运行 `ksud insmod ... gki=1 activate=1` 会绕过用户态 BTF 检查；推荐使用完整模块包。

## 打包发布

完成全部六个目标后执行：

```sh
python3 scripts/build-abi-check.py
python3 tests/module/test_loader.py
python3 tests/abi/test_check.py
python3 scripts/package-module.py --output out/release-v0.2.0
```

打包器要求六个目标全部存在，源码、模块、ABI 审核及测试日志哈希一致，且完整
运行测试通过。ABI 检查程序必须是静态 ARM64 ELF。输出包含六个独立 `.ko`、
一个 KernelSU ZIP 和 `SHA256SUMS`；ZIP 内包含 KMI 清单、约束及构建测试证据。

发布前还会针对实际 ZIP 执行六组载荷选择与 ABI 检查器集成测试：

```sh
python3 tests/module/check_release.py out/release-v0.2.0/Droidspaces-LKM-KernelSU-v0.2.0.zip
```

此测试使用实际模块、校验和及 ARM64 ABI 检查器，只模拟内核身份和 ksud；
不向宿主内核插入模块。真正的内核加载与功能测试由前述六组 QEMU 回归完成。
VM 加载辅助程序执行与 ksud 相应的符号重定位和模块加载步骤；它不是 Android
设备上的 ksud 进程。实际 KernelSU 安装后重启流程与真机稳定性尚待设备验证。

## v0.2.0 验证基线

| KMI | 固定测试内核 release | memfd namespace 策略 | pidfd namespace ioctl |
| --- | --- | --- | --- |
| android17-6.18 | `6.18.32-4k-g47cc22554442-dirty` | 已测试 | 已测试 |
| android16-6.12 | `6.12.76-4k-gae4e2f4f997e-dirty` | 已测试 | 已测试 |
| android15-6.6 | `6.6.127-4k-g46a034eca005-dirty` | 已测试 | 基线未提供 |
| android14-6.1 | `6.1.166-dirty` | 基线未提供 | 基线未提供 |
| android13-5.15 | `5.15.202-android13-5.15.202_r00-dirty` | 基线未提供 | 基线未提供 |
| android12-5.10 | `5.10.252-dirty` | 基线未提供 | 基线未提供 |

六个目标均通过三轮核心对象加载/卸载自测、激活后的 PID/IPC 功能测试、
引用回收检查，以及激活模块拒绝热卸载检查。PID、IPC 和任务侧表计数全部归零。
基线支持 BPF iterator 的五组均完成保留 FD 的生命周期测试；5.10 的缺项如上所述。
6.18 另验证初始与子 namespace 的非零且不同 ID，以及原生 PIDFD_GET_INFO、
FS_IOC_GETVERSION 和 mount namespace ioctl 的转发行为。

未出现内核崩溃或 WARN 回溯。加载过程中的外部模块 taint、签名/版本信息和
部分基线的模块 BTF 提示保留在日志中，不能将其描述为完全无警告加载。
ZIP 中的 `validation/<KMI>.console.log` 保存对应运行记录，哈希与 `validation.json` 绑定。
这些 GKI 记录不包含 v0.1.0 的真机验证，也不代表所有相同 KMI 的厂商内核均可加载。
