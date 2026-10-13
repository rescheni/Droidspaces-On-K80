/* SPDX-License-Identifier: GPL-2.0-only */
/* FUSE user-namespace paths from the pinned android16-6.12 DDK.
 * Copyright (C) 2001-2008 Miklos Szeredi <miklos@szeredi.hu>
 */
#include "fs/fuse/fuse_i.h"
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include "live-fuse.h"

#include <linux/sched/signal.h>
extern struct kmem_cache *fuse_req_cachep;
extern void fuse_put_request(struct fuse_req *);
#define fuse_get_req ds_userns_fuse_get_req
static void fuse_request_init(struct fuse_mount *fm, struct fuse_req *req)
{
	INIT_LIST_HEAD(&req->list);
	INIT_LIST_HEAD(&req->intr_entry);
	init_waitqueue_head(&req->waitq);
	refcount_set(&req->count, 1);
	__set_bit(FR_PENDING, &req->flags);
	req->fm = fm;
}


static struct fuse_req *fuse_request_alloc(struct fuse_mount *fm, gfp_t flags)
{
	struct fuse_req *req = kmem_cache_zalloc(fuse_req_cachep, flags);
	if (req)
		fuse_request_init(fm, req);

	return req;
}


static bool fuse_block_alloc(struct fuse_conn *fc, bool for_background)
{
	return !fc->initialized || (for_background && fc->blocked);
}


static void fuse_drop_waiting(struct fuse_conn *fc)
{
	/*
	 * lockess check of fc->connected is okay, because atomic_dec_and_test()
	 * provides a memory barrier matched with the one in fuse_wait_aborted()
	 * to ensure no wake-up is missed.
	 */
	if (atomic_dec_and_test(&fc->num_waiting) &&
	    !READ_ONCE(fc->connected)) {
		/* wake up aborters */
		wake_up_all(&fc->blocked_waitq);
	}
}


struct fuse_req *fuse_get_req(struct mnt_idmap *idmap,
				     struct fuse_mount *fm,
				     bool for_background)
{
	struct fuse_conn *fc = fm->fc;
	struct fuse_req *req;
	bool no_idmap = !fm->sb || (fm->sb->s_iflags & SB_I_NOIDMAP);
	kuid_t fsuid;
	kgid_t fsgid;
	int err;

	atomic_inc(&fc->num_waiting);

	if (fuse_block_alloc(fc, for_background)) {
		err = -EINTR;
		if (wait_event_killable_exclusive(fc->blocked_waitq,
				!fuse_block_alloc(fc, for_background)))
			goto out;
	}
	/* Matches smp_wmb() in fuse_set_initialized() */
	smp_rmb();

	err = -ENOTCONN;
	if (!fc->connected)
		goto out;

	err = -ECONNREFUSED;
	if (fc->conn_error)
		goto out;

	req = fuse_request_alloc(fm, GFP_KERNEL);
	err = -ENOMEM;
	if (!req) {
		if (for_background)
			wake_up(&fc->blocked_waitq);
		goto out;
	}

	req->in.h.pid = pid_nr_ns(task_pid(current), fc->pid_ns);

	__set_bit(FR_WAITING, &req->flags);
	if (for_background)
		__set_bit(FR_BACKGROUND, &req->flags);

	/*
	 * Keep the old behavior when idmappings support was not
	 * declared by a FUSE server.
	 *
	 * For those FUSE servers who support idmapped mounts,
	 * we send UID/GID only along with "inode creation"
	 * fuse requests, otherwise idmap == &invalid_mnt_idmap and
	 * req->in.h.{u,g}id will be equal to FUSE_INVALID_UIDGID.
	 */
	fsuid = no_idmap ? current_fsuid() : mapped_fsuid(idmap, fc->user_ns);
	fsgid = no_idmap ? current_fsgid() : mapped_fsgid(idmap, fc->user_ns);
	req->in.h.uid = from_kuid(fc->user_ns, fsuid);
	req->in.h.gid = from_kgid(fc->user_ns, fsgid);

	if (no_idmap && unlikely(req->in.h.uid == ((uid_t)-1) ||
				 req->in.h.gid == ((gid_t)-1))) {
		fuse_put_request(req);
		return ERR_PTR(-EOVERFLOW);
	}

	return req;

 out:
	fuse_drop_waiting(fc);
	return ERR_PTR(err);
}

/* Native forced requests inline !USER_NS credential conversion.  Child
 * connections use private queue operations so this conversion is performed
 * exactly once, after construction and before handing the request to FUSE.
 * Normal requests were already mapped by ds_userns_fuse_get_req(). */
extern void process_init_reply(struct fuse_mount *, struct fuse_args *, int);
static void ds_fuse_init_reply(struct fuse_mount *fm, struct fuse_args *args,
			      int error)
{
	struct fuse_init_out *out = args->out_args[0].value;
	u64 flags = out->flags | (u64)out->flags2 << 32;

	flags &= ~(FUSE_ALLOW_IDMAP | FUSE_PASSTHROUGH);
	out->flags = flags;
	out->flags2 = flags >> 32;
	process_init_reply(fm, args, error);
}

static void ds_fuse_queue_req(struct fuse_iqueue *fiq, struct fuse_req *req,
			      bool sync)
{
	struct fuse_conn *fc = req->fm->fc;
	struct fuse_args *args = req->args;

	if (test_bit(FR_FORCE, &req->flags) && !args->nocreds) {
		req->in.h.uid = from_kuid_munged(fc->user_ns,
					      KUIDT_INIT(req->in.h.uid));
		req->in.h.gid = from_kgid_munged(fc->user_ns,
					      KGIDT_INIT(req->in.h.gid));
	}
	if (args->opcode == FUSE_INIT) {
		struct fuse_init_in *in = (void *)args->in_args[0].value;
		u64 flags = in->flags | (u64)in->flags2 << 32;

		flags &= ~(FUSE_ALLOW_IDMAP | FUSE_PASSTHROUGH);
		in->flags = flags;
		in->flags2 = flags >> 32;
		args->end = ds_fuse_init_reply;
	}
	fuse_dev_fiq_ops.send_req(fiq, req, sync);
}

static void ds_fuse_queue_forget(struct fuse_iqueue *fiq,
				 struct fuse_forget_link *forget, bool sync)
{
	fuse_dev_fiq_ops.send_forget(fiq, forget, sync);
}

static void ds_fuse_queue_interrupt(struct fuse_iqueue *fiq,
				    struct fuse_req *req, bool sync)
{
	fuse_dev_fiq_ops.send_interrupt(fiq, req, sync);
}

const struct fuse_iqueue_ops ds_userns_fuse_iq_ops = {
	.send_req = ds_fuse_queue_req,
	.send_forget = ds_fuse_queue_forget,
	.send_interrupt = ds_fuse_queue_interrupt,
};

extern ssize_t ds_userns_fuse_simple_request_original(struct mnt_idmap *,
					struct fuse_mount *, struct fuse_args *);
ssize_t ds_userns_fuse_simple_request(struct mnt_idmap *idmap,
				      struct fuse_mount *fm, struct fuse_args *args)
{
	unsigned int saved_size = 0;
	bool plain_lookup = false;
	ssize_t ret;

	if (fm->fc->user_ns != &init_user_ns) {
		if (args->opcode == FUSE_CANONICAL_PATH)
			return -EOPNOTSUPP;
		/* Do not accept Android's extra backing-file/BPF reply payload.
		 * The native copy-out path rejects oversized replies before it
		 * resolves any supplied backing fd. Plain LOOKUP remains valid. */
		plain_lookup = args->opcode == FUSE_LOOKUP && args->out_numargs == 2;
		if (plain_lookup) {
			saved_size = args->out_args[1].size;
			args->out_args[1].size = 0;
		}
	}
	ret = ds_userns_fuse_simple_request_original(idmap, fm, args);
	if (plain_lookup)
		args->out_args[1].size = saved_size;
	return ret;
}
