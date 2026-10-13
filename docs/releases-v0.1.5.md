# Droidspaces LKM v0.1.5 — PLZ110 Docker 兼容性修复

在现有 PLZ110 内核上通过纯 LKM 修复 namespace 引用泄漏、避免子 user namespace
中的原生 OverlayFS 告警，并补充 `xt_addrtype` 与 nftables。保留 PID / IPC、
devtmpfs、Sandboxing 和普通沙箱 FUSE 支持，无需刷写内核。

## 修复与新增功能

- **引用回收**：定位到指定厂商模块 `oplus_sys_stability_helper` 的
  `send_signal_catcher` 获取 PID 引用后未释放。新的 `droidspaces_compat.ko`
  校验完整回调指令指纹和调用目标，只替换遗漏释放的调用点；保留信号与日志行为。
  指纹不符会拒绝加载。修复阻止新泄漏，旧引用需重启清除。
- **OverlayFS**：初始 user namespace 中的原生 OverlayFS 保持可用；子 user
  namespace 中的 mount / fsopen 在进入不兼容原生路径前返回 `EOPNOTSUPP`，
  避免内核 WARN。子 namespace 可使用已支持的 fuse-overlayfs；本版未实现原生
  child-owned OverlayFS。
- **netfilter**：新的 `droidspaces_netfilter.ko` 提供 `xt_addrtype`、nftables
  核心、集合、conntrack、NAT / masquerade / redirect、FIB、reject、logging、
  limit 和 xtables 兼容层，支持 IPv4、IPv6、INET。每个网络 namespace 的 nftables
  状态使用独立扩展存储，不改变原生 `struct net` 布局。
- **开机加载**：KernelSU 依次加载 namespace、devtmpfs、compat、netfilter 四个
  `.ko`，核对文件哈希并执行 Sandboxing 健康检查。模块自身不创建防火墙规则。

实现来源和重现方法见 [compat 来源记录](../src/compat/ORIGIN)、
[netfilter 来源记录](../src/netfilter/ORIGIN) 与仓库测试脚本。

## 验证范围

发布前完成指定 GKI 和 PLZ110 原始内核的 QEMU 回归，并在 PLZ110 的 SELinux
Enforcing 环境验证开机加载及实际 Droidspaces 容器。详细结果、载荷、源码和本地
证据哈希记录于 `build/plz110-0.1.5.json`，安装包内包含同一份 `validation.json`。

- 完整 user namespace / FUSE 回归：22 项策略、32 轮退出竞态、保留 FD、直接及
  Droidspaces 容器内的 Bubblewrap。
- OverlayFS：rootful copy-up、whiteout、重新挂载，以及 32 轮子 namespace 的
  mount / fsopen 拒绝与引用回收。
- 厂商回调离线测试：原始指令重现泄漏、拒绝指纹不匹配、64 次修复后调用与回收。
  厂商 ELF 和 HMA 均不随仓库或发布包分发。
- Docker 29.9.0：iptables-legacy、iptables-nft、原生 nftables 三种后端，覆盖
  bridge、容器 DNS / HTTP、端口发布、跨 namespace 访问、MASQUERADE 和跨桥隔离。
- nftables：IPv6 集合丢弃 / 放行、失败批次回滚及 IPv4 / IPv6 INET DNAT。
- 真机失败路径回归：16 次预期启动失败与 8 次成功运行后 PID / user namespace
  计数回到基线；完整停止测试容器后归零。

设备原始日志不公开。厂商已有的启动与休眠告警不作为本项目修复范围；本版不声称
整个设备无告警。开机验证采用额外写入 disable 的一次性保护脚本，验证通过后恢复
正式脚本；发布包与测试使用相同 `.ko` 和加载逻辑。

## 安装与产物

仅支持 PLZ110 / `qcom,canoe`，ARM64、4 KiB 页面及完整内核版本：

```text
6.12.69-android16-6-g242e1a07f878-ab15865446-4k
```

BTF SHA256：`1870e245673b3eaae0d1fdafb0fd7027888b638018ed9561943a51dc2539d6af`。
额外要求厂商信号回调与记录的指令指纹匹配。不适用于 PLC110 6.6，也不替代 GKI v0.2.0。

在 KernelSU 中安装 `Droidspaces-LKM-KernelSU-v0.1.5-PLZ110.zip` 后重启。
模块 ID 为 `droidspaces_lkm`，作者为 **Shikochin**。升级会替换同 ID 的旧版本；
此前禁用了模块的用户应确认已启用。已激活的 namespace、devtmpfs 和 compat
必须重启才能替换或移除。

Release 另含四个独立载荷和 `SHA256SUMS`：

- `droidspaces_ns-6.12.69-canoe.ko`
- `droidspaces_devtmpfs-6.12.69-plz110.ko`
- `droidspaces_compat-6.12.69-plz110.ko`
- `droidspaces_netfilter-6.12.69-plz110.ko`

推荐使用完整 ZIP。简介在四个模块加载及基础健康检查通过后显示 `[已加载]`；
扩展加载失败显示 `[部分加载]`，具体原因记录在模块目录的 `load.log`。
状态是加载时检查结果，不是持续监控，也不会在开机时启动 Docker 测试容器。

## 已知限制

- rootless Docker 的 seccomp / capability 问题本版未处理。失败路径测试不代表
  rootless Docker 已可正常使用，也不建议用扩大 capability 作为日常配置。
- 未补充 cgroup 控制器；CPU、cpuset、swap 和部分 I/O 限额仍可能不可用。
- 未实现 `br_netfilter`、nftables bridge / netdev families；已测 Docker 网络使用
  默认 userland proxy，关闭它的路径仍不支持。
- [Docker 原生 nftables 后端](https://docs.docker.com/engine/network/firewall-nftables/)仍属于 Docker 的实验功能；本次未验证 Swarm，测试
  显式启用容器网络 namespace 内的 IPv4 forwarding。
- 未证明完整 CONFIG_USER_NS 等价性、所有容器应用或长期高负载稳定性。

本版本继续作为指定设备的预发布版本发布。
