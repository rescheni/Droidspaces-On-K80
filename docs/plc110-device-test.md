# PLC110 手动加载测试版

本测试版开放 PLC110 的真机手动加载，包含 `proc_exit_connector` 可选依赖修复。
真机测试尚待反馈；已在用户提供的 boot image 内核上完成 QEMU 功能验证。
首次真机测试仍可能引发内核崩溃或重启，请先保存工作。

只对应此完整内核版本：

```text
6.6.118-android15-8-ge58033dc8ea6-abogki498046332-4k
```

不能用于其他 6.6 构建。`plc110_test=1` 是显式开启真机测试的参数，默认关闭；
模块本身检查完整 release，加载脚本另外检查 Android 型号和内核 BTF 哈希。
型号检查来自 `ro.product.model` 或 `ro.product.vendor.model` 的精确 `PLC110` 值，
没有依据 boot image 猜测设备树 compatible。直接调用 ksud 不会执行脚本的型号/BTF 检查。

## 推荐测试步骤

将 ZIP 解压，保持 `.ko`、`SHA256SUMS` 和加载脚本在同一目录。
这不是 KernelSU 管理器安装包，不会配置开机自动加载。
在终端进入解压目录后执行：

```sh
su
sh load-plc110-test.sh selftest
sh load-plc110-test.sh activate
```

`selftest` 成功后会自动卸载；它会临时注册相关 proc/sysctl/mqueue 接口，
但不替换 namespace 系统调用。`activate` 会再次执行核心自测并激活完整 hook。
成功时脚本显示 `LOADED`，随后运行设备上已安装的 `droidspaces check`。

加载脚本默认使用 `/data/adb/ksud`。其他路径可用：

```sh
KSUD=/实际路径/ksud sh load-plc110-test.sh activate
```

脚本会检查型号、完整内核版本、ARM64、模块是否已加载、BTF 和 `.ko` 校验值。
失败时查看同目录的 `load.log`，该日志包含 ksud 输出和最近的内核日志。
再次运行脚本会覆盖日志，因此出错后先保存它。

## 直接 ksud 加载

如果已经自行确认设备、内核和文件对应，也可以在 root shell 执行：

```sh
/data/adb/ksud insmod ./droidspaces_ns-6.6.118-plc110-test.ko plc110_test=1 activate=1
```

参数 `plc110_test=1` 不能省略。省略后真机加载返回 `EOPNOTSUPP`，这是主动拒绝。

**激活后不能热卸载或替换 `.ko`，必须重启。** 不要使用强制卸载。
该测试包没有启动服务，重启后不会自行再次加载。

## 反馈信息

请提供 `load.log`、`droidspaces check` 输出，以及以下命令的结果：

```sh
uname -r
getprop ro.product.model
getenforce
cat /sys/module/droidspaces_ns/parameters/activate
cat /sys/module/droidspaces_ns/parameters/plc110_test
cat /sys/module/droidspaces_ns/parameters/pid_live
cat /sys/module/droidspaces_ns/parameters/ipc_live
```

若出现重启，可在重启后查看 `/sys/fs/pstore/` 是否留有记录。
不要求关闭 SELinux；如遇拒绝，请保留 AVC 日志用于定位。

## 验证与限制

最终测试版带 `plc110_test=1` 在同一原始 6.6.118 Image 上通过完整 QEMU 回归，
PID/IPC/task 计数全部归零，未出现内核崩溃或 WARN 回溯。
加载日志包含外部模块 taint、`module_layout` 缺少版本信息及模块 BTF 不匹配提示；
这些提示未阻止本次 QEMU 加载与功能测试，不能据此宣称真机已验证。
脚本的型号、版本、BTF、校验失败、重复加载、加载错误及成功路径有隔离测试。
真机加载条件已开放，但 QEMU 不能证明设备驱动、调度扩展、SELinux 域和长期负载安全。
仍不支持 32 位 IPC 和 `SHM_HUGETLB`，不提供 6.6 缺失的 pidfd namespace ioctl。
