// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Andrei Vagin <avagin@openvz.org>
 * Author: Dmitry Safonov <dima@arista.com>
 */

#include <linux/time_namespace.h>
#include <linux/user_namespace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/clocksource.h>
#include <linux/seq_file.h>
#include <linux/proc_ns.h>
#include <linux/export.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/nsproxy.h>

#include <vdso/datapage.h>

static struct ucounts *inc_time_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_TIME_NAMESPACES);
}

static void dec_time_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_TIME_NAMESPACES);
}

/**
 * clone_time_ns - Clone a time namespace
 * @user_ns:	User namespace which owns a new namespace.
 * @old_ns:	Namespace to clone
 *
 * Clone @old_ns and set the clone refcount to 1
 *
 * Return: The new namespace or ERR_PTR.
 */
static struct time_namespace *clone_time_ns(struct user_namespace *user_ns,
					  struct time_namespace *old_ns)
{
	struct time_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	ucounts = inc_time_namespaces(user_ns);
	if (!ucounts)
		goto fail;

	err = -ENOMEM;
	ns = kmalloc(sizeof(*ns), GFP_KERNEL_ACCOUNT);
	if (!ns)
		goto fail_dec;

	refcount_set(&ns->ns.count, 1);

	ns->vvar_page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!ns->vvar_page)
		goto fail_free;

	err = ns_alloc_inum(&ns->ns);
	if (err)
		goto fail_free_page;

	ns->ucounts = ucounts;
	ns->ns.ops = &timens_operations;
	ns->user_ns = get_user_ns(user_ns);
	ns->offsets = old_ns->offsets;
	ns->frozen_offsets = false;
	return ns;

fail_free_page:
	__free_page(ns->vvar_page);
fail_free:
	kfree(ns);
fail_dec:
	dec_time_namespaces(ucounts);
fail:
	return ERR_PTR(err);
}

/**
 * copy_time_ns - Create timens_for_children from @old_ns
 * @flags:	Cloning flags
 * @user_ns:	User namespace which owns a new namespace.
 * @old_ns:	Namespace to clone
 *
 * If CLONE_NEWTIME specified in @flags, creates a new timens_for_children;
 * adds a refcounter to @old_ns otherwise.
 *
 * Return: timens_for_children namespace or ERR_PTR.
 */
struct time_namespace *ds_userns_copy_time_ns(unsigned long flags,
	struct user_namespace *user_ns, struct time_namespace *old_ns)
{
	if (!(flags & CLONE_NEWTIME))
		return get_time_ns(old_ns);

	return clone_time_ns(user_ns, old_ns);
}

void ds_userns_free_time_ns(struct time_namespace *ns)
{
	dec_time_namespaces(ns->ucounts);
	put_user_ns(ns->user_ns);
	ns_free_inum(&ns->ns);
	__free_page(ns->vvar_page);
	kfree(ns);
}

static struct time_namespace *to_time_ns(struct ns_common *ns)
{
	return container_of(ns, struct time_namespace, ns);
}

void ds_userns_timens_put(struct ns_common *ns)
{
	put_time_ns(to_time_ns(ns));
}

int ds_userns_timens_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct time_namespace *ns = to_time_ns(new);

	if (!current_is_single_threaded())
		return -EUSERS;

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	get_time_ns(ns);
	put_time_ns(nsproxy->time_ns);
	nsproxy->time_ns = ns;

	get_time_ns(ns);
	put_time_ns(nsproxy->time_ns_for_children);
	nsproxy->time_ns_for_children = ns;
	return 0;
}

void ds_userns_timens_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk)
{
	struct ns_common *nsc = &nsproxy->time_ns_for_children->ns;
	struct time_namespace *ns = to_time_ns(nsc);

	/* create_new_namespaces() already incremented the ref counter */
	if (nsproxy->time_ns == nsproxy->time_ns_for_children)
		return;

	get_time_ns(ns);
	put_time_ns(nsproxy->time_ns);
	nsproxy->time_ns = ns;

	timens_commit(tsk, ns);
}
