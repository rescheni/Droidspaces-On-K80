# Droidspaces LKM v0.1.4 — PLZ110 沙箱 FUSE

在现有 PLZ110 内核上，通过纯 LKM 支持沙箱内新建普通 FUSE 挂载，无需刷写内核。
保留 v0.1.3 的首次解锁修复，以及 PID / IPC、devtmpfs 和 Sandboxing 支持。

## 变化

- 恢复 FUSE 连接所属 user namespace、请求 UID/GID、文件属主、属性修改和创建时的身份转换。
- 恢复 FUSE 权限与 POSIX ACL 映射，保留 LSM、设备 FD 所有者和 namespace 访问限制。
- 支持 Bubblewrap 内真实 FUSE daemon 的创建、读写、截断、chown 和卸载，包含在 Droidspaces 容器内运行的场景。
- 保留 Android 宿主 FUSE 的原生初始化约定，避免再次读取编译器省略的参数。

沙箱内不支持 Android FUSE-BPF backing、`no_daemon`、`fuseblk`、passthrough
或 idmapped FUSE mount；宿主连接保留原有扩展。沙箱仍需 `/dev/fuse` 访问与自身
mount namespace 内的挂载权限。配置示例及实现边界见 [FUSE 适配记录](userns-fuse.md)。

## 验证

- GKI 与 PLZ110 原始内核的 QEMU 完整回归通过，均预加载 HMA。
- PLZ110 真机开机自动加载、首次密码解锁、文件管理器正常，Android 模拟存储 FUSE 挂载正常，无重启。
- 真机 SELinux 保持 Enforcing；HMA UID Fake 0.4.0+1c71b6c 和 NoMount 保持运行。
- 无宿主特权的 UID 1000 创建 user namespace 后，FUSE 实际读写、多 UID/GID 映射、命名用户/组 ACL 往返及权限判断通过。
- 父级预开设备 FD、父级/兄弟 namespace 访问、未映射属主和扩展响应拒绝测试通过；设备及挂载 FD 保留/释放通过八轮回归。
- 22 项策略、32 轮映射 FD 与退出竞态、直接及 Droidspaces 容器内的 Bubblewrap 回归通过。
- PID / IPC / POSIX mqueue、devtmpfs 八轮热插拔与打开 FD 生命周期回归通过。
- 异步清理后 user namespace 活跃计数为零，devtmpfs 错误为零；正式加载器连续两次检查及 14 项加载器测试通过。

两颗 VM 无 BUG、WARNING、Oops 或 panic。真机保留厂商驱动启动告警及
`oplus_secure_guard_new` 提权上报路径的 FORTIFY 告警，未发现 FUSE/Droidspaces
的 Oops 或 panic。设备原始日志不随发布包公开，验证清单仅记录哈希。

开机测试使用与发布包相同的 `.ko` 和加载逻辑，额外写入下一次启动禁用的保护标记；
确认正常后恢复正式开机脚本。源码、二进制、内核与证据哈希见 `validation.json`
及仓库 `build/plz110-0.1.4.json`。

## 安装与产物

仅支持 PLZ110 / `qcom,canoe`，ARM64、4 KiB 页面及完整内核版本：

```text
6.12.69-android16-6-g242e1a07f878-ab15865446-4k
```

BTF SHA256：`1870e245673b3eaae0d1fdafb0fd7027888b638018ed9561943a51dc2539d6af`。
安装与开机均核对设备、完整内核版本、BTF 指纹及载荷哈希。
不适用于 PLC110 6.6，也不替代此前的独立 GKI v0.2.0。

在 KernelSU 中安装 ZIP 并重启。模块 ID 为 `droidspaces_lkm`，作者为 Shikochin。
已激活 `.ko` 固定在当前内核中，更新、禁用或卸载需重启生效；升级前禁用了模块的用户，
安装后应确认模块已启用。Droidspaces 容器需启用 `--allow-sandboxing`，安装包不修改现有容器配置。

- `droidspaces_ns-6.12.69-canoe.ko`：PID / IPC / user namespace 与沙箱 FUSE 合并载荷。
- `droidspaces_devtmpfs-6.12.69-plz110.ko`：沿用 v0.1.1 的 devtmpfs 载荷。
- `Droidspaces-LKM-KernelSU-v0.1.4-PLZ110.zip`：上述载荷、功能检查器和开机脚本。
- `SHA256SUMS`：发布文件校验值。

简介在 PID / IPC 接口、devtmpfs 状态及 Sandboxing 身份映射与权限检查通过时显示
`[已加载]`；失败时显示具体原因。开机检查不运行完整 FUSE 读写套件，也不是持续监控。
日志位于模块目录的 `load.log`。独立 namespace `.ko` 参数仍为
`activate=1 userns_device_test=1`，推荐使用完整 ZIP。

本版本仍为指定设备预发布版，不声称完整 CONFIG_USER_NS 等价性。其他内联权限路径、
长期高负载、所有 Android SELinux 域，以及完整 Docker / Podman / Flatpak / 浏览器
应用均不在本次验证范围内。
