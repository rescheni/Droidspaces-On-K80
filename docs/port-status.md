# PID / IPC namespace 移植状态

## 2026-10-10 真机结果

目标 PLZ110（`qcom,canoe`），Android 17，ARM64 / 4 KiB：
`6.12.69-android16-6-g242e1a07f878-ab15865446-4k`。

namespace 模块已通过 `/data/adb/ksud insmod ... activate=1` 在手机加载并激活。
`/proc/self/ns` 新增 `pid`、`pid_for_children`、`ipc`；Droidspaces v6.6.0 检查
由缺少两项必需能力变为 `All required features found!`。SELinux 保持 Enforcing，
boot ID 不变，`kptr_restrict` 保持 2。没有改 boot 分区，没有添加开机加载服务。

基线提交是 `45aded6`；本轮补齐 IPC 后端和其他 namespace 调用路径。
已加载的原始模块（ksud 重定位前）4,923,672 字节，SHA-256：
`33ed20ce38f092d98f4379701d45127522370d13f3d59ba6458916a0b6f592f4`。
复制保存在 `out/release/droidspaces_ns-6.12.69-canoe.ko`。

## 验证顺序与证据

1. DDK 内核 `6.12.76-4k-gae4e2f4f997e-dirty`，QEMU virt、2 vCPU、2 GiB。
2. 从手机活动槽 `boot_b` 只读提取 Image，在相同 QEMU 环境测试实际手机内核。
   Image SHA-256：`695cd037054aa9c4bb521694cc8eba261eff3aea51de21ec29a1101ea2bf2964`。
3. 真机 `activate=0`：入口预检、核心对象创建/共享/销毁、计数归零、成功卸载。
4. 真机 `activate=1`：Droidspaces check、独立用户态功能和生命周期测试。

QEMU 原内核未启用 PID_NS、IPC_NS、SYSVIPC、POSIX_MQUEUE，保留 CFI；不重编内核。
QEMU 不装载 SELinux 策略，真机测试使用现有 KernelSU root 域和 enforcing 策略。
没有临时设置 permissive 或修改 SELinux 策略。

真机测试输出：

```text
DS_NAMESPACES_PASS: PID 1, nested PID, procfs, orphan reaping, IPC identity, setns, clone, permissions
DS_SYSVIPC_PASS: messages, semaphores, shared memory, isolation, SEM_UNDO, clone sharing, permissions, blocked removal, procfs, forced cleanup
DS_MQUEUE_PASS: mount, priority, timeout, notification, namespace isolation, sysctl, unlink lifetime
DS_PID_POLICY_PASS: memfd executable policy, seal, enforcement, parent floor, host isolation
DS_PID_LIFETIMES_PASS: perf inheritance and retained FD, BPF iterator retained FD, cgroup pidlist, FUSE mount teardown
```

namespace 测试还执行了 pidfd 的 PID/IPC namespace ioctl，以及容器 RESTART、
RESTART2、HALT、POWER_OFF；验证 namespace init 的退出信号，宿主保持运行。
测试后稍等，真机 `pid_live=0`、`ipc_live=0`。`ipc_tasks` 为活跃 Android 线程维护
SEM_UNDO 状态，采样为 211，不等同于泄漏。QEMU 退出测试进程并回收缓存后，三个
计数均为 0。真机检查的后续日志未见相关 BUG / WARNING / Oops / CFI failure。

证据文件（在本机 out 中，不提交设备资料）：

| 路径 | 内容 |
| --- | --- |
| `out/device/droidspaces-before.txt` | 激活前缺 PID / IPC |
| `out/device/core-preflight.txt` | 真机预检、自测、卸载 |
| `out/device/activation.txt` | 激活及 Droidspaces 检查全部必需项通过 |
| `out/device/functional-tests.txt` | 真机 namespace / SysV / mqueue / memfd |
| `out/device/lifetime-tests.txt` | 真机 perf / BPF / cgroup / FUSE |
| `out/device/after-tests.txt` | 延迟回收后的计数和日志检查；末尾 list 是不支持的 CLI 命令 |
| `out/vm/device-preflight-run.log` | 实际手机内核的完整离线测试 |
| `out/abi/device-report.json` | 结构布局和依赖符号对比 |

## 接入点

| 宿主入口 | 模块行为 |
| --- | --- |
| alloc_pid、put_pid、delayed_put_pid | 多层 PID 分配及 namespace 引用 |
| do_exit | namespace init 退出时的 zap、孤儿回收、IPC 任务状态释放 |
| copy_namespaces、unshare_nsproxy_namespaces、free_nsproxy、exec_task_namespaces | namespace 创建、共享、切换、释放 |
| ARM64 setns 入口 | 类型校验、权限检查和切换 |
| proc namespace lookup / readdir | 展示 pid、pid_for_children、ipc |
| proc_init_fs_context、proc_kill_sb | procfs 对 PID namespace 的引用 |
| kernel_clone、ksys_unshare | SEM_UNDO 共享、失败回滚、unshare、shm 创建者脱离 |
| 18 个 ARM64 IPC syscall table 槽位 | SysV IPC 与 POSIX mqueue |
| reboot syscall table 槽位 | 子 namespace reboot；宿主保留原处理函数 |
| memfd_create syscall table 槽位 | namespace 策略；宿主原生 memfd 分配、seals |
| pidfd_ioctl | PID / IPC namespace FD 和引用 |
| fuse_conn_init、fuse_conn_put | FUSE 连接 namespace 引用 |
| cgroup_pidlist_start | cgroup-v1 PID 列表缓存及延迟销毁引用 |
| 三种 BPF iterator descriptor 的 init/fini | task、task_file、task_vma iterator 引用 |
| perf_event_alloc、free_event_rcu | 保留宿主实现，额外持有子 namespace 引用 |

预检检查 21 个函数入口的符号边界、长度和对齐，20 个 syscall table 槽位、6 个
BPF 回调的原始目标；实际替换函数指针的槽位还核对 KCFI 类型字。静态的直接调用
函数可能没有 KCFI 前缀，因此不把任意入口前四字节当作类型哈希。

perf 通过受限跳板调用宿主原函数；只允许已确认不依赖 PC 的栈序言指令，并在不支持
的指令出现时拒绝加载。分配失败不登记引用，成功的子 namespace event 由侧表管理，
最终 RCU 回调释放；加载前已有的宿主 event 没有额外引用。

入口替换、syscall table 和 BPF 回调写入都在 stop_machine 中执行。失败恢复全部
已保存入口；恢复失败则 panic，避免继续运行不完整的生命周期替换。该机制仍不是
正式 livepatch 一致性协议；手机已验证当前负载下激活，没有穷尽所有过渡时序。
激活后模块永久 pin，不支持热卸载。

## IPC 任务状态和策略

CONFIG_SYSVIPC 保持关闭，避免改变宿主 task_struct。模块哈希表按 task 指针存放
undo_list 与 shm_clist。CLONE_SYSVSEM 在 copy_process 前准备共享引用，失败撤回，
成功后在唤醒子任务前安装状态。普通 fork 不继承 undo 调整。共享内存映射保留原生
VMA open/close 生命周期；退出先进行 undo 和 shm 清理，再释放任务侧表状态。

IPC namespace 销毁执行 msg/sem/shm 清理；mqueue 通过挂载和 inode 生命周期清理。
构造失败也解除 mqueue 内核挂载。激活前拒绝用户 mqueue 挂载和 memfd 策略写入。
IPC 的原 security / audit 调用保留，真机测试的 root 域可以正常使用；其他 SELinux
域的容器策略没有逐域验证。

## ABI 核对

相同构建的模块 DWARF 生成完整 BTF，与手机 `/sys/kernel/btf/vmlinux` 比较：
714 个具名聚合类型的大小及成员偏移/宽度完全匹配；另外三个类型整体大小和共有
成员不变，仅手机增加厂商别名：

- mm_struct：1,216 字节，dmabuf_info。
- task_struct：5,184 字节，dmabuf_info 和一个 vendor 保留字段别名。
- files_struct：704 字节，dmabuf_info 和 KABI 别名。

没有借用这些保留位置。模块的 418 个未定义依赖符号在手机均存在；372 个在两侧
BTF 均有函数原型的依赖全部一致。其余依赖包括数据符号和缺少可比较原型的符号，
不声称它们通过了函数原型检查。BTF 布局工具也不比较全部内联语义或厂商代码行为。

构建的 __versions 为空，仍有 `no symbol version for module_layout` 以及 BTF
address_space 重复候选提示。ksud 会重定位非导出符号并适配 vermagic，这不构成
完整的 CRC 校验。上述 ABI 证据、实际内核 QEMU 和真机测试共同限定了当前支持范围。

## 仍未验证的范围

- 完整 Droidspaces rootfs / systemd 启动和长期高并发、OOM、故障注入。
- 32 位兼容 IPC syscall table（这台手机使用 ARM64-only ABI）。
- SIGEV_THREAD 通知、hugetlb 共享内存和其他 SELinux 域。
- 内核升级、其他设备以及任意厂商模块在子 PID namespace 中使用内联 get/put 的情况。
- devtmpfs、user namespace 和额外 cgroup 控制器不在本次 PID / IPC 移植范围。

当前产物用于这台个人设备，重启后恢复原内核能力；再次加载须使用相同完整内核版本。
