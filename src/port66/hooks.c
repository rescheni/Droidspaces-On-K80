// SPDX-License-Identifier: GPL-2.0-only
/* Experimental entry replacements for the validated target kernel. */
#define pr_fmt(fmt) "droidspaces_ns: " fmt
#include <linux/module.h>
#include <linux/stop_machine.h>
#include <linux/kallsyms.h>
#include <linux/pid_namespace.h>
#include <linux/proc_ns.h>
#include <linux/fs_context.h>
#include <asm/patching.h>
#include <asm/ptrace.h>
#include <asm/unistd.h>
#include <linux/sched/task.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include "fs/fuse/fuse_i.h"
#include "core.h"
#include "port.h"
#include "ipc-backend.h"

extern const struct file_operations ds_proc_ns_dir_operations;
extern const struct inode_operations ds_proc_ns_dir_inode_operations;
extern long __arm64_sys_ds_setns(const struct pt_regs *regs);
extern void ds_delayed_put_pid(struct rcu_head *rcu);
extern int ds_proc_init_fs_context(struct fs_context *fc);
extern void ds_proc_kill_sb(struct super_block *sb);
extern pid_t ds_kernel_clone(struct kernel_clone_args *args);
extern int ds_ksys_unshare(unsigned long flags);
extern long (*ds_native_memfd_create)(const struct pt_regs *);
extern long (*ds_native_reboot)(const struct pt_regs *);
extern void ds_fuse_conn_init(struct fuse_conn *, struct fuse_mount *,
	struct user_namespace *, const struct fuse_iqueue_ops *, void *);
extern void ds_fuse_delayed_release(struct rcu_head *);
extern void *ds_cgroup_pidlist_start(struct seq_file *, loff_t *);
extern int ds_bpf_init_seq_pidns(void *, struct bpf_iter_aux_info *);
extern void ds_bpf_fini_seq_pidns(void *);
extern struct perf_event *ds_perf_event_alloc(struct perf_event_attr *, int,
	struct task_struct *, struct perf_event *, struct perf_event *,
	perf_overflow_handler_t, void *, int);
extern void ds_perf_free_event_rcu(struct rcu_head *);
extern u32 ds_perf_alloc_original[], ds_perf_free_original[];

struct ds_pointer_hook {
	unsigned long *slot;
	unsigned long saved, replacement;
};
static struct ds_pointer_hook bpf_hooks[6];

static bool matching_cfi(unsigned long native, unsigned long replacement)
{
	u32 host_type, module_type;

	if (!IS_ENABLED(CONFIG_CFI_CLANG))
		return true;
	return !safe_read(&host_type, (void *)(native - sizeof(u32)), sizeof(u32)) &&
		!safe_read(&module_type, (void *)(replacement - sizeof(u32)), sizeof(u32)) &&
		host_type == module_type;
}

static int write_pointer(unsigned long *slot, unsigned long value)
{
	u32 *words = (u32 *)slot;
	int ret = aarch64_insn_patch_text_nosync(words, lower_32_bits(value));
	return ret ? ret : aarch64_insn_patch_text_nosync(words + 1, upper_32_bits(value));
}

static int prepare_bpf_hooks(void)
{
	static const char *const names[] = { "task_seq_info", "task_file_seq_info", "task_vma_seq_info" };
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct bpf_iter_seq_info *info = (void *)kallsyms_name_to_addr(names[i]);
		struct ds_pointer_hook *init = &bpf_hooks[i * 2], *fini = init + 1;
		if (!info)
			return -ENOENT;
		init->slot = (void *)&info->init_seq_private;
		fini->slot = (void *)&info->fini_seq_private;
		if (safe_read(&init->saved, init->slot, sizeof(init->saved)) ||
		    safe_read(&fini->saved, fini->slot, sizeof(fini->saved)) ||
		    init->saved != kallsyms_name_to_addr("init_seq_pidns") ||
		    fini->saved != kallsyms_name_to_addr("fini_seq_pidns"))
			return -EINVAL;
		init->replacement = (unsigned long)ds_bpf_init_seq_pidns;
		fini->replacement = (unsigned long)ds_bpf_fini_seq_pidns;
		if (!matching_cfi(init->saved, init->replacement) ||
		    !matching_cfi(fini->saved, fini->replacement))
			return -EPROTOTYPE;
	}
	return 0;
}

#define DECLARE_IPC_SYSCALL(name) extern long __arm64_sys_ds_##name(const struct pt_regs *)
DECLARE_IPC_SYSCALL(msgget);
DECLARE_IPC_SYSCALL(msgctl);
DECLARE_IPC_SYSCALL(msgsnd);
DECLARE_IPC_SYSCALL(msgrcv);
DECLARE_IPC_SYSCALL(semget);
DECLARE_IPC_SYSCALL(semctl);
DECLARE_IPC_SYSCALL(semop);
DECLARE_IPC_SYSCALL(semtimedop);
DECLARE_IPC_SYSCALL(shmget);
DECLARE_IPC_SYSCALL(shmctl);
DECLARE_IPC_SYSCALL(shmat);
DECLARE_IPC_SYSCALL(shmdt);
DECLARE_IPC_SYSCALL(mq_open);
DECLARE_IPC_SYSCALL(mq_unlink);
DECLARE_IPC_SYSCALL(mq_timedsend);
DECLARE_IPC_SYSCALL(mq_timedreceive);
DECLARE_IPC_SYSCALL(mq_notify);
DECLARE_IPC_SYSCALL(mq_getsetattr);
DECLARE_IPC_SYSCALL(reboot);
DECLARE_IPC_SYSCALL(memfd_create);

struct ds_syscall_hook {
	const char *name;
	unsigned int nr;
	unsigned long replacement, saved;
};
#define IPC_SYSCALL(name) { "__arm64_sys_" #name, __NR_##name, (unsigned long)__arm64_sys_ds_##name, 0 }
static struct ds_syscall_hook ipc_syscalls[] = {
	IPC_SYSCALL(msgget), IPC_SYSCALL(msgctl), IPC_SYSCALL(msgsnd), IPC_SYSCALL(msgrcv),
	IPC_SYSCALL(semget), IPC_SYSCALL(semctl), IPC_SYSCALL(semop), IPC_SYSCALL(semtimedop),
	IPC_SYSCALL(shmget), IPC_SYSCALL(shmctl), IPC_SYSCALL(shmat), IPC_SYSCALL(shmdt),
	IPC_SYSCALL(mq_open), IPC_SYSCALL(mq_unlink), IPC_SYSCALL(mq_timedsend),
	IPC_SYSCALL(mq_timedreceive), IPC_SYSCALL(mq_notify), IPC_SYSCALL(mq_getsetattr),
	IPC_SYSCALL(reboot), IPC_SYSCALL(memfd_create),
};
static unsigned long *syscall_table;

static int write_syscall(struct ds_syscall_hook *hook, bool restore)
{
	unsigned long value = restore ? hook->saved : hook->replacement;
	u32 *slot = (u32 *)&syscall_table[hook->nr];
	int ret = aarch64_insn_patch_text_nosync(slot, lower_32_bits(value));
	return ret ? ret : aarch64_insn_patch_text_nosync(slot + 1, upper_32_bits(value));
}

struct ds_hook {
	const char *name;
	unsigned long replacement;
	unsigned long address;
	u32 saved[4];
	u32 patch[4];
	u32 *original;
};

#define HOOK(name, target) { name, (unsigned long)(target), 0, { 0 }, { 0 }, NULL }
#define HOOK_ORIGINAL(name, target, original) { name, (unsigned long)(target), 0, { 0 }, { 0 }, original }
static struct ds_hook hooks[] = {
	HOOK_ORIGINAL("perf_event_alloc", ds_perf_event_alloc, ds_perf_alloc_original),
	HOOK_ORIGINAL("free_event_rcu", ds_perf_free_event_rcu, ds_perf_free_original),
	HOOK("fuse_conn_init", ds_fuse_conn_init),
	HOOK("delayed_release", ds_fuse_delayed_release),
	HOOK("cgroup_pidlist_start", ds_cgroup_pidlist_start),
	HOOK("kernel_clone", ds_kernel_clone),
	HOOK("ksys_unshare", ds_ksys_unshare),
	HOOK("do_exit", ds_do_exit),
	HOOK("put_pid", ds_put_pid),
	HOOK("delayed_put_pid", ds_delayed_put_pid),
	HOOK("alloc_pid", ds_alloc_pid),
	HOOK("free_nsproxy", ds_free_nsproxy),
	HOOK("copy_namespaces", ds_copy_namespaces),
	HOOK("unshare_nsproxy_namespaces", ds_unshare_nsproxy_namespaces),
	HOOK("exec_task_namespaces", ds_exec_task_namespaces),
	HOOK("__arm64_sys_setns", __arm64_sys_ds_setns),
	HOOK("proc_init_fs_context", ds_proc_init_fs_context),
	HOOK("proc_kill_sb", ds_proc_kill_sb),
};

static struct ds_hook proc_hooks[2];

static bool movable_prologue(u32 op)
{
	/* NOP/PACIASP; stack allocation/frame pointer; stack stores, no PC operands. */
	return op == 0xd503201f || op == 0xd503233f ||
		(op & 0xffc003ff) == 0xd10003ff ||
		(op & 0xffc003ff) == 0x910003fd ||
		(op & 0xffc003e0) == 0xf90003e0 || /* STR xN,[sp,#imm] */
		(op & 0xff8003e0) == 0xa90003e0 ||
		(op & 0xff8003e0) == 0xa98003e0;
}

static int prepare_hook(struct ds_hook *hook)
{
	char symbol[KSYM_SYMBOL_LEN];
	char *start, *end;
	unsigned long offset, size;

	hook->address = kallsyms_name_to_addr(hook->name);
	if (!hook->address || (hook->address & 3) ||
	    safe_read(hook->saved, (void *)hook->address, sizeof(hook->saved)))
		return -ENOENT;
	/* Direct entry branches preserve the host prefix. Local direct-call-only
	 * functions can lack a KCFI word; compare hashes only for pointer slots.
	 */
	sprint_symbol(symbol, hook->address);
	start = strchr(symbol, '+');
	if (!start)
		return -EINVAL;
	offset = simple_strtoul(start + 1, &end, 0);
	if (end == start + 1 || *end != '/')
		return -EINVAL;
	start = end + 1;
	size = simple_strtoul(start, &end, 0);
	if (end == start || offset || size < sizeof(hook->saved))
		return -EINVAL;
	if (hook->original) {
		unsigned int i;
		for (i = 0; i < ARRAY_SIZE(hook->saved); i++) {
			if (!movable_prologue(hook->saved[i])) {
				pr_err("unsupported original prologue %s word %u: %08x\n",
				       hook->name, i, hook->saved[i]);
				return -EOPNOTSUPP;
			}
		}
	}
	/* ldr x16, .+8; br x16; .quad destination. CFI type prefixes stay intact. */
	hook->patch[0] = 0x58000050;
	hook->patch[1] = 0xd61f0200;
	hook->patch[2] = lower_32_bits(hook->replacement);
	hook->patch[3] = upper_32_bits(hook->replacement);
	return 0;
}

static int write_hook(struct ds_hook *hook, bool restore)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hook->saved); i++) {
		ret = aarch64_insn_patch_text_nosync((void *)(hook->address + i * 4),
					 restore ? hook->saved[i] : hook->patch[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int install_stopped(void *unused)
{
	unsigned int i;
	int ret;
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		struct ds_hook *h = &hooks[i];
		u32 code[8];
		unsigned int j;
		if (!h->original)
			continue;
		memcpy(code, h->saved, sizeof(h->saved));
		code[4] = 0x58000050;
		code[5] = 0xd61f0200;
		code[6] = lower_32_bits(h->address + sizeof(h->saved));
		code[7] = upper_32_bits(h->address + sizeof(h->saved));
		for (j = 0; j < ARRAY_SIZE(code); j++) {
			ret = aarch64_insn_patch_text_nosync(&h->original[j], code[j]);
			if (ret)
				goto rollback;
		}
	}

	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		ret = write_hook(&hooks[i], false);
		if (ret)
			goto rollback;
	}
	for (i = 0; i < ARRAY_SIZE(proc_hooks); i++) {
		ret = write_hook(&proc_hooks[i], false);
		if (ret)
			goto rollback;
	}
	WRITE_ONCE(init_pid_ns.ns.ops, &pidns_operations);
	for (i = 0; i < ARRAY_SIZE(ipc_syscalls); i++) {
		ret = write_syscall(&ipc_syscalls[i], false);
		if (ret) {
			WRITE_ONCE(init_pid_ns.ns.ops, NULL);
			goto rollback;
		}
	}
	WRITE_ONCE(ds_ipc_active, true);
	for (i = 0; i < ARRAY_SIZE(bpf_hooks); i++) {
		ret = write_pointer(bpf_hooks[i].slot, bpf_hooks[i].replacement);
		if (ret) {
			WRITE_ONCE(ds_ipc_active, false);
			WRITE_ONCE(init_pid_ns.ns.ops, NULL);
			goto rollback;
		}
	}
	return 0;

rollback:
	for (i = 0; i < ARRAY_SIZE(bpf_hooks); i++)
		if (write_pointer(bpf_hooks[i].slot, bpf_hooks[i].saved))
			panic("droidspaces_ns: BPF rollback failed");
	for (i = 0; i < ARRAY_SIZE(ipc_syscalls); i++)
		if (write_syscall(&ipc_syscalls[i], true))
			panic("droidspaces_ns: syscall rollback failed");
	/* Do not resume CPUs with an incomplete set of lifecycle replacements. */
	for (i = 0; i < ARRAY_SIZE(hooks); i++)
		if (write_hook(&hooks[i], true))
			panic("droidspaces_ns: hook rollback failed");
	for (i = 0; i < ARRAY_SIZE(proc_hooks); i++)
		if (write_hook(&proc_hooks[i], true))
			panic("droidspaces_ns: proc hook rollback failed");
	return ret;
}

int ds_hooks_prepare(void)
{
	unsigned int i;
	int ret;

	if (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL) || init_pid_ns.ns.ops)
		return -EOPNOTSUPP;
	ret = prepare_bpf_hooks();
	if (ret)
		return ret;
	syscall_table = (void *)kallsyms_name_to_addr("sys_call_table");
	if (!syscall_table)
		return -ENOENT;
	for (i = 0; i < ARRAY_SIZE(ipc_syscalls); i++) {
		struct ds_syscall_hook *hook = &ipc_syscalls[i];
		if (safe_read(&hook->saved, &syscall_table[hook->nr], sizeof(hook->saved)) ||
		    hook->saved != kallsyms_name_to_addr(hook->name)) {
			pr_err("syscall %u no longer points to %s\n", hook->nr, hook->name);
			return -EOPNOTSUPP;
		}
		if (hook->nr == __NR_reboot)
			ds_native_reboot = (void *)hook->saved;
		if (hook->nr == __NR_memfd_create)
			ds_native_memfd_create = (void *)hook->saved;
		if (!matching_cfi(hook->saved, hook->replacement)) {
			pr_err("CFI mismatch for %s\n", hook->name);
			return -EPROTOTYPE;
		}
	}
	proc_hooks[0] = (struct ds_hook)HOOK("proc_ns_dir_readdir",
					ds_proc_ns_dir_operations.iterate_shared);
	proc_hooks[1] = (struct ds_hook)HOOK("proc_ns_dir_lookup",
					ds_proc_ns_dir_inode_operations.lookup);
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		ret = prepare_hook(&hooks[i]);
		if (ret) {
			pr_err("cannot prepare hook %s: %d\n", hooks[i].name, ret);
			return ret;
		}
	}
	for (i = 0; i < ARRAY_SIZE(proc_hooks); i++) {
		ret = prepare_hook(&proc_hooks[i]);
		if (ret)
			return ret;
	}
	pr_info("entry preflight passed: %zu functions, %zu syscalls, %zu BPF callbacks; no patches written\n",
		ARRAY_SIZE(hooks) + ARRAY_SIZE(proc_hooks), ARRAY_SIZE(ipc_syscalls), ARRAY_SIZE(bpf_hooks));
	return 0;
}

int ds_hooks_install(void)
{
	int ret = ds_hooks_prepare();

	if (ret)
		return ret;
	/* Objects in procfs can retain operation pointers after all tasks exit. */
	__module_get(THIS_MODULE);
	ret = stop_machine(install_stopped, NULL, NULL);
	if (ret)
		module_put(THIS_MODULE);
	return ret;
}
