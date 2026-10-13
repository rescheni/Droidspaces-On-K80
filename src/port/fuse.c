/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fs/fuse/fuse_i.h"

#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/statfs.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/exportfs.h>
#include <linux/posix_acl.h>
#include <linux/pid_namespace.h>
#include <uapi/linux/magic.h>

#define FUSE_DEFAULT_MAX_BACKGROUND 12
#define FUSE_DEFAULT_CONGESTION_THRESHOLD (FUSE_DEFAULT_MAX_BACKGROUND * 3 / 4)

static void fuse_iqueue_init(struct fuse_iqueue *fiq,
			     const struct fuse_iqueue_ops *ops,
			     void *priv)
{
	memset(fiq, 0, sizeof(struct fuse_iqueue));
	spin_lock_init(&fiq->lock);
	init_waitqueue_head(&fiq->waitq);
	INIT_LIST_HEAD(&fiq->pending);
	INIT_LIST_HEAD(&fiq->interrupts);
	fiq->forget_list_tail = &fiq->forget_list_head;
	fiq->connected = 1;
	fiq->ops = ops;
	fiq->priv = priv;
}

void ds_fuse_conn_init(struct fuse_conn *fc, struct fuse_mount *fm,
		    struct user_namespace *user_ns,
		    const struct fuse_iqueue_ops *fiq_ops, void *fiq_priv)
{
	memset(fc, 0, sizeof(*fc));
	spin_lock_init(&fc->lock);
	spin_lock_init(&fc->bg_lock);
	init_rwsem(&fc->killsb);
	refcount_set(&fc->count, 1);
	atomic_set(&fc->dev_count, 1);
	init_waitqueue_head(&fc->blocked_waitq);
	fuse_iqueue_init(&fc->iq, fiq_ops, fiq_priv);
	INIT_LIST_HEAD(&fc->bg_queue);
	INIT_LIST_HEAD(&fc->entry);
	INIT_LIST_HEAD(&fc->devices);
	atomic_set(&fc->num_waiting, 0);
	fc->max_background = FUSE_DEFAULT_MAX_BACKGROUND;
	fc->congestion_threshold = FUSE_DEFAULT_CONGESTION_THRESHOLD;
	atomic64_set(&fc->khctr, 0);
	fc->polled_files = RB_ROOT;
	fc->blocked = 0;
	fc->initialized = 0;
	fc->connected = 1;
	atomic64_set(&fc->attr_version, 1);
	get_random_bytes(&fc->scramble_key, sizeof(fc->scramble_key));
	fc->pid_ns = get_pid_ns(task_active_pid_ns(current));
	/* Native !USER_NS callers can omit x2 entirely: it is a dead argument
	 * in the host implementation. Preserve their initial-namespace contract.
	 * The ported fuse_get_tree explicitly installs the retained owner for
	 * child connections after this initializer returns.
	 */
#ifdef DS_USERNS_LIVE
	fc->user_ns = get_user_ns(&init_user_ns);
#else
	fc->user_ns = get_user_ns(user_ns);
#endif
	fc->max_pages = FUSE_DEFAULT_MAX_PAGES_PER_REQ;
	fc->max_pages_limit = FUSE_MAX_MAX_PAGES;

	if (IS_ENABLED(CONFIG_FUSE_PASSTHROUGH))
		fuse_backing_files_init(fc);

	INIT_LIST_HEAD(&fc->mounts);
	list_add(&fm->fc_entry, &fc->mounts);
	fm->fc = fc;
}

static void delayed_release(struct rcu_head *p)
{
	struct fuse_conn *fc = container_of(p, struct fuse_conn, rcu);

	put_user_ns(fc->user_ns);
	fc->release(fc);
}

void ds_fuse_conn_put(struct fuse_conn *fc)
{
	if (refcount_dec_and_test(&fc->count)) {
		struct fuse_iqueue *fiq = &fc->iq;
		struct fuse_sync_bucket *bucket;

		if (IS_ENABLED(CONFIG_FUSE_DAX))
			fuse_dax_conn_free(fc);
		if (fiq->ops->release)
			fiq->ops->release(fiq);
		put_pid_ns(fc->pid_ns);
		bucket = rcu_dereference_protected(fc->curr_bucket, 1);
		if (bucket) {
			WARN_ON(atomic_read(&bucket->count) != 1);
			kfree(bucket);
		}
		if (IS_ENABLED(CONFIG_FUSE_PASSTHROUGH))
			fuse_backing_files_free(fc);
		call_rcu(&fc->rcu, delayed_release);
	}
}

