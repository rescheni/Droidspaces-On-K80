// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_ns: " fmt
#include <linux/ipc_namespace.h>
#include <linux/pid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/rcupdate.h>
#include "port.h"

int ds_port_selftest(void)
{
	struct pid_namespace *pid = NULL, *pid_shared = NULL, *invalid;
	struct ipc_namespace *ipc = NULL, *ipc_shared = NULL;
	int ret = -EINVAL;

	pid = copy_pid_ns(CLONE_NEWPID, &init_user_ns, &init_pid_ns);
	if (IS_ERR(pid)) {
		ret = PTR_ERR(pid);
		pid = NULL;
		goto out;
	}
	if (pid->level != 1 || pid->parent != &init_pid_ns ||
	    !pid->ns.inum || !pid->pid_cachep || !idr_is_empty(&pid->idr) ||
	    pid->pid_allocated != PIDNS_ADDING ||
	    refcount_read(&pid->ns.__ns_ref) != 1)
		goto out;
	pid_shared = copy_pid_ns(0, &init_user_ns, pid);
	if (pid_shared != pid || refcount_read(&pid->ns.__ns_ref) != 2)
		goto out;
	/* A task cannot create a PID namespace from an inactive child namespace. */
	invalid = copy_pid_ns(CLONE_NEWPID, &init_user_ns, pid);
	if (!IS_ERR(invalid)) {
		put_pid_ns(invalid);
		goto out;
	}
	if (PTR_ERR(invalid) != -EINVAL)
		goto out;
	pr_info("PID: allocation, parent, IDR, sharing and invalid-parent checks passed\n");

	ipc = copy_ipcs(CLONE_NEWIPC, &init_user_ns, &init_ipc_ns);
	if (IS_ERR(ipc)) {
		ret = PTR_ERR(ipc);
		ipc = NULL;
		goto out;
	}
	if (!ipc->ns.inum || ipc->ns.inum == pid->ns.inum ||
	    ipc->user_ns != &init_user_ns || refcount_read(&ipc->ns.__ns_ref) != 1)
		goto out;
	ipc_shared = copy_ipcs(0, &init_user_ns, ipc);
	if (ipc_shared != ipc || refcount_read(&ipc->ns.__ns_ref) != 2)
		goto out;
	pr_info("IPC: allocation, namespace identity, owner and sharing checks passed\n");
	ret = 0;
out:
	if (ipc_shared)
		put_ipc_ns(ipc_shared);
	if (ipc)
		put_ipc_ns(ipc);
	if (pid_shared)
		put_pid_ns(pid_shared);
	if (pid)
		put_pid_ns(pid);
	ds_ipc_flush();
	rcu_barrier();
	if (atomic_read(&ds_pid_live) || atomic_read(&ds_ipc_live))
		ret = -EBUSY;
	if (ret)
		pr_err("namespace core selftest failed: %d\n", ret);
	else
		pr_info("PID/IPC: deferred destruction finished, no live namespaces\n");
	return ret;
}
