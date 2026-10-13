# 沙箱内 FUSE 挂载适配

v0.1.4 为 PLZ110 增加沙箱内普通 FUSE 挂载。模块已通过 GKI 和 PLZ110 原始内核的
QEMU 回归，以及真机 Enforcing 下的完整开机、首次解锁和 FUSE 实际读写测试。
用户确认解锁进入桌面、使用文件管理器正常，没有重启。

目标保持为现有的 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` 内核，
通过纯 LKM 恢复相关路径，不替换内核镜像。

## 实现范围

- 沙箱内创建普通 `fuse` 挂载，并保留连接所属 user namespace 的引用。
- 请求头中的 UID/GID、文件属主、SETATTR、STATX 和创建时的补充组转换。
- `allow_other` 按连接的 user namespace 判断；父级和兄弟 namespace 不因此获准访问。
  原生 `allow_sys_admin_access` 管理员开关保持原有语义。
- POSIX ACL 的 daemon、VFS 和调用者三处身份转换；保留原有 LSM 检查。
- `/dev/fuse` 必须在挂载所属 user namespace 中打开，不能使用父级预先打开的 FD。
- 保留 v0.1.3 的宿主初始化约定，避免再次读取原生编译器省略的 namespace 参数。

沙箱内不启用 Android FUSE-BPF backing、`no_daemon`、块设备 FUSE、passthrough
或 idmapped FUSE mount。BPF backing 挂载选项被拒绝，LOOKUP 的额外 backing
响应被拒绝，passthrough 和 idmapped mount 标志不参与协商。宿主连接保留原有扩展。

原生内核把多个身份转换和权限判断内联进调用者，因此不能只替换公共函数。
本次同时移植了对应的权限、属性和 ACL 入口。另外，原生静态函数
`fuse_do_getattr` 已删除首个参数并移动其余参数寄存器；模块使用独立实现，
不依据源码声明直接调用这个本地符号。

## 用户态条件

沙箱内需要能打开 `/dev/fuse`，并有权限在自己的 mount namespace 中调用
`mount(2)`。例如 Bubblewrap 测试显式传入设备，并在沙箱内保留 `CAP_SYS_ADMIN`：

```sh
bwrap --unshare-all --uid 0 --gid 0 --cap-add CAP_SYS_ADMIN \
  --ro-bind /bin /bin --proc /proc --dev /dev \
  --dev-bind /dev/fuse /dev/fuse --tmpfs /tmp --chdir / \
  -- /bin/sh
```

此例只示意 namespace、设备和能力设置；实际 FUSE 程序还需要其可执行文件、
依赖和挂载目录。已有沙箱的 seccomp、设备访问限制和 SELinux 策略仍然生效。

## 已完成验证

两颗 VM 内核均预加载设备使用的 HMA 模块，回归结果均为
`DS_USERNS_LIVE_VM_RESULT=0`，无 BUG、WARNING、Oops 或 panic。

| 验证 | 结果 |
| --- | --- |
| 模块加载前后的宿主 FUSE 挂载及释放 | 通过 |
| UID 1000 无宿主特权创建 user namespace 并挂载 FUSE | 通过 |
| 实际 daemon 请求头、文件创建、读写、截断、chown、卸载 | 通过 |
| 多 UID/GID 映射、命名用户/组 ACL 往返和访问判断 | 通过 |
| 父级 `/dev/fuse` FD、父级/兄弟 namespace 访问拒绝 | 通过 |
| 超出映射的挂载属主和 chown、BPF 扩展响应拒绝 | 通过 |
| 未协商的 passthrough/idmap 回复标志过滤 | 通过 |
| 创建者退出后保留设备及挂载 FD，关闭后 RCU 回收 | 每颗内核 8 轮通过 |
| Bubblewrap 内实际 FUSE 挂载和读写 | 通过 |
| Droidspaces 容器内的 Bubblewrap 实际 FUSE 挂载和读写 | 通过 |
| 既有 user namespace 策略、退出竞态和容器回归 | 通过 |
| 测试完成后 user namespace 活跃计数 | 0 |

ABI 对比包含 824 个一致的聚合类型，无缺失导入符号或函数原型差异。
仅保留原有三个设备 KABI 别名差异，结构大小和使用字段偏移不变。
ABI 对比不替代实际机器码调用约定和运行测试。

本地验证输出为 `out/userns-fuse-mount/`：

- `device/droidspaces_ns.ko`：PLZ110 发布载荷，SHA-256
  `890c0037bf234d70e164cb67bdfee55e0b2ffbf3181e2b8ce2d745c8cd8f9654`。
- `gki-vm/console.log`、`plz110-vm/console.log`：完整离线回归。
- `device-abi.json`：ABI 对比。
- `device-preflight.log`：真机指纹、Enforcing 和独立测试根目录预检。
- `stage/`：手动测试文件及已执行的一次开机测试脚本。

真机使用相同载荷通过上述 FUSE 功能回归、22 项策略、32 轮退出竞态、
直接 Bubblewrap 和 Droidspaces 容器内 Bubblewrap 测试。PID/IPC/mqueue 与
八轮 devtmpfs 热插拔回归通过，异步回收后 user namespace 计数为零、devtmpfs
错误计数为零。正式加载器连续两次检查通过。

开机测试在加载前写入一次性 `disable` 保护标记。首次解锁后确认 Android 的
`/mnt/user/0/emulated` 和 `/storage/emulated` FUSE 挂载正常，启动 ID 未改变。
真机日志有厂商驱动的启动告警，以及 `oplus_secure_guard_new` 提权上报路径的
FORTIFY 告警；未发现 FUSE/Droidspaces 的 Oops 或 panic。原始设备日志仅保留本地，
发布清单记录校验值，不公开用户日志。

安装包保持相同二进制和加载逻辑，更新版本与验证元数据，并恢复正常开机脚本。
完整证据索引见 `build/plz110-0.1.4.json`；长期负载及其他 Android SELinux 域
不在本次验证范围内。
