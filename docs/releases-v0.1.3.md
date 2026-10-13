# Droidspaces LKM v0.1.3 — PLZ110 FUSE 开机修复

修复 v0.1.2 在首次输入密码、Android 挂载模拟存储时发生的内核崩溃。
v0.1.2 Release 及附件已撤下，Git 标签保留用于追溯。请使用此版本。

## 原因与修复

原内核没有启用 USER_NS，编译器省略了 FUSE 初始化中不再使用的 user namespace
参数。v0.1.2 的移植函数读取了这个无效参数，导致 `ds_fuse_conn_init` 空指针崩溃。
此前解锁后手动加载时 FUSE 已经挂载，因此没有触发这条路径。

v0.1.3 保留原生 FUSE 连接的初始 namespace 所有权，不再读取被编译器省略的参数。
沙箱内新建 FUSE 挂载暂时返回 `EOPNOTSUPP`，避免未经完整移植的身份转换路径
错误地使用宿主权限。Android 宿主 FUSE 与已有 Sandboxing 工作负载正常使用。
这不是通过延时加载绕开问题。

## 验证

- GKI 和 PLZ110 原始内核的 QEMU 均先复现旧版 FUSE 崩溃；修正版通过加载前后
  的 FUSE 挂载、卸载，以及拒绝沙箱创建 FUSE 的回归。
- PLZ110 真机完成开机自动加载、首次密码解锁和文件管理器使用；Android 的
  `/mnt/user/0/emulated`、`/storage/emulated` 等 FUSE 挂载正常。
- 开机后再次通过真实 UID 1000 的 22 项策略测试、32 轮映射 FD 与退出竞态，
  以及直接运行和在 Droidspaces 6.6.0 容器内运行的三种 Bubblewrap 配置。
- PID / IPC / POSIX mqueue、devtmpfs 八轮热插拔及打开 FD 生命周期回归通过。
- 全局 SELinux 保持 Enforcing，HMA UID Fake 0.4.0+1c71b6c 和 NoMount 保持运行。
- Namespace 活跃计数回到零，devtmpfs 错误计数为零。KernelSU 加载器 14 项测试通过。

真机开机验证使用相同 `.ko` 与加载逻辑，测试时额外设置了崩溃后下次启动禁用的
保护标记；通过验证后已恢复正常开机脚本。ZIP 的版本信息与验证清单随后更新为 v0.1.3。
详见 [故障与修复记录](v0.1.2-boot-failure.md) 和 [实现范围](userns-status.md)。

## 安装与产物

仅支持 PLZ110 / `qcom,canoe`，ARM64、4 KiB 页面，完整内核版本：

```text
6.12.69-android16-6-g242e1a07f878-ab15865446-4k
```

BTF SHA256：`1870e245673b3eaae0d1fdafb0fd7027888b638018ed9561943a51dc2539d6af`。
安装和启动都会核对设备、完整内核版本、BTF 指纹和载荷哈希。
此版本不适用于 PLC110 的 6.6 内核，也不替代独立 GKI v0.2.0。

在 KernelSU 中安装 ZIP 后重启。模块 ID 仍为 `droidspaces_lkm`，作者 Shikochin。
已激活的 `.ko` 无法热替换，更新、禁用或卸载后均需重启生效。
升级前已禁用模块的用户，在安装 v0.1.3 后确认模块已启用。

- `droidspaces_ns-6.12.69-canoe.ko`：修正后的 PID / IPC / user namespace 合并载荷。
- `droidspaces_devtmpfs-6.12.69-plz110.ko`：与 v0.1.1 相同的 devtmpfs 载荷。
- `Droidspaces-LKM-KernelSU-v0.1.3-PLZ110.zip`：上述载荷、真实功能检查器与开机脚本。
- `SHA256SUMS`：发布文件校验值。

简介只有在 PID / IPC 接口、devtmpfs 状态及 Sandboxing 身份映射和权限检查均通过时
才显示 `[已加载]`，否则显示具体失败或部分加载原因。状态反映检查时的结果，不是
持续监控；日志位于模块目录的 `load.log`。Droidspaces 容器需开启 `--allow-sandboxing`，
安装包不会修改现有容器配置。

独立 namespace `.ko` 的参数仍为 `activate=1 userns_device_test=1`；推荐使用完整 ZIP。

本版本仍为指定设备预发布版。未声称完整 Linux USER_NS 等价性，沙箱新建 FUSE、
idmapped mount、其他内联权限路径、长期高负载及所有 Android SELinux 域未完整支持或验证。
完整 Docker / Podman / Flatpak / 浏览器应用不在已验证范围内。
