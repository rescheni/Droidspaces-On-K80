# 实现机制

内核里没有编入的子系统，为什么一个 `.ko` 能补上？本文说明本项目实际使用的机制，
以及它成立与失效的边界。文中引用的入口数量、校验项、加载顺序都对应
`src/port61full/` 的当前代码。

## 1. 前提：为什么普通模块做不到

`CONFIG_PID_NS` / `CONFIG_IPC_NS` / `CONFIG_POSIX_MQUEUE` 不是模块化的编译开关，
它们决定：

- `kernel/pid_namespace.c`、`ipc/namespace.c`、`ipc/mqueue.c` 是否参与编译；
- `struct pid_namespace`、`struct ipc_namespace` 等对象是否真的存在；
- `struct task_struct` / `struct nsproxy` 中相关字段的**布局**。

因此普通模块只能"使用"这些子系统，而**完全缺失**时没有任何符号可以链接。本项目
的做法不是"补一个开关"，而是**把子系统重新实现进模块**，再在运行时接管内核里那
些本该调用它的位置。

这需要三件事同时成立：

1. 能拿到内核符号地址（含未导出符号）；
2. 能改内核代码入口；
3. 改完之后，内核原有的调用路径会走到模块的实现。

## 2. 三层基础能力

### 2.1 符号解析：KallRecon（`vendor/kallrecon/`）

模块没有 `kallsyms_lookup_name`（多数内核已不导出，本机内核符号表里确实没有
`kallsyms_name_to_addr` 依赖）。KallRecon 直接在内存中定位 kallsyms 的
`offsets / names / token table / token index / seqs / markers` 结构，然后自行解码
压缩符号名，对外提供：

- `kallsyms_name_to_addr(name)`：按名字查地址；
- `sym_addr(idx)`：按索引取地址；
- `kallrecon_find_variant(name, prefer_cfi_jt)`：处理厂商改名与 CFI jump table
  变体。

设备上 `CONFIG_KALLSYMS` 打开，`/proc/kallsyms` 可读（root），因此解码所需数据
结构完整存在。

### 2.2 入口替换：跳板 + `stop_machine`（`hooks.c`）

对每个目标函数，模块在**入口处写入 4 条指令**：

```asm
ldr x16, .+8        ; 0x58000050
br  x16             ; 0xd61f0200
.quad 目标函数
```

写入通过 `aarch64_insn_patch_text_nosync()` 完成，并在 `stop_machine()` 中执行，
即所有 CPU 都停在一个已知状态下才改代码。要改内核指令内存，前提是能拿到符号
地址并在 `stop_machine` 上下文写只读段。

对需要**调用宿主原实现**的两个入口（`perf_event_alloc`、`free_event_rcu`），模块
额外构造原函数跳板：把被覆盖的前 4 条指令放进模块自己的缓冲区，末尾接一个跳回
`address + 16` 的分支，于是模块可以"先做自己的事，再调原函数"。为保证这段搬运
安全，`prepare_hook()` 只接受**位置无关**的序言指令：

| 允许 | 说明 |
| --- | --- |
| `NOP` / `PACIASP` | 0xd503201f / 0xd503233f |
| `sub sp, sp, #imm`、`mov x29, sp` | 栈分配与帧指针 |
| `stp/str` 到 `[sp, #imm]` | 栈保存，不含 PC 相对操作数 |

出现其它指令（例如 PC 相对寻址）就拒绝加载，而不是冒险搬运。

### 2.3 syscall 表替换

ARM64 的 `sys_call_table` 是函数指针数组。模块把 19 个槽位换成自己的入口：

| 类别 | 槽位 |
| --- | --- |
| SysV IPC | `msgget` `msgctl` `msgsnd` `msgrcv` `semget` `semctl` `semop` `semtimedop` `shmget` `shmctl` `shmat` `shmdt` |
| POSIX mqueue | `mq_open` `mq_unlink` `mq_timedsend` `mq_timedreceive` `mq_notify` `mq_getsetattr` |
| reboot | `reboot`（容器内重启路由到子 PID namespace，宿主保留原处理函数） |

写指针同样按两个 32 位字写入，并对替换目标做 CFI 校验（见 2.4）。

### 2.4 校验与回滚

加载时先做**只读预检**，任何一项不满足就拒绝，不写任何代码：

- 符号必须存在、地址 4 字节对齐、`sprint_symbol()` 显示偏移为 0 且长度足够；
- syscall 槽位必须**仍然指向**预期的 `__arm64_sys_<name>`；在本机这些槽位是
  `ni_syscall` 的别名，预检如实比对，不匹配即拒绝；
- 启用 `CONFIG_CFI_CLANG` 时，比较目标函数入口前 4 字节的 KCFI 类型字
  （`matching_cfi()`），类型不一致即拒绝，避免间接调用被 CFI 拦下；
- 内存模型要求 `init_pid_ns.ns.ops == NULL`（本机确实为空）且未使用
  `CONFIG_ARM64_BTI_KERNEL`。

真正写入时如果中途失败，会**逐项回滚**；回滚失败直接 `panic()`，宁可停机也不
让内核运行在半套生命周期替换之上。

### 2.5 为什么必须精确匹配内核

替换入口要写入**绝对地址**，而模块编译时用的结构体偏移（`task_struct`、
`nsproxy`、`pid_namespace`、`ipc_namespace` 等）来自 DDK 的 6.1 源码。厂商内核
只要改动这些结构或函数布局，就会写出错误代码。因此：

- 编译使用 DDK 的 6.1 源码树（`/opt/ddk/src/android14-6.1`）；
- 构建产物与**同一棵树的 vmlinux BTF** 比对布局；
- 加载前再用设备 BTF 与之对齐，并要求 `uname -r` 完全一致。

在本机上，模块 731 个具名聚合类型与设备 BTF 的尺寸/成员偏移全部一致，仅有 3 处
厂商别名（`task_struct.dmabuf_info`、`mm_struct.abi_extend`、
`files_struct.dmabuf_info`），它们与既有 `android_kabi_reserved*` / 保留字段**同
偏移同宽度**，模块没有借用这些位置。

## 3. 接管了哪些入口

激活时共替换 **17 个函数入口 + 2 个 procfs 操作指针 + 19 个 syscall 槽位 +
6 个 BPF 回调指针 + 1 个 `ns_common.ops`**。

| 入口 | 模块行为 |
| --- | --- |
| `alloc_pid` / `put_pid` / `delayed_put_pid` | 多层 PID 分配、namespace 引用与延迟释放 |
| `do_exit` | namespace init 退出时的 zap、孤儿回收、IPC 任务状态释放 |
| `kernel_clone` | `CLONE_SYSVSEM` 共享 undo 状态的准备/安装/回滚 |
| `copy_namespaces` | 创建/共享 `nsproxy`，调用模块自己的 `copy_ipcs` / `copy_pid_ns` |
| `unshare_nsproxy_namespaces` / `ksys_unshare` | `unshare(CLONE_NEWPID/NEWIPC)` 路径 |
| `free_nsproxy` | 释放模块创建的 namespace 引用 |
| `__arm64_sys_setns` | 类型校验、权限检查、切换，并初始化目标 namespace 后端 |
| `proc_init_fs_context` / `proc_kill_sb` | procfs 对 PID namespace 的引用管理 |
| `proc_ns_dir_readdir` / `proc_ns_dir_lookup` | 让 `/proc/*/ns` 列出来自模块的操作表 |
| `perf_event_alloc` / `free_event_rcu` | 保留宿主实现，额外持有子 namespace 引用 |
| `fuse_conn_init` / `delayed_release` | FUSE 连接的 namespace 引用 |
| `cgroup_pidlist_start` | cgroup-v1 PID 列表缓存及其延迟销毁引用 |
| 6 个 BPF iterator 回调 | `task` / `task_file` / `task_vma` iterator 的引用 |

另外把 `init_pid_ns.ns.ops` 指向模块的 `pidns_operations`，使 `/proc/*/ns/pid`
的解析、`setns`、`nsfs` 路径都走模块实现。

## 4. PID namespace 路径

- `alloc_pid()` 按 `pid_namespace` 级别分配 `struct pid`，多级 namespace 各自分配
  一个 `upid`；
- `struct pid` 的缓存按级别创建，模块自行 `kmem_cache_create()`；
- `copy_pid_ns()` 支持 `CLONE_NEWPID` 创建、共享（`flags = 0`）和"从非活跃 namespace
  再创建"被拒绝（`-EINVAL`）；
- 子 namespace 的 init 进程 `getpid() == 1`、`getppid() == 0`，退出时由 `do_exit`
  回收并唤醒父进程。

## 5. IPC namespace / SysV / mqueue 路径

本机内核不仅没有 IPC namespace，连接口层都不存在：`init_ipc_ns`、`mq_lock`、
`mq_init_ns`、`mq_put_mnt`、`setup_mq_sysctls` 等符号在设备上**全部缺失**。所以
模块必须自带整个后端，这就是新增 `src/port61full` 的原因：

| 内容 | 来源 |
| --- | --- |
| `ipc/namespace.c` 移植 | `src/port61/ipc.c`（6.1 原生实现） |
| `init_ipc_ns`、`mq_lock` | 本模块 `ipc.c` 定义（宿主没有） |
| `ipc/mqueue.c` | 从固定 6.1 源码树移植，改命名空间查找、入口命名、挂载门禁、init/exit |
| `ipc/mq_sysctl.c` | 同上，`__register_sysctl_table(3 参数)` 与 6.1 一致 |
| SysV msg/sem/shm | 模块实现；`SEM_UNDO`、`shm` 创建者列表放在**模块侧任务表**里，因为宿主 `task_struct` 没有对应字段（`CONFIG_SYSVIPC` 保持关闭，避免改动布局） |
| mqueue 文件系统 | 模块注册 `mqueue` fs 与每个 IPC namespace 的内核挂载，并在激活前拒绝用户态挂载 |

POSIX 队列的 syscall 在本机是 `ni_syscall` 别名，模块直接替换这些槽位。

`struct ipc_namespace` 的布局不受这些 config 影响（含 `CONFIG_CHECKPOINT_RESTORE`
分支），本机与 DDK 均为 1272 字节且成员偏移一致，因此可以安全地按编译时偏移访问。

注意一处**不是** bug 的现象：`mq_open()` 的原始 syscall 参数不含前导 `/`
（libc 会去掉前缀），直接以 `"/name"` 调用会得到 `-EACCES`。

## 6. procfs 呈现

`/proc/self/ns` 的内容来自模块的操作表（`proc_ns.c` 的 `ns_entries[]`）。
激活后该目录新增 `pid`、`pid_for_children`、`ipc`，Droidspaces 的必需项检查正是
读取这三项，因此从"缺 2 项"变为 `All required features found!`。

## 7. 引用与生命周期

namespace 对象是引用计数的，长期存活的内核对象必须被追踪：

- `perf` 事件通过受限跳板调用宿主原函数，成功后由模块侧表持有子 namespace 引用，
  最终在 RCU 回调释放；加载前已存在的宿主事件没有额外引用；
- FUSE 连接的 namespace 引用在连接初始化时获取、在延迟释放路径归还；
- cgroup-v1 `pidlist` 缓存、BPF iterator 的保留 FD 分别使用模块的引用与销毁路径；
- `pid_live` / `ipc_live` / `ipc_tasks` 三个模块参数暴露计数，便于确认归零。

## 8. 加载顺序与不可逆点

`ds_port_init()` 的顺序：

1. 目标门禁：内核 release +（经加载器检查的）BTF 指纹；
2. `kallrecon_supply()` + `find_kallsyms_base()`：符号解析；
3. `ds_nsproxy_cache_init()`：定位宿主 `nsproxy_cachep`（厂商内核把它改名成
   `nsproxy_cachep.llvm.<hash>`，无法按名字链接）；
4. 门禁：要求原生 `pidns_operations` / `ipcns_operations` **不存在**（= 确实缺失）；
5. `ds_hooks_prepare()`：只读预检，不写任何代码；
6. `ds_pid_init()` + `ds_ipc_backend_init()`：创建缓存、命名空间、mqueue 挂载与 sysctl；
7. `ds_port_selftest()`：用真实 `copy_pid_ns()` / `copy_ipcs()` 验证创建、共享、
   非法创建被拒、延迟销毁后计数归零；
8. `activate=1` 时 `ds_hooks_install()`：`stop_machine` 内完成全部替换。

**激活后模块永久 pin**：`ds_hooks_install()` 成功路径持有模块引用，`module_put` 只在
安装失败时执行。因此替换或卸载 `.ko` 必须重启。

## 9. 本机未覆盖的部分

- devtmpfs / user namespace：这两个 config 同样为 `n`，本包不含对应实现；
- `memfd` namespace 策略与 `pidfd` namespace ioctl：6.1 基线本身没有这两个接口；
- 32 位兼容 IPC syscall 表：本机为 ARM64-only ABI；
- SIGEV_THREAD 通知、hugetlb 共享内存、其他 SELinux 域的容器策略。

## 10. 边界声明

上述机制不是正式 livepatch 一致性协议：它替换入口、且不保证覆盖所有过渡时序
（例如加载瞬间正处于目标函数中间的任务）。已验证范围内（本机 SELinux Enforcing、
`activate=1`、容器可启动并可 `enter`）没有出现 BUG / WARNING / Oops / CFI failure；
这不等于任意负载、任意厂商内核都安全。
