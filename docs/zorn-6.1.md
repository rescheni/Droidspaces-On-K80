# zorn / 6.1.138 设备包

面向 **Redmi K80 Pro（`zorn` / `24117RK2CC`）** 的厂商 6.1 内核，其内核未编入任何
namespace 与 IPC 支持：

```text
6.1.138-android14-11-g965475777129-mi
```

| 配置 | 状态 |
| --- | --- |
| `CONFIG_PID_NS` / `CONFIG_IPC_NS` | 未设置 |
| `CONFIG_SYSVIPC` / `CONFIG_POSIX_MQUEUE` | 未设置 |
| `CONFIG_USER_NS` / `CONFIG_DEVTMPFS` | 未设置 |
| 页面大小 / 架构 | 4096 / aarch64 |
| Root | KernelSU（`su` → `uid=0 u:r:ksu:s0`，`ksud insmod` 可用） |
| SELinux | Enforcing |

`src/port61full` 是唯一适用于这种“全缺失 6.1”基线的新源码集合；官方
`android14-6.1` 载荷（`src/port61`）要求宿主自带 IPC namespace 与 POSIX mqueue，
在这台设备上会在符号解析阶段失败：

```text
droidspaces_ns: Unknown symbol init_ipc_ns (err -2)
droidspaces_ns: Unknown symbol mq_init_ns (err -2)
```

## 构建

不依赖 docker：DDK 的生成目录通过绝对路径 `/opt/ddk/src/...` 引用源码树，因此把
解包出的 DDK 根目录在私有 user+mount namespace 内 bind-mount 到 `/opt/ddk` 即可。
镜像自带的 `modpost` / `resolve_btfids` / `pahole` 链接了比本机更新的 glibc，
`scripts/fix-ddk-hosttools.sh` 会用固定源码重编前两者，并让 `pahole` 走镜像自带
的 loader。

```sh
DDK=$HOME/opt/ddk/rootfs/opt/ddk
scripts/build-local-ddk.sh $DDK/kdir/android14-6.1 $DDK/src/android14-6.1 $DDK \
    TARGET=namespace PORT_DIR=src/port61full \
    GKI_ANDROID=android14 GKI_VERSION=6.1 OUT=$PWD/out/port61full -j4

tests/device/build-nsprobe.sh out/zorn/nsprobe $DDK
```

## 打包

`build/zorn-6.1.json` 固定了源码集、模块、探针、设备 BTF 与设备证据的 SHA-256；
任一输入变化都会让打包失败，因此一个 ZIP 只对应一次已验证的构建。

```sh
scripts/collect-zorn-evidence.sh out/zorn      # 采集设备证据（需要 adb）
python3 scripts/package-zorn.py --record       # 仅在建包前更新固定哈希
python3 scripts/package-zorn.py
```

产物为 `Droidspaces-LKM-KernelSU-v0.3.0-zorn.zip`、独立 `.ko` 与 `SHA256SUMS`。

## 加载器行为

`kernelsu-zorn/common.sh` 在加载前要求：

1. `uname -m` = `aarch64`；
2. `uname -r` 与 `6.1.138-android14-11-g965475777129-mi` 完全一致；
3. `/sys/kernel/btf/vmlinux` 的 SHA-256 等于
   `80a145adb8665abc5fc495250843144f5b006caf43c62a080ea34ca8fa0079bb`；
4. 模块载荷与 `SHA256SUMS` 校验通过。

任一条不满足即拒绝加载并在管理器简介与 `load.log` 中说明原因。加载成功后运行
`bin/nsprobe`，用真实系统调用确认 PID / IPC / POSIX mqueue 行为，再据此设置简介：

| 简介 | 含义 |
| --- | --- |
| `[已加载]` | namespace 已激活，且用户态检查全部通过 |
| `[部分加载]` | namespace 已激活，但用户态检查未通过 |
| `[不支持]` / `[加载失败]` | 内核不匹配或载荷/加载器失败，未接管任何入口 |

## 验证范围

已验证（真机，SELinux Enforcing，`activate=1`）：

- `Droidspaces v6.6.0` 需求检查：`[✓] PID namespace`、`[✓] IPC namespace`，
  `All required features found!`；
- `tests/device/nsprobe.c`：SysV 消息队列创建与收发、SysV 对象在新 IPC
  namespace 中不可见、POSIX 队列创建与收发、队列在新 IPC namespace 中不可见、
  子 PID namespace 的 init 进程 `getpid() == 1` 且 `getppid() == 0`。

未验证：完整 Droidspaces rootfs 启动、长期高并发 / OOM / 故障注入、
`CONFIG_USER_NS` 与 `CONFIG_DEVTMPFS`（本包不含这两项）、SIGEV_THREAD 通知、
32 位兼容 IPC 表。

激活后模块永久 pin，替换或移除 `.ko` 必须重启。
