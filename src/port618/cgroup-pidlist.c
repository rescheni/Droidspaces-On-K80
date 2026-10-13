// SPDX-License-Identifier: GPL-2.0-only
#include "kernel/cgroup/cgroup-internal.h"

#include <linux/ctype.h>
#include <linux/kmod.h>
#include <linux/sort.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/delayacct.h>
#include <linux/pid_namespace.h>
#include <linux/cgroupstats.h>
#include <linux/fs_parser.h>

#include <trace/events/cgroup.h>
#include <trace/hooks/cgroup.h>

enum cgroup_filetype {
	CGROUP_FILE_PROCS,
	CGROUP_FILE_TASKS,
};

/*
 * A pidlist is a list of pids that virtually represents the contents of one
 * of the cgroup files ("procs" or "tasks"). We keep a list of such pidlists,
 * a pair (one each for procs, tasks) for each pid namespace that's relevant
 * to the cgroup.
 */
struct cgroup_pidlist {
	/*
	 * used to find which pidlist is wanted. doesn't change as long as
	 * this particular list stays in the list.
	*/
	struct { enum cgroup_filetype type; struct pid_namespace *ns; } key;
	/* array of xids */
	pid_t *list;
	/* how many elements the above list has */
	int length;
	/* each of these stored in a list by its cgroup */
	struct list_head links;
	/* pointer to the cgroup we belong to, for list removal purposes */
	struct cgroup *owner;
	/* for delayed destruction */
	struct delayed_work destroy_dwork;
};

/* Release the list and its namespace after the native cache delay. */
static void cgroup_pidlist_destroy_work_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct cgroup_pidlist *l = container_of(dwork, struct cgroup_pidlist,
						destroy_dwork);
	struct cgroup_pidlist *tofree = NULL;

	mutex_lock(&l->owner->pidlist_mutex);

	/*
	 * Destroy iff we didn't get queued again.  The state won't change
	 * as destroy_dwork can only be queued while locked.
	 */
	if (!delayed_work_pending(dwork)) {
		list_del(&l->links);
		kvfree(l->list);
		put_pid_ns(l->key.ns);
		tofree = l;
	}

	mutex_unlock(&l->owner->pidlist_mutex);
	kfree(tofree);
}

/*
 * pidlist_uniq - given a kmalloc()ed list, strip out all duplicate entries
 * Returns the number of unique elements.
 */
static int pidlist_uniq(pid_t *list, int length)
{
	int src, dest = 1;

	/*
	 * we presume the 0th element is unique, so i starts at 1. trivial
	 * edge cases first; no work needs to be done for either
	 */
	if (length == 0 || length == 1)
		return length;
	/* src and dest walk down the list; dest counts unique elements */
	for (src = 1; src < length; src++) {
		/* find next unique element */
		while (list[src] == list[src-1]) {
			src++;
			if (src == length)
				goto after;
		}
		/* dest always points to where the next unique element goes */
		list[dest] = list[src];
		dest++;
	}
after:
	return dest;
}

/*
 * The two pid files - task and cgroup.procs - guaranteed that the result
 * is sorted, which forced this whole pidlist fiasco.  As pid order is
 * different per namespace, each namespace needs differently sorted list,
 * making it impossible to use, for example, single rbtree of member tasks
 * sorted by task pointer.  As pidlists can be fairly large, allocating one
 * per open file is dangerous, so cgroup had to implement shared pool of
 * pidlists keyed by cgroup and namespace.
 */
static int cmppid(const void *a, const void *b)
{
	return *(pid_t *)a - *(pid_t *)b;
}

static struct cgroup_pidlist *cgroup_pidlist_find(struct cgroup *cgrp,
						  enum cgroup_filetype type)
{
	struct cgroup_pidlist *l;
	/* don't need task_nsproxy() if we're looking at ourself */
	struct pid_namespace *ns = task_active_pid_ns(current);

	lockdep_assert_held(&cgrp->pidlist_mutex);

	list_for_each_entry(l, &cgrp->pidlists, links)
		if (l->key.type == type && l->key.ns == ns)
			return l;
	return NULL;
}

/*
 * find the appropriate pidlist for our purpose (given procs vs tasks)
 * returns with the lock on that pidlist already held, and takes care
 * of the use count, or returns NULL with no locks held if we're out of
 * memory.
 */
static struct cgroup_pidlist *cgroup_pidlist_find_create(struct cgroup *cgrp,
						enum cgroup_filetype type)
{
	struct cgroup_pidlist *l;

	lockdep_assert_held(&cgrp->pidlist_mutex);

	l = cgroup_pidlist_find(cgrp, type);
	if (l)
		return l;

	/* entry not found; create a new one */
	l = kzalloc(sizeof(struct cgroup_pidlist), GFP_KERNEL);
	if (!l)
		return l;

	INIT_DELAYED_WORK(&l->destroy_dwork, cgroup_pidlist_destroy_work_fn);
	l->key.type = type;
	/* don't need task_nsproxy() if we're looking at ourself */
	l->key.ns = get_pid_ns(task_active_pid_ns(current));
	l->owner = cgrp;
	list_add(&l->links, &cgrp->pidlists);
	return l;
}

/*
 * Load a cgroup's pidarray with either procs' tgids or tasks' pids
 */
static int pidlist_array_load(struct cgroup *cgrp, enum cgroup_filetype type,
			      struct cgroup_pidlist **lp)
{
	pid_t *array;
	int length;
	int pid, n = 0; /* used for populating the array */
	struct css_task_iter it;
	struct task_struct *tsk;
	struct cgroup_pidlist *l;

	lockdep_assert_held(&cgrp->pidlist_mutex);

	/*
	 * If cgroup gets more users after we read count, we won't have
	 * enough space - tough.  This race is indistinguishable to the
	 * caller from the case that the additional cgroup users didn't
	 * show up until sometime later on.
	 */
	length = cgroup_task_count(cgrp);
	array = kvmalloc_array(length, sizeof(pid_t), GFP_KERNEL);
	if (!array)
		return -ENOMEM;
	/* now, populate the array */
	css_task_iter_start(&cgrp->self, 0, &it);
	while ((tsk = css_task_iter_next(&it))) {
		if (unlikely(n == length))
			break;
		/* get tgid or pid for procs or tasks file respectively */
		if (type == CGROUP_FILE_PROCS)
			pid = task_tgid_vnr(tsk);
		else
			pid = task_pid_vnr(tsk);
		if (pid > 0) /* make sure to only use valid results */
			array[n++] = pid;
	}
	css_task_iter_end(&it);
	length = n;
	/* now sort & strip out duplicates (tgids or recycled thread PIDs) */
	sort(array, length, sizeof(pid_t), cmppid, NULL);
	length = pidlist_uniq(array, length);

	l = cgroup_pidlist_find_create(cgrp, type);
	if (!l) {
		kvfree(array);
		return -ENOMEM;
	}

	/* store array, freeing old if necessary */
	kvfree(l->list);
	l->list = array;
	l->length = length;
	*lp = l;
	return 0;
}

/*
 * seq_file methods for the tasks/procs files. The seq_file position is the
 * next pid to display; the seq_file iterator is a pointer to the pid
 * in the cgroup->l->list array.
 */

void *ds_cgroup_pidlist_start(struct seq_file *s, loff_t *pos);
void *ds_cgroup_pidlist_start(struct seq_file *s, loff_t *pos)
{
	/*
	 * Initially we receive a position value that corresponds to
	 * one more than the last pid shown (or 0 on the first call or
	 * after a seek to the start). Use a binary-search to find the
	 * next pid to display, if any
	 */
	struct kernfs_open_file *of = s->private;
	struct cgroup_file_ctx *ctx = of->priv;
	struct cgroup *cgrp = seq_css(s)->cgroup;
	struct cgroup_pidlist *l;
	enum cgroup_filetype type = seq_cft(s)->private;
	int index = 0, pid = *pos;
	int *iter, ret;

	mutex_lock(&cgrp->pidlist_mutex);

	/*
	 * !NULL @ctx->procs1.pidlist indicates that this isn't the first
	 * start() after open. If the matching pidlist is around, we can use
	 * that. Look for it. Note that @ctx->procs1.pidlist can't be used
	 * directly. It could already have been destroyed.
	 */
	if (ctx->procs1.pidlist)
		ctx->procs1.pidlist = cgroup_pidlist_find(cgrp, type);

	/*
	 * Either this is the first start() after open or the matching
	 * pidlist has been destroyed inbetween.  Create a new one.
	 */
	if (!ctx->procs1.pidlist) {
		ret = pidlist_array_load(cgrp, type, &ctx->procs1.pidlist);
		if (ret)
			return ERR_PTR(ret);
	}
	l = ctx->procs1.pidlist;

	if (pid) {
		int end = l->length;

		while (index < end) {
			int mid = (index + end) / 2;
			if (l->list[mid] == pid) {
				index = mid;
				break;
			} else if (l->list[mid] < pid)
				index = mid + 1;
			else
				end = mid;
		}
	}
	/* If we're off the end of the array, we're done */
	if (index >= l->length)
		return NULL;
	/* Update the abstract position to be the actual pid that we found */
	iter = l->list + index;
	*pos = *iter;
	return iter;
}
