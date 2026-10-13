# Droidspaces LKM v0.1.2 — PLZ110 Sandboxing

> 此 Release 已因首次解锁时的 FUSE 内核崩溃撤下。请使用 [v0.1.3](releases-v0.1.3.md)。
> 下文仅保留原始发布记录，不代表完整开机流程通过验证。

此版本在现有内核上通过 LKM 增加 user namespace，供 Droidspaces 的
`--allow-sandboxing` 使用，并保留 PID / IPC namespace 与 devtmpfs。
无需刷写内核或在设备上提供内核源码。

## 适用目标与安装

仅支持 ARM64、4 KiB 页面、PLZ110 / `qcom,canoe`：

```text
6.12.69-android16-6-g242e1a07f878-ab15865446-4k
```

BTF SHA256：`1870e245673b3eaae0d1fdafb0fd7027888b638018ed9561943a51dc2539d6af`。
设备、完整版本和 BTF 指纹任一不符即停止安装或加载。

在 KernelSU 中安装 `Droidspaces-LKM-KernelSU-v0.1.2-PLZ110.zip` 后重启。
作者为 Shikochin；沿用模块 ID `droidspaces_lkm`，可覆盖旧版本。
激活后的 LKM 固定在当前内核中，安装更新不会热替换，禁用或卸载也需重启生效。
本设备版本不替代此前独立 GKI v0.2.0，也不适用于 PLC110 的 6.6 内核。

开机简介只有在 PID / IPC 接口、devtmpfs 健康状态以及真实 Sandboxing 检查均通过时
才显示 `[已加载]`。功能检查降到 UID/GID 1000，创建 user namespace，拒绝宿主 root
映射、建立自身映射、拒绝返回宿主 namespace，并创建私有 UTS namespace。
失败显示 `[部分加载]` 或具体加载错误；运行旧载荷则提示 `[需重启]`。
状态记录本次检查结果，不是持续监测。日志位于模块目录的 `load.log`。

Droidspaces 容器需启用 `--allow-sandboxing`；安装包不改动现有容器配置。

## 验证

- 两颗内核（固定 GKI 和 PLZ110 原始 boot image）通过 QEMU 回归，预加载实际
  HMA UID Fake 载荷。真机保持 HMA UID Fake 0.4.0+1c71b6c 与 NoMount 运行。
- PLZ110 全局 SELinux Enforcing：真实 UID 1000 的 22 项策略测试、32 轮映射
  文件并发写入与任务退出竞态，以及 namespace 引用回收均通过。
- Bubblewrap 0.8.0 的基本、`--unshare-all`、禁止嵌套 user namespace 三种配置，
  直接运行和在实际 Droidspaces 6.6.0 临时容器内运行均通过。
- 宿主 root 文件访问、未映射身份、伪造 socket 凭据及越权 namespace 切换被拒绝。
- PID / IPC / POSIX mqueue 回归与 devtmpfs 节点、热插拔、模式、打开 FD 生命周期测试通过。
- KernelSU 加载逻辑的 14 项测试通过；同一真机上运行实际载荷校验及功能检查，重复调用通过。
  该检查在独立暂存目录运行；未另外重启验证新 ZIP 的完整开机时序。

修复 HMA 对 setuid 能力检查入口的兼容性：保留 HMA 包装逻辑，在严格校验后接入
namespace 感知的原生函数恢复点。未知入口修改仍拒绝加载。
测试工具也修复了 Android loop 设备 minor 间隔的处理。

完整实现、来源和测试范围见 [user namespace 状态](userns-status.md)。
ZIP 内 `validation.json` 保存源码及验证输入的 SHA256，`SOURCE.txt` 指向对应标签。

## 产物与边界

- `droidspaces_ns-6.12.69-canoe.ko`：PID / IPC / user namespace 合并载荷。
- `droidspaces_devtmpfs-6.12.69-plz110.ko`：与 v0.1.1 相同的 devtmpfs 载荷。
- `Droidspaces-LKM-KernelSU-v0.1.2-PLZ110.zip`：目标校验、两份 `.ko`、功能检查器及开机脚本。
- `SHA256SUMS`：上述文件的校验值。

独立 `.ko` 仅供已核对目标的手动加载，namespace 参数为
`activate=1 userns_device_test=1`。推荐安装具有完整指纹检查的 ZIP。

本版本为指定设备的预发布版本。已验证工作负载不构成完整 Linux USER_NS 等价性
或所有应用兼容性保证；idmapped mount、其他内联权限路径、长期高负载及所有 Android
SELinux 域尚未覆盖。没有测试完整 Docker / Podman / Flatpak / 浏览器应用。
