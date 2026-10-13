// SPDX-License-Identifier: GPL-2.0-only
/* Namespace policy from kernel/pid_sysctl.h and mm/memfd.c; native allocation retained. */
#include <linux/pid_namespace.h>
#include <linux/capability.h>
#include <linux/sysctl.h>
#include <linux/memfd.h>
#include <uapi/linux/memfd.h>
#include <asm/ptrace.h>
#include "ipc-backend.h"

static int pid_mfd_noexec_dointvec_minmax(const struct ctl_table *table,
	int write, void *buf, size_t *lenp, loff_t *ppos)
{
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct ctl_table table_copy;
	int err, scope, parent_scope;

	if (write && !READ_ONCE(ds_ipc_active))
		return -EBUSY;

	if (write && !ns_capable(ns->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	table_copy = *table;

	/* You cannot set a lower enforcement value than your parent. */
	parent_scope = pidns_memfd_noexec_scope(ns->parent);
	/* Equivalent to pidns_memfd_noexec_scope(ns). */
	scope = max(READ_ONCE(ns->memfd_noexec_scope), parent_scope);

	table_copy.data = &scope;
	table_copy.extra1 = &parent_scope;

	err = proc_dointvec_minmax(&table_copy, write, buf, lenp, ppos);
	if (!err && write)
		WRITE_ONCE(ns->memfd_noexec_scope, scope);
	return err;
}

static struct ctl_table pid_ns_ctl_table_vm[] = {
	{
		.procname	= "memfd_noexec",
		.data		= &init_pid_ns.memfd_noexec_scope,
		.maxlen		= sizeof(init_pid_ns.memfd_noexec_scope),
		.mode		= 0644,
		.proc_handler	= pid_mfd_noexec_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
};

static struct ctl_table_header *memfd_header;
long (*ds_native_memfd_create)(const struct pt_regs *);

int ds_memfd_init(void);
int ds_memfd_init(void)
{
	memfd_header = register_sysctl("vm", pid_ns_ctl_table_vm);
	return memfd_header ? 0 : -ENOMEM;
}

void ds_memfd_exit(void);
void ds_memfd_exit(void)
{
	unregister_sysctl_table(memfd_header);
}

asmlinkage long __arm64_sys_ds_memfd_create(const struct pt_regs *regs);
asmlinkage long __arm64_sys_ds_memfd_create(const struct pt_regs *regs)
{
	const unsigned int allowed = MFD_CLOEXEC | MFD_ALLOW_SEALING |
		MFD_HUGETLB | MFD_NOEXEC_SEAL | MFD_EXEC;
	unsigned int flags = regs->regs[1];
	struct pt_regs adjusted;
	int scope;

	/* Preserve native validation and its error order for malformed flags. */
	if ((flags & ~(allowed | ((flags & MFD_HUGETLB) ?
		(MFD_HUGE_MASK << MFD_HUGE_SHIFT) : 0))) ||
	    ((flags & MFD_EXEC) && (flags & MFD_NOEXEC_SEAL)))
		return ds_native_memfd_create(regs);
	scope = pidns_memfd_noexec_scope(task_active_pid_ns(current));
	if (!(flags & (MFD_EXEC | MFD_NOEXEC_SEAL)))
		flags |= scope >= MEMFD_NOEXEC_SCOPE_NOEXEC_SEAL ? MFD_NOEXEC_SEAL : MFD_EXEC;
	if (!(flags & MFD_NOEXEC_SEAL) && scope >= MEMFD_NOEXEC_SCOPE_NOEXEC_ENFORCED)
		return -EACCES;
	adjusted = *regs;
	adjusted.regs[1] = flags;
	return ds_native_memfd_create(&adjusted);
}
