// SPDX-License-Identifier: GPL-2.0
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pid.h>
#include <linux/pidfs.h>
#include <linux/pid_namespace.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/pseudo_fs.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <uapi/linux/pidfd.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/utsname.h>
#include <net/net_namespace.h>

#include "fs/internal.h"
#include "mount.h"

long ds_pidfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct task_struct *task __free(put_task) = NULL;
	struct nsproxy *nsp __free(put_nsproxy) = NULL;
	struct pid *pid = pidfd_pid(file);
	struct ns_common *ns_common = NULL;
	struct pid_namespace *pid_ns;

	if (arg)
		return -EINVAL;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	scoped_guard(task_lock, task) {
		nsp = task->nsproxy;
		if (nsp)
			get_nsproxy(nsp);
	}
	if (!nsp)
		return -ESRCH; /* just pretend it didn't exist */

	/*
	 * We're trying to open a file descriptor to the namespace so perform a
	 * filesystem cred ptrace check. Also, we mirror nsfs behavior.
	 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS))
		return -EACCES;

	switch (cmd) {
	/* Namespaces that hang of nsproxy. */
	case PIDFD_GET_CGROUP_NAMESPACE:
		if (IS_ENABLED(CONFIG_CGROUPS)) {
			get_cgroup_ns(nsp->cgroup_ns);
			ns_common = to_ns_common(nsp->cgroup_ns);
		}
		break;
	case PIDFD_GET_IPC_NAMESPACE:
		if (IS_ENABLED(CONFIG_IPC_NS)) {
			ns_common = to_ns_common(get_ipc_ns(nsp->ipc_ns ?
						 nsp->ipc_ns : &init_ipc_ns));
		}
		break;
	case PIDFD_GET_MNT_NAMESPACE:
		get_mnt_ns(nsp->mnt_ns);
		ns_common = to_ns_common(nsp->mnt_ns);
		break;
	case PIDFD_GET_NET_NAMESPACE:
		if (IS_ENABLED(CONFIG_NET_NS)) {
			ns_common = to_ns_common(nsp->net_ns);
			get_net_ns(ns_common);
		}
		break;
	case PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE:
		if (IS_ENABLED(CONFIG_PID_NS)) {
			get_pid_ns(nsp->pid_ns_for_children);
			ns_common = to_ns_common(nsp->pid_ns_for_children);
		}
		break;
	case PIDFD_GET_TIME_NAMESPACE:
		if (IS_ENABLED(CONFIG_TIME_NS)) {
			get_time_ns(nsp->time_ns);
			ns_common = to_ns_common(nsp->time_ns);
		}
		break;
	case PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE:
		if (IS_ENABLED(CONFIG_TIME_NS)) {
			get_time_ns(nsp->time_ns_for_children);
			ns_common = to_ns_common(nsp->time_ns_for_children);
		}
		break;
	case PIDFD_GET_UTS_NAMESPACE:
		if (IS_ENABLED(CONFIG_UTS_NS)) {
			get_uts_ns(nsp->uts_ns);
			ns_common = to_ns_common(nsp->uts_ns);
		}
		break;
	/* Namespaces that don't hang of nsproxy. */
	case PIDFD_GET_USER_NAMESPACE:
		if (IS_ENABLED(CONFIG_USER_NS)) {
			rcu_read_lock();
			ns_common = to_ns_common(get_user_ns(task_cred_xxx(task, user_ns)));
			rcu_read_unlock();
		}
		break;
	case PIDFD_GET_PID_NAMESPACE:
		if (IS_ENABLED(CONFIG_PID_NS)) {
			rcu_read_lock();
			pid_ns = task_active_pid_ns(task);
			if (pid_ns)
				ns_common = to_ns_common(get_pid_ns(pid_ns));
			rcu_read_unlock();
		}
		break;
	default:
		return -ENOIOCTLCMD;
	}

	if (!ns_common)
		return -EOPNOTSUPP;

	/* open_namespace() unconditionally consumes the reference */
	return open_namespace(ns_common);
}
