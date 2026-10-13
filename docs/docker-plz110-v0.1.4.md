# PLZ110 v0.1.4 Docker 真机兼容性测试

这是 v0.1.4 的历史测试记录。引用泄漏、OverlayFS 告警及防火墙扩展的后续修复见
[v0.1.5 发布说明](releases-v0.1.5.md)；rootless 和资源限制仍未解决。

测试日期：2026-10-10。结论：普通 rootful Docker 在指定兼容配置下可以运行服务、
构建镜像和保存数据；默认网络、内层资源限制及标准 Rootless Docker 未通过。
Rootless 测试还触发原生 OverlayFS WARN，停止所有测试后发现一个尚未回收的
user namespace 引用，因此目前只能称为有条件的功能通过，不能称为稳定兼容。
本次仅测试和调整独立容器的配置，没有修改或重新加载内核模块。

## 环境

- PLZ110，`6.12.69-android16-6-g242e1a07f878-ab15865446-4k`，ARM64 / 4 KiB。
- Droidspaces LKM v0.1.4，namespace 载荷 SHA256：
  `890c0037bf234d70e164cb67bdfee55e0b2ffbf3181e2b8ce2d745c8cd8f9654`。
- SELinux Enforcing，保留设备原有 HMA / NoMount。
- 设备安装的 Droidspaces 6.6.0，构建时间 2026-09-21，采用其实际 `help` 接口操作。
- 独立容器 `lkm-docker-test`：Debian 12 ARM64、6 GiB ext4 镜像、NAT、
  `--allow-sandboxing`、外层 2 GiB 内存上限；未开启 Droidspaces privileged 模式。
- Docker Engine 29.9.0 / containerd 2.4.1 / runc 1.5.2 / Compose 5.6.0。
- RootlessKit 3.2.0 / fuse-overlayfs 1.10 / libfuse 3.14.0。

测试镜像先在开发机拉取并通过 `docker save/load` 传入：

| 镜像 | 拉取时的仓库 digest |
| --- | --- |
| busybox:1.37.0 | `sha256:bdf57e528e45e4433820e045b29b4597825a1c9e38353532d90a01445013f82e` |
| redis:7.4-alpine | `sha256:858f009f9709ce576febc734aa78b8f6d624b82571f9ddb6bda4377c833b3499` |

本次只导出并运行 linux/arm64 平台。包版本完整清单保存在本地证据目录。

## 实测结果

| 项目 | 结果 | 条件或限制 |
| --- | --- | --- |
| rootful daemon + VFS | 通过 | 关闭自动防火墙和默认 bridge，工作负载 `--network none` |
| 基础生命周期 | 通过 | PID 1、exec、停止/重启/删除、非零退出码、非 root UID/GID |
| rootful overlay2 | 通过 | ext4 底层；文件层写入、删除下层文件、硬/软链接、chown、commit |
| volume | 通过 | 内容与 UID/GID 保留，daemon 重启后仍正确 |
| rootful BuildKit | 通过 | 实际 RUN、chown、USER 和构建产物执行 |
| Docker 29 默认存储 | 通过 | `overlayfs` / `io.containerd.snapshotter.v1`；实际运行与 BuildKit 构建 |
| 默认 Docker 网络 | 失败 | nftables 未编译；legacy 后端也缺 `xt_addrtype` |
| 自定义 bridge + 手动 NAT | 通过 | 外层 DS 容器内配置；并非 Docker 默认防火墙兼容 |
| Compose Web + Redis | 通过 | 服务 DNS、HTTP、127.0.0.1 端口映射、Redis 持久卷、重启恢复 |
| DNS / 外网 | 通过 | 内层 bridge 与 host 网络均能请求 example.com |
| 在线镜像拉取 | 未完成 | Docker Hub 返回匿名拉取频率限制，不能算作内核失败 |
| 100 轮创建/退出 | 通过 | rootful overlay2、默认 seccomp、每轮文件写入并删除容器 |
| Docker 内层 memory 限额 | 失败 | runc 的 scope 缺 `memory.max`；下放控制器写入返回 EIO |
| Docker 内层 CPU 限额 | 失败 | 内层 v2 无 cpu 控制器，Docker 拒绝 NanoCPUs |
| Docker 内层 pids 限额 | 失败 | 内核未启用 CGROUP_PIDS，Docker 警告后丢弃限制 |
| 标准 Rootless + FUSE | 失败 | 用户服务建立后，runc 加载 seccomp filter 返回 EACCES |
| Rootless + no-new-privileges | 失败 | 随后在 capability bounding set 设置返回 EPERM |
| Rootless 诊断配置 | 部分通过 | `--cap-add ALL --security-opt no-new-privileges=true`，见下文 |
| Rootless BuildKit | 失败 | RUN 阶段仍在加载 seccomp filter 时返回 EACCES |

## 普通 Docker 的可用配置

以下是本次 overlay2 daemon 的有效配置，服务手动启动，socket 与数据目录独立：

```json
{
  "storage-driver": "overlay2",
  "features": {"containerd-snapshotter": false},
  "data-root": "/var/lib/docker-overlay",
  "exec-root": "/run/docker-overlay",
  "pidfile": "/run/docker-overlay.pid",
  "hosts": ["unix:///run/docker-overlay.sock"],
  "iptables": false,
  "ip6tables": false,
  "bridge": "none",
  "ip-forward": false,
  "ip-masq": false
}
```

另一组测试去掉 `storage-driver` 与 `features`，使用独立数据目录，让 Docker 29
选择默认 containerd snapshotter，同样通过实际运行和构建。

默认网络失败的直接报错是：

```text
failed to append jump rules to nat-PREROUTING
Extension addrtype revision 0 not supported, missing kernel module?
iptables: No chain/target/match by that name
```

实际内核配置：`CONFIG_NF_TABLES`、`CONFIG_NETFILTER_XT_MATCH_ADDRTYPE`、
`CONFIG_BRIDGE_NETFILTER` 均未启用。切换 legacy 只能绕过 nftables 缺失，
不能补上 addrtype。

测试中在外层 Droidspaces 的独立网络 namespace 内创建 `ds-connectivity` bridge，
记录其子网为 `172.17.0.0/16`，开启该 namespace 的 IPv4 forwarding，再为这个子网
添加到 `eth0` 的 MASQUERADE 规则。Compose 使用该 external network，发布端口绑定
外层容器的 `127.0.0.1:18080`。HTTP、Redis、服务间 DNS 和外网均通过。
这套配置没有 Docker 自动防火墙提供的全部隔离语义，不应当作默认网络功能完全等价。
子网必须取实际 `docker network inspect` 结果，不能照搬到其他环境。

## Rootless 的边界

使用普通用户 `dockertest`（UID 1000），subuid/subgid 为 `100000:65536`。
先运行系统级 User= 服务，发现缺用户 cgroup；随后建立 systemd user manager，
通过用户级服务启动 RootlessKit / dockerd，再测试标准路径。

Rootless daemon 能启动，`docker info` 确认 `rootless` 与 `fuse-overlayfs`，
镜像可以解包。但默认 `docker run` 在加载 seccomp filter 时失败。
仅加 no-new-privileges 后越过此处，又在设置 capability bounding set 时失败。

诊断运行添加：

```text
--cap-add ALL --security-opt no-new-privileges=true
```

之后实际通过 FUSE 层读写、exec、重启、非 root UID/GID、退出码、volume、chown、
删除下层文件、硬/软链接与 commit。进程仍显示 `NoNewPrivs: 1`、`Seccomp: 2`，
两层 seccomp filter；通过 `--network host` 使用 RootlessKit/slirp4netns 出网也通过。
这里的 host 是 RootlessKit 网络 namespace，不是 Android 宿主网络。

这个结果说明 FUSE 层能承载实际 Docker rootfs，但不证明标准 Rootless 安全配置
已兼容。扩大 capabilities 是诊断条件，不是发布修复。Rootless BuildKit 的 RUN
仍因 seccomp 返回 EACCES 而失败。另一次 `--privileged` 诊断卡在 sysfs 挂载 EPERM。
下一步需分别核对 seccomp 与 capability/prctl 的原生 USER_NS=n 路径，不能通过
统一放行权限检查解决。

## 资源限制

宿主有 memory 控制器，Droidspaces 外层 2 GiB 限额确实存在；但内层 Docker
scope 没有 `memory.max`。在外层容器内尝试启用 root/system.slice 的 memory
subtree control，写入返回 EIO，设置 systemd MemoryAccounting 也没有解决。
此处只定位到可重复的失败点，尚未确定是运行时配置、内核原生路径还是 LKM 适配问题。

CPU 控制器由 Android 的 v1 层级持有，内层 systemd 使用的 v2 未获得 cpu。
内核虽有 CFS_BANDWIDTH，也不能据此声称 Docker 的 `--cpus` 已可用。
CGROUP_PIDS 则确实未编译，因此 `--pids-limit` 没有生效。

## 内核告警与回收问题

测试中记录到 17 次 `ovl_fill_super` WARN，触发进程为 UID 1000 的 runc，
栈经过 `ovl_get_tree` 与 `__arm64_sys_fsconfig`。固定 DDK 的
`fs/overlayfs/super.c:1302` 是：

```c
if (WARN_ON(fc->user_ns != current_user_ns()))
    goto out_err;
```

宿主编译为 USER_NS=n，原生 OverlayFS 内的 `current_user_ns()` 仍按初始 namespace
处理；LKM 恢复的 fs_context 则带有真实子 namespace。源码与运行栈吻合，表明
runc 在非初始 namespace 中触发的原生 OverlayFS 路径尚不兼容。没有通过屏蔽 WARN
或忽略挂载错误将其算作通过。

测试前 `userns_live=0`。停止 Docker、RootlessKit、用户 manager 和整个 Droidspaces
容器后，计数仍为 1；未发现存活测试进程或对应外层挂载，已检查可见任务的 user
namespace。该结果属于引用回收未通过，仍需区分剩余持有者与引用泄漏，尚未定位来源。
不能直接断定是 FUSE 连接泄漏，也不能凭功能测试通过忽略它。

手机启动 ID 始终未变，保持 Enforcing，devtmpfs errors=0。未观察到 Oops/panic，
但上面的 WARN 和计数异常均作为本次测试缺陷记录。

## 证据与保留环境

本地 `out/docker-compat/` 保存测试脚本、镜像准备过程、精确配置与日志。
重点日志位于 `logs/`：

- `smoke-vfs.log`、`storage-overlay.log`、`default-store.log`
- `overlay-diagnostic.log`、`network-compose.log`、`restart-persistence.log`
- `resources-stress.log`、`memory-delegation.log`
- `rootless-user-smoke.log`、`rootless-nnp.log`
- `rootless-conditional-smoke.log`、`rootless-conditional-storage.log`
- `rootless-nnp-caps-diagnostic.log`、`rootless-slirp-network.log`

测试脚本直接使用设备实际支持的 `droidspaces -n NAME run /bin/sh -c ...`。
该设备版本不接受相邻仓库新版文档中的 `run --` 分隔符。

独立镜像保留在手机 `/data/local/tmp/droidspaces-docker-compat/rootfs.img`，
配置为同目录的 `container.config`，便于后续复测。临时 Docker 服务不配置开机自启。
原始设备日志只保留本地，不随 Release 上传。
结构化结果及脚本/日志 SHA256 位于本地 `out/docker-compat/results.json`。

测试结束后已停止专用容器和全部测试 daemon，根镜像已卸载；保留测试文件便于复测。
v0.1.4 的开机启用状态未改变，没有为清理测试而重启手机。
