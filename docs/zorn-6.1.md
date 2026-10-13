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

## 安装与重启

**当前这台设备上的模块是手动 `insmod` 加载的，并且已经永久 pin。重启后它会消失，
内核回到原状（`/proc/self/ns` 里没有 `pid` / `ipc`），Droidspaces 会重新报缺 2 项
必需能力。** 这是预期行为，不是故障。

要让它在每次开机自动生效，安装 Release 里的 ZIP：

1. 把 `Droidspaces-LKM-KernelSU-v0.3.0-zorn.zip` 拷到手机；
2. KernelSU 管理器 → 模块 → 从本地安装 → 选择该 ZIP；
3. **重启设备**（KernelSU 模块在重启后由 `post-fs-data.sh` / `service.sh` 生效）；
4. 开机后确认管理器简介显示 `[已加载]`。

注意：

- 安装包沿用模块 ID `droidspaces_lkm`，与 PLZ110 的 v0.1.5 包**相同**。两个包
  面向不同内核（6.12.69 canoe vs 6.1.138 zorn），不要同时安装；装错的那个会因为
  内核/BTF 不匹配而拒绝加载，并在简介写明原因。
- 模块激活后永久 pin，`rmmod` 不成立。**卸载、替换版本、改 `common.sh` 都需要重启
  才生效**：先重启，再在管理器里改动，然后再次重启。
- 本包不修改 boot 分区，不写开机服务到别处，全部逻辑通过 KernelSU 模块目录下的
  `post-fs-data.sh` / `service.sh` 执行。

## 重启后如何确认

```sh
su -c 'ls /proc/self/ns'                       # 应含 pid, pid_for_children, ipc
su -c 'cat /sys/module/droidspaces_ns/parameters/activate'   # 应为 Y
su -c 'cat /data/adb/modules/droidspaces_lkm/status.txt'     # 简介文本
su -c 'cat /data/adb/modules/droidspaces_lkm/load.log'       # 本次开机日志
su -c '/data/local/Droidspaces/bin/droidspaces check'        # 必需项应全部 ✓
```

`service.sh` 每次开机都会把 `nsprobe` 的输出追加进 `load.log`，因此日志里应当能看到
`PASS sysv-create` … `PASS pidns-init-is-pid1` 这一整段。

## 故障排查

| 现象 | 原因与处理 |
| --- | --- |
| 简介 `[不支持]` | 内核 release 或 BTF SHA-256 与验证目标不一致（常见于系统 OTA 之后）。确认 `uname -r` 与 `/sys/kernel/btf/vmlinux` 的 sha256；若已升级内核，需要按新内核重新构建 |
| 简介 `[加载失败]` | 载荷缺失/校验失败，或 `ksud insmod` 返回非 0。看 `load.log` |
| 简介 `[部分加载]` | namespace 已激活但 `nsprobe` 有 `FAIL`。看 `load.log` 中探针输出的失败项 |
| 开机后 `/proc/self/ns` 没有 `pid` | 模块未生效：确认模块已启用、已重启、`load.log` 里有 `entry preflight passed` |
| `dmesg \| grep droidspaces` 出现 `Unknown symbol` | 装成了不匹配的载荷（例如官方 `android14-6.1` GKI 包）。改用本设备包 |
| `[!] Hardware access mode ... does not support devtmpfs` | 本机 `CONFIG_DEVTMPFS=n`，本包不含该项实现。容器会退回 tmpfs，`/dev` 内容变少、GPU/部分硬件节点可能缺失，但容器正常启动与使用 |
| `[✗] Sandboxing (user namespaces)` | 本机 `CONFIG_USER_NS=n`，需要 `--allow-sandboxing` 的负载（Docker/Podman/Bubblewrap）不可用 |

## 内核更新之后

模块把入口替换成**编译时确定的绝对地址与结构体偏移**，因此任何内核变化都必须重新
构建与验证：换 DDK 源码树/`vmlinux` 的布局输入、重新编译、重新比对设备 BTF、
更新 `kernelsu-zorn/common.sh` 里的 `uname -r` 与 BTF 指纹，然后重新打包。
不要通过放宽加载器检查来"兼容"另一个内核。

实现细节见 [实现机制](mechanism.md)。

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
