# Droidspaces LKM

> AI Slop Warning，这个项目仅有这句警告是我自己写的，适用于 GKI 的 .ko 不保证可用于每个设备，**出了问题建议先问 Agent**，比问我高效很多，欢迎提交适配对应机型的 PR

为 Android 内核动态补充 Droidspaces 所需的 PID / IPC namespace、devtmpfs 及 Sandboxing（user namespace）能力。
通过 KernelSU 的 `ksud insmod` 加载，无需刷写内核，设备运行时无需内核源码。

v0.1.5 是 PLZ110 指定 6.12.69 内核的设备版本，提供 PID / IPC + devtmpfs +
Sandboxing、沙箱 FUSE，以及 Docker 所需的 `xt_addrtype` / nftables 扩展。
本版还修复厂商信号回调导致的 namespace 引用泄漏，并避免子 user namespace 的原生 OverlayFS 告警。已在真机 SELinux Enforcing 环境中，通过身份与权限隔离测试，以及
实际 Droidspaces 容器内的 Bubblewrap 工作负载。实现和验证范围见
[user namespace 移植状态](docs/userns-status.md)。

v0.2.0 是此前发布的独立 GKI 实验版本，提供六份 PID / IPC 构建，不含
devtmpfs；它不替代 PLZ110 专用版本。同一 KMI 不代表厂商内部 ABI 兼容。

## PLZ110 v0.1.5

仅支持 `qcom,canoe`、ARM64、4 KiB 页面及完整内核版本：

```text
6.12.69-android16-6-g242e1a07f878-ab15865446-4k
```

从 [v0.1.5 Release](https://github.com/Shikochin/Droidspaces-LKM/releases/tag/v0.1.5)
下载 `Droidspaces-LKM-KernelSU-v0.1.5-PLZ110.zip`，在 KernelSU 管理器中安装后
重启。安装包使用相同的 `droidspaces_lkm` 模块 ID，可替换已安装的旧版本；
安装和每次开机均要求设备、完整内核版本及 BTF 指纹与验证目标匹配。

开机依次加载 namespace、devtmpfs、厂商 PID 引用修复和 netfilter 四个 `.ko`，
随后以普通 UID 检查真实身份映射与权限拒绝行为，简介显示实际结果：

| 状态 | 含义 |
| --- | --- |
| `[已加载]` | PID / IPC、devtmpfs 和 Sandboxing 检查通过，PID 引用修复与 netfilter 已加载 |
| `[部分加载]` | namespace 已激活，但 devtmpfs、Sandboxing 或 Docker 扩展检查失败 |
| `[需重启]` | 当前仍是旧 namespace 模块，需重启应用更新 |
| `[加载失败]` | 载荷损坏、加载器失败或已有模块状态异常 |
| `[不兼容]` / `[不支持]` | 内核指纹或设备不匹配，停止加载 |

日志保存在 `/data/adb/modules/droidspaces_lkm/load.log`。状态反映开机加载时的
检查结果；后续 devtmpfs 错误可通过其 `errors` 模块参数查看。
已激活模块需重启才能替换或移除。

Release 同时提供四个独立 `.ko` 与 `SHA256SUMS`，建议使用包含目标校验的
完整 KernelSU ZIP。版本说明见 [v0.1.5 验证与安装](docs/releases-v0.1.5.md)。
已验证 HMA UID Fake 0.4.0+1c71b6c 和 NoMount 共存；未知入口修改会拒绝加载。
Droidspaces 容器需启用 `--allow-sandboxing`；不会自动更改现有容器设置。
沙箱内使用 FUSE 还需设备访问和 mount 权限；支持范围与配置见 [FUSE 适配](docs/userns-fuse.md)。

## zorn / 6.1.138 v0.3.0-zorn

面向 **Redmi K80 Pro（`zorn` / `24117RK2CC`）** 的厂商 6.1 内核，其内核既没有
PID / IPC namespace，也没有 SysV IPC 与 POSIX mqueue：

```text
6.1.138-android14-11-g965475777129-mi
```

这台设备上的官方 `android14-6.1` GKI 载荷（`src/port61`）会在符号解析阶段失败，
因为该载荷假定宿主已自带 IPC namespace 与 POSIX mqueue。本版使用新的
[`src/port61full`](src/port61full/README.md)：IPC namespace、SysV IPC 与
POSIX mqueue 全部由模块自带，并由真机验证。

```text
droidspaces_ns: Unknown symbol init_ipc_ns (err -2)   # 官方 6.1 载荷在本机
droidspaces_ns: Unknown symbol mq_init_ns (err -2)
```

安装与构建步骤、加载器判定条件、验证范围见 [zorn 6.1 设备包](docs/zorn-6.1.md)。
加载器要求内核 release 与 BTF SHA-256 同时匹配，并在激活后运行用户态检查，
据此把简介更新为 `[已加载]`、`[部分加载]`、`[加载失败]` 或 `[不支持]`。

安装：把 ZIP 装进 KernelSU 管理器后**重启**，`service.sh` 会在开机时加载并自检。
当前若只是手动 `insmod`，模块已永久 pin，重启后即失效——重启前后该做什么、如何确认、
常见失败原因见 [zorn 6.1 设备包 · 安装与重启](docs/zorn-6.1.md#安装与重启)。
本包只补 PID / IPC namespace 与 POSIX mqueue；`devtmpfs`、user namespace
（Sandboxing）在本机同样为 `n`，不在本包范围内。

为什么一个 `.ko` 能补上编译期缺失的内核子系统：入口替换、符号解析、syscall 表
接管、生命周期引用与失败回滚的完整说明见 [实现机制](docs/mechanism.md)。

## 容器里的 Linux 桌面

namespace 装上之后，Droidspaces 容器可以跑真正的图形桌面。本机用
Termux:X11（`com.termux.x11`）+ 容器内 bspwm 打通，一键脚本
[`scripts/x11-desktop.sh`](scripts/x11-desktop.sh)，开机自启走 KernelSU 的
`/data/adb/service.d/`。

两个必须知道的坑：Android 14+ 要求 `loader.apk` 只读（否则 `app_process` 以
`Writable dex file ... is not allowed` abort），以及 **X server 必须先于容器启动**
（Droidspaces 按文件 bind-mount socket，顺序错了容器会绑到已删除的 inode）。
完整安装步骤、故障对照表与 anland 路线对比见
[Droidspaces Linux 桌面](docs/x11-desktop.md)。

## GKI v0.2.0

下列六组构建和通用安装流程属于 v0.2.0，仅提供 PID / IPC namespace。

## 构建范围

所有产物均面向 ARM64、4 KiB 页面。KMI 中的 Android 代际取自 `uname -r`，
与设备当前运行的 Android 系统版本可能不同。

| KMI | 独立载荷 | 源码 |
| --- | --- | --- |
| android17-6.18 | `droidspaces_ns-android17-6.18-gki.ko` | `src/port618/` |
| android16-6.12 | `droidspaces_ns-android16-6.12-gki.ko` | `src/port/` |
| android15-6.6 | `droidspaces_ns-android15-6.6-gki.ko` | `src/port66/` |
| android14-6.1 | `droidspaces_ns-android14-6.1-gki.ko` | `src/port61/` |
| android13-5.15 | `droidspaces_ns-android13-5.15-gki.ko` | `src/port515/` |
| android12-5.10 | `droidspaces_ns-android12-5.10-gki.ko` | `src/port510/` |

每个目标使用独立源码和固定 DDK；镜像摘要见
[构建清单](build/gki-targets.json)，具体基线与验证范围见
[GKI 构建与验证](docs/gki-builds.md)。

同一 KMI 不代表所有内部 ABI 都兼容。启动脚本会检查结构布局与函数原型；
检查通过也不能替代厂商设备上的功能与稳定性测试。

## 功能

- PID namespace：创建、嵌套、切换、PID 1、procfs、孤儿回收及容器 reboot。
- IPC namespace：SysV 消息队列、信号量、共享内存及 POSIX 消息队列。
- namespace 引用管理：perf、FUSE、cgroup-v1 pidlist，以及基线支持的 BPF task iterator。
- 按基线提供 memfd namespace 策略、pidfd namespace ioctl 和 IPC proc/sysctl 接口。
- KernelSU：开机选取载荷、校验文件与 ABI、显示加载状态并保存日志。

6.1 基线已启用 IPC namespace 和 POSIX mqueue，因此沿用原生对象并补入 SysV IPC；
加载前已存在的 namespace、文件描述符及队列会保留。其他五组基线补入 PID / IPC
namespace 及消息队列后端。具体差异列于验证文档。

## 安装与使用

从 [v0.2.0 Release](https://github.com/Shikochin/Droidspaces-LKM/releases/tag/v0.2.0)
下载 `Droidspaces-LKM-KernelSU-v0.2.0.zip`。同一页面提供六个独立 `.ko` 和
`SHA256SUMS`。

1. 在支持 `ksud insmod` 的 KernelSU 管理器中安装 ZIP。
2. 重启设备，模块在 `service.sh` 阶段选择当前 KMI 对应的载荷并加载。
3. 刷新模块列表，确认简介显示 **`[已加载]`**。
4. 执行 `droidspaces check` 检查运行环境。

安装过程不立即加载内核模块。模块不替换系统文件，不需要 metamodule，也不修改
SELinux 策略。每次启动仅尝试加载一次，失败后保存原因，不循环重试。

加载前优先比较 `/sys/kernel/btf/vmlinux` 与随包约束；BTF 不可读时，只有
`/sys/kernel/notes` 与已验证基线完全相同才允许继续。BTF 检查失败不会退回指纹匹配。
不匹配的设备会停止加载，避免仅凭 KMI 强行使用不兼容的内部结构。

### 状态与日志

| 简介状态 | 含义 |
| --- | --- |
| `[待加载]` | 本次启动尚未执行加载 |
| `[加载中]` | 正在调用 `ksud insmod` |
| `[已加载]` | `activate=Y`，PID、pid_for_children、IPC namespace 接口存在 |
| `[加载失败]` | 文件校验、加载器、insmod 或激活检查失败 |
| `[不支持]` | 无法识别 KMI、架构不符或未包含对应载荷 |
| `[不兼容]` | 内核 ABI 或构建指纹检查失败 |

简介优先通过 KernelSU 临时 `override.description` 更新；不支持该接口时更新
`module.prop`，并在下一次 `post-fs-data.sh` 重置。模块目录
`/data/adb/modules/droidspaces_lkm/` 中包含 `load.log`、`load.log.previous` 和 `status.txt`。

**激活后的内核模块不能热卸载。** 禁用、卸载或更新安装包后，必须重启才能移除
当前内核中的模块。初次测试前请保存工作；发生重启时可保留 `/sys/fs/pstore/` 日志。

### 独立 .ko

独立模块供已核对目标内核的手动测试使用。例如，在 root shell 中加载 6.6：

```sh
/data/adb/ksud insmod /data/local/tmp/droidspaces_ns-android15-6.6-gki.ko gki=1 activate=1
```

该方式绕过安装包的用户态 ABI 检查。推荐通过完整 KernelSU ZIP 加载。
省略 `activate=1` 时仅执行入口预检和可卸载的核心对象自测；自测仍会临时注册
proc/sysctl 和 mqueue 接口。不要对已加载的模块重复执行 insmod。

## 构建与验证

需要 Docker 和 Python 3.9 或更新版本。先准备测试镜像与静态 ABI 检查器：

```sh
docker build --platform linux/arm64 -t droidspaces-lkm-vm:local tests/vm
python3 scripts/build-abi-check.py
```

每个目标独立构建和验证，以 6.6 为例：

```sh
python3 scripts/build-gki.py 6.6 -j2
python3 scripts/prepare-gki-vm.py 6.6
python3 tests/vm/run-gki.py 6.6
```

完成六个目标后运行加载器与 ABI 检查器测试，再打包：

```sh
python3 tests/module/test_loader.py
python3 tests/abi/test_check.py
python3 scripts/package-module.py --output out/release-v0.2.0
```

打包器核对源码、二进制、ABI 约束和完整 VM 回归的哈希，拒绝缺失、未验证或
已变化的产物。ZIP 内的 `validation.json` 保存各目标的来源与验证证据。
完整流程见 [GKI 构建与验证](docs/gki-builds.md)。

QEMU 回归覆盖 namespace 创建与销毁、隔离、权限、IPC 阻塞、SEM_UNDO、消息通知、
保留 FD 和卸载行为。它使用固定 GKI Image 与无磁盘、无网络 initramfs；
不能证明 Android 开机流程、设备驱动或完整 Droidspaces 容器的长期稳定性。

## 历史设备验证

[v0.1.0](https://github.com/Shikochin/Droidspaces-LKM/releases/tag/v0.1.0) 的固定
PLZ110 / canoe 6.12.69 载荷曾在真机 SELinux Enforcing 环境下通过功能测试和
Droidspaces 能力检查。该结果仅适用于当时的二进制，不能代替 v0.2.0 的真机验证。
详见 [原始移植状态](docs/port-status.md)。

PLC110 的指定 6.6.118 内核另有 [手动测试包说明](docs/plc110-device-test.md)，
已完成该 boot image 内核上的 QEMU 验证，真机结果待反馈。

## 限制

- 使用未导出的内核接口和实验性入口替换，不保证跨厂商或跨内核构建兼容。
- 不支持 32 位 IPC 兼容系统调用；`SHM_HUGETLB` 返回 `EOPNOTSUPP`。
- 已验证指定 PLZ110 上 Debian / systemd 和 rootful Docker 的部分工作负载；未覆盖长期高并发、OOM 和所有 SELinux 域。
- user namespace、devtmpfs 与 Docker 扩展请使用 PLZ110 专用 v0.1.5；通用 GKI v0.2.0 不包含这些扩展。
- PLZ110 专用版本支持普通沙箱 FUSE；原生 OverlayFS 在子 user namespace 中返回 `EOPNOTSUPP`，可使用 fuse-overlayfs。
- rootless Docker 的 seccomp / capability 限制未解决；未补入 br_netfilter，不支持关闭 Docker userland proxy 的已测路径。
- 不补充额外 cgroup 控制器。尚未覆盖 idmapped mount、全部内联权限路径及长期高负载，
  已验证 Bubblewrap 场景不代表完整 Linux USER_NS 等价性或所有沙箱应用均兼容。
- 原生已支持 PID namespace 的内核不属于本补丁的适用范围。

## 许可证与来源

项目采用 [GPL-2.0](LICENSE)，保留移植代码和依赖的版权声明。
各源码目录的 `ORIGIN` 记录 Android common kernel 原始文件哈希与改动。

- [KallRecon](https://github.com/Dere3046/KallRecon)：内核符号解析。
- [Android common kernel](https://android.googlesource.com/kernel/common/)：namespace 及相关实现。
- [KernelSU](https://github.com/tiann/KernelSU)：内核模块加载与启动框架。
