# PLZ110 devtmpfs 实验移植

本分支新增独立的 `droidspaces_devtmpfs.ko`，为已启动且未编译
`CONFIG_DEVTMPFS` 的内核补入 devtmpfs。2026-10-10 已在下述 PLZ110
内核上通过真机测试，并纳入 PLZ110 v0.1.1 的独立载荷与 KernelSU 安装包。

## 固定验证目标

| 项目 | 值 |
| --- | --- |
| 设备 | PLZ110 / `qcom,canoe` |
| 内核 | `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` |
| 内核 BTF SHA256 | `1870e245673b3eaae0d1fdafb0fd7027888b638018ed9561943a51dc2539d6af` |
| devtmpfs 测试载荷 SHA256 | `7146e136fc4281e31c58f04ccae31fc0ec2e1b83e9e460b5e6e98355f9a16a00` |
| 配合使用的 v0.1.0 PID/IPC 载荷 SHA256 | `33ed20ce38f092d98f4379701d45127522370d13f3d59ba6458916a0b6f592f4` |

这是固定厂商内核的验证结果，不代表其他 6.12 内核兼容。v0.2.0 通用
GKI 安装包在该设备上仍会因内部 ABI 检查失败而停止加载；本次测试手动
加载已验证的 PLZ110 PID/IPC 载荷。v0.1.1 提供独立的固定目标安装包，
位于 `kernelsu-plz110/`，使用完整 BTF 指纹门控，不放宽通用 GKI ABI 检查。

## 实现

保留 Android common `drivers/base/devtmpfs.c` 的共享 tmpfs、设备命名、
权限、所有者、同步工作队列和节点删除语义。晚加载路径使用独立
`fs_struct` 和内部挂载，避免调用启动后已释放的初始化函数。

模块核对 `device_add` / `device_del` 入口并安装跳转，补齐已有设备，
随后同步处理设备注册和移除。已有设备枚举持有对象引用，跳过注册或
移除过程中的对象；执行原始驱动回调时不持有节点更新锁。

工作线程保留加载进程的凭据，用于 VFS 操作。第一版使用内核线程默认
凭据，在真机创建节点时全部返回 `EPERM`；修正版使用已授权的 KernelSU
加载凭据后通过测试。系统保持 SELinux Enforcing，没有修改 SELinux
策略；该结果限于现有 KernelSU 权限环境，并不表示所有 SELinux 域均
可创建或访问这些节点。

激活后模块固定到本次启动，替换或移除必须重启。

## 验证结果

GKI 6.12 基线和 PLZ110 原始 boot image 中的内核均通过 QEMU 回归。
同一 devtmpfs 候选载荷随后经 `ksud insmod` 在 PLZ110 上加载，结果为：

- `ready=Y`，测试前后 `errors=0`。
- 已有 `/dev/null`、`/dev/zero` 对应节点存在，设备读取成功。
- 两次挂载共享同一节点和超级块。
- 测试 miscdevice 完成 8 轮注册与移除，嵌套路径、0660 权限、UID/GID 正确。
- 移除后节点及空父目录消失，已打开文件的权限清零，保留 FD 仍可完成读取。
- 测试 fixture 已卸载，临时挂载位于私有 mount namespace，测试目录已清理。
- Droidspaces v6.6.0 检查确认 PID、IPC、devtmpfs 可用。
- 本次测试的相关内核日志未发现 BUG、Oops 或 CFI failure。

这不包含完整容器启动、长期并发、内存压力或所有设备驱动的验证。
`ready` 表示初始枚举成功，后续健康检查还必须检查 `errors=0`，不能只
依据 `/proc/filesystems` 中出现 devtmpfs 判断成功。

## 构建和回归

使用固定 DDK 构建：

```sh
./scripts/build-ddk.sh TARGET=devtmpfs TEST_DEVTMPFS=1 OUT=/module/out/devtmpfs -j2
./tests/vm/run-devtmpfs.sh
```

QEMU 脚本需要已有的 `droidspaces-lkm-vm:local` 镜像，以及对应内核和
PID/IPC 载荷；准备过程见 [GKI 构建与验证](gki-builds.md)。可通过
`DS_DEV_KERNEL`、`DS_DEV_MODULE`、`DS_DEV_FIXTURE`、`DS_NS_MODULE`、
`DS_NS_PARAMS` 和 `DS_DEV_LOG_DIR` 选择已单独核验的设备内核组合。
测试 fixture 仅用于回归，不应包含在用户安装包中。

发布打包使用 `python3 scripts/package-plz110-optional.py`。打包器要求
`build/plz110-0.1.1.json` 记录的源码、两个二进制、设备 BTF、原始内核
Image 及测试证据哈希全部匹配。清理目标文件后的重复构建已复现上述
devtmpfs 二进制；完整测试证据位于构建工作区的 `out/devtmpfs/`。
ZIP 内附 `validation.json`，包含来源及验证摘要，测试 fixture 不打包。

开机脚本另经 11 项加载器测试（宿主 shell 与 ARM64 BusyBox），覆盖
错误设备、BTF 不匹配、损坏载荷、先后加载、部分失败及重复调用。
最终 ZIP 已在真机暂存解压，核对实际指纹、载荷校验及已加载状态；
该步骤没有替换已安装的开机模块，也未新增一次开机加载测试。

源码来源及适配说明见 [ORIGIN](../src/devtmpfs/ORIGIN)。

## User namespace 与发布状态

本实现不提供 user namespace，Droidspaces 的 Sandboxing 检查仍未通过。
目标内核中 UID/GID 映射、凭据中的 namespace 引用和多个权限路径存在
`CONFIG_USER_NS=n` 的编译期分支；仅移植 namespace 构造函数不能恢复
这些路径的隔离语义。后续工作仍限定现有内核与纯 LKM，不以修改检查
结果代替隔离实现。

v0.1.1 发布 PID / IPC + devtmpfs；user namespace 留待 v0.1.2 继续适配。
v0.1.1 不宣称支持 Sandboxing，也不保证 v0.1.2 在现有内核中的实现结果。
