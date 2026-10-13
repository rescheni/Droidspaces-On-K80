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
#ifdef DS_USERNS_LIVE
#include "../userns/live.h"
#include "../userns/live-fuse.h"
extern u32 ds_userns_fuse_simple_request_original[];
extern u32 ds_userns_fuse_do_set_acl_original[], ds_userns_fuse_do_get_acl_original[];
extern u32 ds_userns_mm_init_original[], ds_userns_ns_ioctl_original[], ds_userns_sk_getsockopt_original[], ds_userns_fuse_get_tree_original[];
#include <linux/user_namespace.h>
#include <linux/utsname.h>
#endif

extern const struct file_operations ds_proc_ns_dir_operations;
extern const struct inode_operations ds_proc_ns_dir_inode_operations;
extern long __arm64_sys_ds_setns(const struct pt_regs *regs);
extern void ds_delayed_put_pid(struct rcu_head *rcu);
extern int ds_proc_init_fs_context(struct fs_context *fc);
extern void ds_proc_kill_sb(struct super_block *sb);
extern pid_t ds_kernel_clone(struct kernel_clone_args *args);
extern int ds_ksys_unshare(unsigned long flags);
extern long ds_pidfd_ioctl(struct file *, unsigned int, unsigned long);
extern long (*ds_native_memfd_create)(const struct pt_regs *);
extern long (*ds_native_reboot)(const struct pt_regs *);
extern void ds_fuse_conn_init(struct fuse_conn *, struct fuse_mount *,
	struct user_namespace *, const struct fuse_iqueue_ops *, void *);
extern void ds_fuse_conn_put(struct fuse_conn *);
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
#ifdef DS_USERNS_LIVE
static struct ds_pointer_hook callback_hooks[7];
#else
static struct ds_pointer_hook callback_hooks[6];
#endif

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

static int prepare_callback_hooks(void)
{
	static const char *const names[] = { "task_seq_info", "task_file_seq_info", "task_vma_seq_info" };
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct bpf_iter_seq_info *info = (void *)kallsyms_name_to_addr(names[i]);
		struct ds_pointer_hook *init = &callback_hooks[i * 2], *fini = init + 1;
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
#ifdef DS_USERNS_LIVE
	{
		struct ctl_table_header *header = init_user_ns.sysctls;
		struct ctl_table_root *root;
		struct ds_pointer_hook *hook = &callback_hooks[6];
		if (!header || safe_read(&root, &header->root, sizeof(root)) || !root)
			return -EINVAL;
		hook->slot = (void *)&root->lookup;
		if (safe_read(&hook->saved, hook->slot, sizeof(hook->saved)) ||
		    hook->saved != kallsyms_name_to_addr("set_lookup"))
			return -EINVAL;
		hook->replacement = (unsigned long)ds_userns_set_lookup;
		if (!matching_cfi(hook->saved, hook->replacement))
			return -EPROTOTYPE;
	}
#endif
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
#ifdef DS_USERNS_LIVE
DECLARE_IPC_SYSCALL(chroot);
DECLARE_IPC_SYSCALL(newfstat);
DECLARE_IPC_SYSCALL(newfstatat);
DECLARE_IPC_SYSCALL(setregid);
DECLARE_IPC_SYSCALL(setgid);
DECLARE_IPC_SYSCALL(setreuid);
DECLARE_IPC_SYSCALL(setuid);
DECLARE_IPC_SYSCALL(setresuid);
DECLARE_IPC_SYSCALL(getresuid);
DECLARE_IPC_SYSCALL(setresgid);
DECLARE_IPC_SYSCALL(getresgid);
DECLARE_IPC_SYSCALL(setfsuid);
DECLARE_IPC_SYSCALL(setfsgid);
DECLARE_IPC_SYSCALL(getgroups);
DECLARE_IPC_SYSCALL(setgroups);
DECLARE_IPC_SYSCALL(statmount);
DECLARE_IPC_SYSCALL(listmount);
DECLARE_IPC_SYSCALL(getuid);
DECLARE_IPC_SYSCALL(geteuid);
DECLARE_IPC_SYSCALL(getgid);
DECLARE_IPC_SYSCALL(getegid);
#endif

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
#ifdef DS_USERNS_LIVE
	IPC_SYSCALL(chroot),
	IPC_SYSCALL(setregid),
	IPC_SYSCALL(setgid),
	IPC_SYSCALL(setreuid),
	IPC_SYSCALL(setuid),
	IPC_SYSCALL(setresuid),
	IPC_SYSCALL(getresuid),
	IPC_SYSCALL(setresgid),
	IPC_SYSCALL(getresgid),
	IPC_SYSCALL(setfsuid),
	IPC_SYSCALL(setfsgid),
	IPC_SYSCALL(getgroups),
	IPC_SYSCALL(setgroups),
	IPC_SYSCALL(statmount), IPC_SYSCALL(listmount),
	IPC_SYSCALL(getuid), IPC_SYSCALL(geteuid), IPC_SYSCALL(getgid), IPC_SYSCALL(getegid),
#endif
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
	unsigned int patch_words;
};

#define HOOK(name, target) { name, (unsigned long)(target), 0, { 0 }, { 0 }, NULL, 4 }
#define HOOK_ORIGINAL(name, target, original) { name, (unsigned long)(target), 0, { 0 }, { 0 }, original, 4 }
static struct ds_hook hooks[] = {
#ifdef DS_USERNS_LIVE
	HOOK_ORIGINAL("fuse_get_tree", ds_userns_fuse_get_tree, ds_userns_fuse_get_tree_original),
	HOOK_ORIGINAL("__fuse_simple_request", ds_userns_fuse_simple_request, ds_userns_fuse_simple_request_original),
	HOOK("fuse_get_req", ds_userns_fuse_get_req),
	HOOK("fuse_parse_param", ds_userns_fuse_parse_param),
	HOOK("fuse_show_options", ds_userns_fuse_show_options),
	HOOK("fuse_change_attributes_common", ds_userns_fuse_change_attributes_common),
	HOOK("fuse_fillattr", ds_userns_fuse_fillattr),
	HOOK("fuse_allow_current_process", ds_userns_fuse_allow_current_process),
	HOOK("fuse_do_setattr", ds_userns_fuse_do_setattr),
	HOOK("fuse_get_acl", ds_userns_fuse_get_acl),
	HOOK("fuse_get_inode_acl", ds_userns_fuse_get_inode_acl),
	HOOK("fuse_set_acl", ds_userns_fuse_set_acl),
	HOOK("get_create_ext", ds_userns_fuse_get_create_ext),
	HOOK("fuse_permission", ds_userns_fuse_permission),
	HOOK("fuse_setattr", ds_userns_fuse_setattr),
	HOOK("fuse_getattr", ds_userns_fuse_getattr),
	HOOK("fuse_update_get_attr", ds_userns_fuse_update_get_attr),
	HOOK_ORIGINAL("do_set_acl", ds_userns_fuse_do_set_acl, ds_userns_fuse_do_set_acl_original),
	HOOK_ORIGINAL("do_get_acl", ds_userns_fuse_do_get_acl, ds_userns_fuse_do_get_acl_original),
	HOOK_ORIGINAL("sk_getsockopt", ds_userns_sk_getsockopt, ds_userns_sk_getsockopt_original),
	HOOK("netlink_recvmsg", ds_userns_netlink_recvmsg),
	HOOK("__scm_send", ds_userns_scm_send),
	HOOK("unix_dgram_recvmsg", ds_userns_unix_dgram_recvmsg_entry),
	HOOK("unix_seqpacket_recvmsg", ds_userns_unix_seqpacket_recvmsg),
	HOOK("__unix_dgram_recvmsg", ds_userns_unix_dgram_recvmsg),
	HOOK("unix_stream_read_generic", ds_userns_unix_stream_read_generic),
	HOOK("cgroup_init_fs_context", ds_userns_cgroup_init_fs_context),
	HOOK("copy_cgroup_ns", ds_userns_copy_cgroup_ns),
	HOOK("free_cgroup_ns", ds_userns_free_cgroup_ns),
	HOOK("copy_time_ns", ds_userns_copy_time_ns),
	HOOK("timens_put", ds_userns_timens_put),
	HOOK("timens_install", ds_userns_timens_install),
	HOOK("timens_on_fork", ds_userns_timens_on_fork),
	HOOK("free_time_ns", ds_userns_free_time_ns),
	HOOK("sysfs_init_fs_context", ds_userns_sysfs_init_fs_context),
	HOOK_ORIGINAL("ns_ioctl", ds_userns_ns_ioctl, ds_userns_ns_ioctl_original),
	HOOK("net_ns_get_ownership", ds_userns_net_ns_get_ownership),
	HOOK_ORIGINAL("mm_init", ds_userns_mm_init, ds_userns_mm_init_original),
	HOOK("__mmdrop", ds_userns_mmdrop),
	HOOK("begin_new_exec", ds_userns_begin_new_exec),
	HOOK("would_dump", ds_userns_would_dump),
	HOOK("cap_task_fix_setuid", ds_userns_cap_task_fix_setuid),
	HOOK("cap_bprm_creds_from_file", ds_userns_cap_bprm_creds_from_file),
	HOOK("cap_inode_getsecurity", ds_userns_cap_inode_getsecurity),
	HOOK("cap_convert_nscap", ds_userns_cap_convert_nscap),
	HOOK("get_vfs_caps_from_disk", ds_userns_get_vfs_caps_from_disk),
	HOOK("mnt_may_suid", ds_userns_mnt_may_suid),
	HOOK("__arm64_sys_newfstat", __arm64_sys_ds_newfstat),
	HOOK("__arm64_sys_newfstatat", __arm64_sys_ds_newfstatat),
	HOOK("do_faccessat", ds_userns_do_faccessat),
	HOOK("chown_common", ds_userns_chown_common),
	HOOK("privileged_wrt_inode_uidgid", ds_userns_privileged_wrt_inode_uidgid),
	HOOK("capable_wrt_inode_uidgid", ds_userns_capable_wrt_inode_uidgid),
	HOOK("inode_owner_or_capable", ds_userns_inode_owner_or_capable),
	HOOK("set_is_seen", ds_userns_set_is_seen),
	HOOK("proc_pid_status", ds_userns_proc_pid_status),
	HOOK("cp_statx", ds_userns_cp_statx),
	HOOK("alloc_fs_context", ds_userns_alloc_fs_context),
	HOOK("vfs_dup_fs_context", ds_userns_vfs_dup_fs_context),
	HOOK("put_fs_context", ds_userns_put_fs_context),
	HOOK("alloc_super", ds_userns_alloc_super),
	HOOK("destroy_super_work", ds_userns_destroy_super_work),
	HOOK("sget_fc", ds_userns_sget_fc),
	HOOK("sget", ds_userns_sget),
	HOOK("alloc_mnt_ns", ds_userns_alloc_mnt_ns),
	HOOK("mnt_ns_release", ds_userns_mnt_ns_release),
	HOOK("free_mnt_ns", ds_userns_free_mnt_ns),
	HOOK("prepare_creds", ds_userns_prepare_creds),
	HOOK("prepare_kernel_cred", ds_userns_prepare_kernel_cred),
	HOOK("__put_cred", ds_userns_put_cred),
	HOOK("put_cred_rcu", ds_userns_put_cred_rcu),
	HOOK("copy_creds", ds_userns_copy_creds),
	HOOK("key_change_session_keyring", ds_userns_key_change_session_keyring),
	HOOK("alloc_ucounts", ds_un_alloc_ucounts),
	HOOK("put_ucounts", ds_un_put_ucounts),
	HOOK("get_ucounts", ds_un_get_ucounts),
	HOOK("inc_ucount", ds_un_inc_ucount),
	HOOK("dec_ucount", ds_un_dec_ucount),
	HOOK("inc_rlimit_ucounts", ds_un_inc_rlimit_ucounts),
	HOOK("dec_rlimit_ucounts", ds_un_dec_rlimit_ucounts),
	HOOK("inc_rlimit_get_ucounts", ds_un_inc_rlimit_get_ucounts),
	HOOK("dec_rlimit_put_ucounts", ds_un_dec_rlimit_put_ucounts),
	HOOK("is_rlimit_overlimit", ds_un_is_rlimit_overlimit),
	HOOK("copy_utsname", ds_un_copy_utsname),
	HOOK("free_uts_ns", ds_un_free_uts_ns),
	HOOK("proc_tgid_base_lookup", ds_userns_tgid_lookup),
	HOOK("proc_tid_base_lookup", ds_userns_tid_lookup),
	HOOK("proc_tgid_base_readdir", ds_userns_tgid_readdir),
	HOOK("proc_tid_base_readdir", ds_userns_tid_readdir),
#endif
	HOOK_ORIGINAL("perf_event_alloc", ds_perf_event_alloc, ds_perf_alloc_original),
	HOOK_ORIGINAL("free_event_rcu", ds_perf_free_event_rcu, ds_perf_free_original),
	HOOK("fuse_conn_init", ds_fuse_conn_init),
	HOOK("fuse_conn_put", ds_fuse_conn_put),
	HOOK("cgroup_pidlist_start", ds_cgroup_pidlist_start),
	HOOK("pidfd_ioctl", ds_pidfd_ioctl),
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

#ifdef DS_USERNS_LIVE
/* HMA UID Fake 0.4.0 detours one non-argument instruction, then resumes at +4.
 * Keep its entry and wrapper intact. The replacement starts at that resume
 * point, so HMA still surrounds the namespace-aware capability transition.
 * Accept only the observed SUB w8,w2,#1 / B-resume trampoline, never a general
 * relocation of another module's prologue.
 */
static int setuid_chain_offset(struct ds_hook *hook)
{
	unsigned long stub, original, inline_hook, target;
	u32 code[4], stub_code[2];
	s64 delta;

	if (strcmp(hook->name, "cap_task_fix_setuid") ||
	    (hook->saved[0] & 0xfc000000) != 0x14000000)
		return 0;
	stub = module_kallsyms_lookup_name("hma_uidfake:uf_setuid_stub");
	original = module_kallsyms_lookup_name("hma_uidfake:uf_setuid_orig");
	inline_hook = module_kallsyms_lookup_name("hma_uidfake:uf_setuid_inline_hook");
	delta = sign_extend64(hook->saved[0] & 0x03ffffff, 25) * 4;
	target = hook->address + delta;
	if (!stub || !original || !inline_hook || target != stub ||
	    safe_read(code, (void *)original, sizeof(code)) ||
	    safe_read(stub_code, (void *)stub, sizeof(stub_code)) ||
	    code[0] != 0x51000448 || (code[1] & 0xfc000000) != 0x14000000 ||
	    code[2] || code[3] || stub_code[0] != 0xd50324df ||
	    (stub_code[1] & 0xfc000000) != 0x14000000 ||
	    !matching_cfi(hook->address, hook->replacement) ||
	    !matching_cfi(inline_hook, hook->replacement))
		return -EOPNOTSUPP;
	delta = sign_extend64(code[1] & 0x03ffffff, 25) * 4;
	if (original + 4 + delta != hook->address + 4)
		return -EOPNOTSUPP;
	delta = sign_extend64(stub_code[1] & 0x03ffffff, 25) * 4;
	if (stub + 4 + delta != inline_hook)
		return -EOPNOTSUPP;
	pr_info("preserving HMA setuid entry; replacing native resume body\n");
	return 4;
}
#endif

static int prepare_hook(struct ds_hook *hook)
{
	char symbol[KSYM_SYMBOL_LEN];
	char *start, *end;
	unsigned long offset, size;
	int entry_offset = 0;
	bool short_entry = false;

	hook->patch_words = ARRAY_SIZE(hook->saved);
	hook->address = kallsyms_name_to_addr(hook->name);
	if (!hook->address || (hook->address & 3) ||
	    safe_read(hook->saved, (void *)hook->address, sizeof(hook->saved)))
		return -ENOENT;

#ifdef DS_USERNS_LIVE
	entry_offset = setuid_chain_offset(hook);
	if (entry_offset < 0)
		return entry_offset;
	if (entry_offset) {
		hook->address += entry_offset;
		if (safe_read(hook->saved, (void *)hook->address, sizeof(hook->saved)))
			return -EFAULT;
	}
#endif
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
	if (end == start || offset != entry_offset || size < entry_offset + sizeof(hook->saved))
		return -EINVAL;
	/* kallsyms size includes the next function's KCFI prefix. A short
	 * return/tail-call may therefore leave fewer than 16 patchable bytes. */
	{
		unsigned int i;
		for (i = 0; i + 1 < ARRAY_SIZE(hook->saved); i++) {
			u32 op = hook->saved[i];
			if ((op & 0xfffffc1f) == 0xd65f0000 ||
			    op == 0xd65f0bff || op == 0xd65f0fff ||
			    (op & 0xfc000000) == 0x14000000) {
				/* A 12-byte leaf can use ADRP/ADD/BR within 4 GiB.
				 * Keep the fourth word (often the next KCFI hash). */
				if (i == 2 && !hook->original) {
					short_entry = true;
					break;
				}
				pr_err("short entry %s cannot hold a branch: %08x %08x %08x %08x\n",
				       hook->name, hook->saved[0], hook->saved[1], hook->saved[2], hook->saved[3]);
				if ((op & 0xfc000000) == 0x14000000) {
					long delta = sign_extend64(op & 0x03ffffff, 25) * 4;
					pr_err("entry branch destination: %pS\n",
					       (void *)(hook->address + i * 4 + delta));
				}
				return -EOPNOTSUPP;
			}
		}
	}
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
	if (short_entry) {
		s64 pages = ((s64)(hook->replacement & PAGE_MASK) -
			     (s64)(hook->address & PAGE_MASK)) >> PAGE_SHIFT;
		u32 imm;
		if (PAGE_SHIFT != 12 || pages < -(1L << 20) || pages >= (1L << 20))
			return -ERANGE;
		imm = (u32)pages & 0x1fffff;
		hook->patch[0] = 0x90000010 | ((imm & 3) << 29) | ((imm >> 2) << 5);
		hook->patch[1] = 0x91000210 | ((hook->replacement & 0xfff) << 10);
		hook->patch[2] = 0xd61f0200;
		hook->patch_words = 3;
		return 0;
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

	for (i = 0; i < hook->patch_words; i++) {
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
	for (i = 0; i < ARRAY_SIZE(callback_hooks); i++) {
		ret = write_pointer(callback_hooks[i].slot, callback_hooks[i].replacement);
		if (ret) {
			WRITE_ONCE(ds_ipc_active, false);
			WRITE_ONCE(init_pid_ns.ns.ops, NULL);
			goto rollback;
		}
	}
#ifdef DS_USERNS_LIVE
	WRITE_ONCE(init_user_ns.ns.ops, &userns_operations);
#endif
	return 0;

rollback:
	for (i = 0; i < ARRAY_SIZE(callback_hooks); i++)
		if (write_pointer(callback_hooks[i].slot, callback_hooks[i].saved))
			panic("droidspaces_ns: callback rollback failed");
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
	ret = prepare_callback_hooks();
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
	pr_info("entry preflight passed: %zu functions, %zu syscalls, %zu callbacks; no patches written\n",
		ARRAY_SIZE(hooks) + ARRAY_SIZE(proc_hooks), ARRAY_SIZE(ipc_syscalls), ARRAY_SIZE(callback_hooks));
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
