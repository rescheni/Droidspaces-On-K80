/* SPDX-License-Identifier: GPL-2.0-only */
/* FUSE user-namespace paths from the pinned android16-6.12 DDK.
 * Copyright (C) 2001-2008 Miklos Szeredi <miklos@szeredi.hu>
 */
#include "fs/fuse/fuse_i.h"
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include "live-fuse.h"

#include <linux/pagemap.h>
#include <linux/security.h>
#include <linux/namei.h>
#include <linux/posix_acl.h>
#include "fs/internal.h"
extern bool allow_sys_admin_access;
/* Native fuse_do_getattr drops idmap and shifts the remaining registers.
 * Use a private port with an explicit C ABI; never call that local symbol. */
static int fuse_do_getattr(struct mnt_idmap *idmap, struct inode *inode,
			   struct kstat *stat, struct file *file)
{
	int err;
	struct fuse_getattr_in inarg;
	struct fuse_attr_out outarg;
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	u64 attr_version;

	attr_version = fuse_get_attr_version(fm->fc);

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));
	/* Directories have separate file-handle space */
	if (file && S_ISREG(inode->i_mode)) {
		struct fuse_file *ff = file->private_data;

		inarg.getattr_flags |= FUSE_GETATTR_FH;
		inarg.fh = ff->fh;
	}
	args.opcode = FUSE_GETATTR;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(outarg);
	args.out_args[0].value = &outarg;
	err = fuse_simple_request(fm, &args);
	if (!err)
		err = finalize_attr(inode, &outarg, attr_version, stat);
	return err;
}

#define fuse_permission ds_userns_fuse_permission
#define fuse_setattr ds_userns_fuse_setattr
#define fuse_getattr ds_userns_fuse_getattr
#define fuse_update_get_attr ds_userns_fuse_update_get_attr
#define fuse_fillattr ds_userns_fuse_fillattr

#define fuse_allow_current_process ds_userns_fuse_allow_current_process
#define fuse_do_setattr ds_userns_fuse_do_setattr
#define get_create_ext ds_userns_fuse_get_create_ext
static inline bool fuse_permissible_uidgid(struct fuse_conn *fc)
{
	const struct cred *cred = current_cred();

	return (uid_eq(cred->euid, fc->user_id) &&
		uid_eq(cred->suid, fc->user_id) &&
		uid_eq(cred->uid,  fc->user_id) &&
		gid_eq(cred->egid, fc->group_id) &&
		gid_eq(cred->sgid, fc->group_id) &&
		gid_eq(cred->gid,  fc->group_id));
}


bool fuse_allow_current_process(struct fuse_conn *fc)
{
	bool allow;

	if (fc->allow_other)
		allow = current_in_userns(fc->user_ns);
	else
		allow = fuse_permissible_uidgid(fc);

	if (!allow && allow_sys_admin_access && capable(CAP_SYS_ADMIN))
		allow = true;

	return allow;
}


static void __fuse_release_nowrite(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	BUG_ON(fi->writectr != FUSE_NOWRITE);
	fi->writectr = 0;
	fuse_flush_writepages(inode);
}


static void fuse_setattr_fill(struct fuse_conn *fc, struct fuse_args *args,
			      struct inode *inode,
			      struct fuse_setattr_in *inarg_p,
			      struct fuse_attr_out *outarg_p)
{
	args->opcode = FUSE_SETATTR;
	args->nodeid = get_node_id(inode);
	args->in_numargs = 1;
	args->in_args[0].size = sizeof(*inarg_p);
	args->in_args[0].value = inarg_p;
	args->out_numargs = 1;
	args->out_args[0].size = sizeof(*outarg_p);
	args->out_args[0].value = outarg_p;
}


int fuse_do_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		    struct iattr *attr, struct file *file)
{
	struct inode *inode = d_inode(dentry);
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_conn *fc = fm->fc;
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct address_space *mapping = inode->i_mapping;
	FUSE_ARGS(args);
	struct fuse_setattr_in inarg;
	struct fuse_attr_out outarg;
	bool is_truncate = false;
	bool is_wb = fc->writeback_cache && S_ISREG(inode->i_mode);
	loff_t oldsize;
	int err;
	bool trust_local_cmtime = is_wb;
	bool fault_blocked = false;
	u64 attr_version;

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_setattr_io,
			       fuse_setattr_initialize, fuse_setattr_backing,
			       fuse_setattr_finalize, dentry, attr, file);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	if (!fc->default_permissions)
		attr->ia_valid |= ATTR_FORCE;

	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	if (attr->ia_valid & ATTR_SIZE) {
		if (WARN_ON(!S_ISREG(inode->i_mode)))
			return -EIO;
		is_truncate = true;
	}

	if (FUSE_IS_DAX(inode) && is_truncate) {
		filemap_invalidate_lock(mapping);
		fault_blocked = true;
		err = fuse_dax_break_layouts(inode, 0, -1);
		if (err) {
			filemap_invalidate_unlock(mapping);
			return err;
		}
	}

	if (attr->ia_valid & ATTR_OPEN) {
		/* This is coming from open(..., ... | O_TRUNC); */
		WARN_ON(!(attr->ia_valid & ATTR_SIZE));
		WARN_ON(attr->ia_size != 0);
		if (fc->atomic_o_trunc) {
			/*
			 * No need to send request to userspace, since actual
			 * truncation has already been done by OPEN.  But still
			 * need to truncate page cache.
			 */
			i_size_write(inode, 0);
			truncate_pagecache(inode, 0);
			goto out;
		}
		file = NULL;
	}

	/* Flush dirty data/metadata before non-truncate SETATTR */
	if (is_wb &&
	    attr->ia_valid &
			(ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_MTIME_SET |
			 ATTR_TIMES_SET)) {
		err = write_inode_now(inode, true);
		if (err)
			return err;

		fuse_set_nowrite(inode);
		fuse_release_nowrite(inode);
	}

	if (is_truncate) {
		fuse_set_nowrite(inode);
		set_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);
		if (trust_local_cmtime && attr->ia_size != inode->i_size)
			attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;
	}

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));
	iattr_to_fattr(idmap, fc, attr, &inarg, trust_local_cmtime);
	if (file) {
		struct fuse_file *ff = file->private_data;
		inarg.valid |= FATTR_FH;
		inarg.fh = ff->fh;
	}

	/* Kill suid/sgid for non-directory chown unconditionally */
	if (fc->handle_killpriv_v2 && !S_ISDIR(inode->i_mode) &&
	    attr->ia_valid & (ATTR_UID | ATTR_GID))
		inarg.valid |= FATTR_KILL_SUIDGID;

	if (attr->ia_valid & ATTR_SIZE) {
		/* For mandatory locking in truncate */
		inarg.valid |= FATTR_LOCKOWNER;
		inarg.lock_owner = fuse_lock_owner_id(fc, current->files);

		/* Kill suid/sgid for truncate only if no CAP_FSETID */
		if (fc->handle_killpriv_v2 && !capable(CAP_FSETID))
			inarg.valid |= FATTR_KILL_SUIDGID;
	}

	attr_version = fuse_get_attr_version(fm->fc);
	fuse_setattr_fill(fc, &args, inode, &inarg, &outarg);
	err = fuse_simple_request(fm, &args);
	if (err) {
		if (err == -EINTR)
			fuse_invalidate_attr(inode);
		goto error;
	}

	if (fuse_invalid_attr(&outarg.attr) ||
	    inode_wrong_type(inode, outarg.attr.mode)) {
		fuse_make_bad(inode);
		err = -EIO;
		goto error;
	}

	spin_lock(&fi->lock);
	/* the kernel maintains i_mtime locally */
	if (trust_local_cmtime) {
		if (attr->ia_valid & ATTR_MTIME)
			inode_set_mtime_to_ts(inode, attr->ia_mtime);
		if (attr->ia_valid & ATTR_CTIME)
			inode_set_ctime_to_ts(inode, attr->ia_ctime);
		/* FIXME: clear I_DIRTY_SYNC? */
	}

	if (fi->attr_version > attr_version) {
		/*
		 * Apply attributes, for example for fsnotify_change(), but set
		 * attribute timeout to zero.
		 */
		outarg.attr_valid = outarg.attr_valid_nsec = 0;
	}

	fuse_change_attributes_common(inode, &outarg.attr, NULL,
				      ATTR_TIMEOUT(&outarg),
				      fuse_get_cache_mask(inode));
	oldsize = inode->i_size;
	/* see the comment in fuse_change_attributes() */
	if (!is_wb || is_truncate)
		i_size_write(inode, outarg.attr.size);

	if (is_truncate) {
		/* NOTE: this may release/reacquire fi->lock */
		__fuse_release_nowrite(inode);
	}
	spin_unlock(&fi->lock);

	/*
	 * Only call invalidate_inode_pages2() after removing
	 * FUSE_NOWRITE, otherwise fuse_launder_folio() would deadlock.
	 */
	if ((is_truncate || !is_wb) &&
	    S_ISREG(inode->i_mode) && oldsize != outarg.attr.size) {
		truncate_pagecache(inode, outarg.attr.size);
		invalidate_inode_pages2(mapping);
	}

	clear_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);
out:
	if (fault_blocked)
		filemap_invalidate_unlock(mapping);

	return 0;

error:
	if (is_truncate)
		fuse_release_nowrite(inode);

	clear_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);

	if (fault_blocked)
		filemap_invalidate_unlock(mapping);
	return err;
}


static int get_security_context(struct dentry *entry, umode_t mode,
				struct fuse_in_arg *ext)
{
	struct fuse_secctx *fctx;
	struct fuse_secctx_header *header;
	void *ctx = NULL, *ptr;
	u32 ctxlen, total_len = sizeof(*header);
	int err, nr_ctx = 0;
	const char *name;
	size_t namelen;

	err = security_dentry_init_security(entry, mode, &entry->d_name,
					    &name, &ctx, &ctxlen);
	if (err) {
		if (err != -EOPNOTSUPP)
			goto out_err;
		/* No LSM is supporting this security hook. Ignore error */
		ctxlen = 0;
		ctx = NULL;
	}

	if (ctxlen) {
		nr_ctx = 1;
		namelen = strlen(name) + 1;
		err = -EIO;
		if (WARN_ON(namelen > XATTR_NAME_MAX + 1 || ctxlen > S32_MAX))
			goto out_err;
		total_len += FUSE_REC_ALIGN(sizeof(*fctx) + namelen + ctxlen);
	}

	err = -ENOMEM;
	header = ptr = kzalloc(total_len, GFP_KERNEL);
	if (!ptr)
		goto out_err;

	header->nr_secctx = nr_ctx;
	header->size = total_len;
	ptr += sizeof(*header);
	if (nr_ctx) {
		fctx = ptr;
		fctx->size = ctxlen;
		ptr += sizeof(*fctx);

		strcpy(ptr, name);
		ptr += namelen;

		memcpy(ptr, ctx, ctxlen);
	}
	ext->size = total_len;
	ext->value = header;
	err = 0;
out_err:
	kfree(ctx);
	return err;
}


static void *extend_arg(struct fuse_in_arg *buf, u32 bytes)
{
	void *p;
	u32 newlen = buf->size + bytes;

	p = krealloc(buf->value, newlen, GFP_KERNEL);
	if (!p) {
		kfree(buf->value);
		buf->size = 0;
		buf->value = NULL;
		return NULL;
	}

	memset(p + buf->size, 0, bytes);
	buf->value = p;
	buf->size = newlen;

	return p + newlen - bytes;
}


static u32 fuse_ext_size(size_t size)
{
	return FUSE_REC_ALIGN(sizeof(struct fuse_ext_header) + size);
}


static int get_create_supp_group(struct mnt_idmap *idmap,
				 struct inode *dir,
				 struct fuse_in_arg *ext)
{
	struct fuse_conn *fc = get_fuse_conn(dir);
	struct fuse_ext_header *xh;
	struct fuse_supp_groups *sg;
	kgid_t kgid = dir->i_gid;
	vfsgid_t vfsgid = make_vfsgid(idmap, fc->user_ns, kgid);
	gid_t parent_gid = from_kgid(fc->user_ns, kgid);

	u32 sg_len = fuse_ext_size(sizeof(*sg) + sizeof(sg->groups[0]));

	if (parent_gid == (gid_t) -1 || vfsgid_eq_kgid(vfsgid, current_fsgid()) ||
	    !vfsgid_in_group_p(vfsgid))
		return 0;

	xh = extend_arg(ext, sg_len);
	if (!xh)
		return -ENOMEM;

	xh->size = sg_len;
	xh->type = FUSE_EXT_GROUPS;

	sg = (struct fuse_supp_groups *) &xh[1];
	sg->nr_groups = 1;
	sg->groups[0] = parent_gid;

	return 0;
}


int get_create_ext(struct mnt_idmap *idmap,
			  struct fuse_args *args,
			  struct inode *dir, struct dentry *dentry,
			  umode_t mode)
{
	struct fuse_conn *fc = get_fuse_conn_super(dentry->d_sb);
	struct fuse_in_arg ext = { .size = 0, .value = NULL };
	int err = 0;

	if (fc->init_security)
		err = get_security_context(dentry, mode, &ext);
	if (!err && fc->create_supp_group)
		err = get_create_supp_group(idmap, dir, &ext);

	if (!err && ext.size) {
		WARN_ON(args->in_numargs >= ARRAY_SIZE(args->in_args));
		args->is_ext = true;
		args->ext_idx = args->in_numargs++;
		args->in_args[args->ext_idx] = ext;
	} else {
		kfree(ext.value);
	}

	return err;
}

static void fuse_statx_to_attr(struct fuse_statx *sx, struct fuse_attr *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->ino = sx->ino;
	attr->size = sx->size;
	attr->blocks = sx->blocks;
	attr->atime = sx->atime.tv_sec;
	attr->mtime = sx->mtime.tv_sec;
	attr->ctime = sx->ctime.tv_sec;
	attr->atimensec = sx->atime.tv_nsec;
	attr->mtimensec = sx->mtime.tv_nsec;
	attr->ctimensec = sx->ctime.tv_nsec;
	attr->mode = sx->mode;
	attr->nlink = sx->nlink;
	attr->uid = sx->uid;
	attr->gid = sx->gid;
	attr->rdev = new_encode_dev(MKDEV(sx->rdev_major, sx->rdev_minor));
	attr->blksize = sx->blksize;
}

static bool fuse_valid_size(u64 size)
{
	return size <= LLONG_MAX;
}

static int fuse_do_statx(struct mnt_idmap *idmap, struct inode *inode,
			 struct file *file, struct kstat *stat)
{
	int err;
	struct fuse_attr attr;
	struct fuse_statx *sx;
	struct fuse_statx_in inarg;
	struct fuse_statx_out outarg;
	struct fuse_mount *fm = get_fuse_mount(inode);
	u64 attr_version = fuse_get_attr_version(fm->fc);
	FUSE_ARGS(args);

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));
	/* Directories have separate file-handle space */
	if (file && S_ISREG(inode->i_mode)) {
		struct fuse_file *ff = file->private_data;

		inarg.getattr_flags |= FUSE_GETATTR_FH;
		inarg.fh = ff->fh;
	}
	/* For now leave sync hints as the default, request all stats. */
	inarg.sx_flags = 0;
	inarg.sx_mask = STATX_BASIC_STATS | STATX_BTIME;
	args.opcode = FUSE_STATX;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(outarg);
	args.out_args[0].value = &outarg;
	err = fuse_simple_request(fm, &args);
	if (err)
		return err;

	sx = &outarg.stat;
	if (((sx->mask & STATX_SIZE) && !fuse_valid_size(sx->size)) ||
	    ((sx->mask & STATX_TYPE) && (!fuse_valid_type(sx->mode) ||
					 inode_wrong_type(inode, sx->mode)))) {
		fuse_make_bad(inode);
		return -EIO;
	}

	fuse_statx_to_attr(&outarg.stat, &attr);
	if ((sx->mask & STATX_BASIC_STATS) == STATX_BASIC_STATS) {
		fuse_change_attributes(inode, &attr, &outarg.stat,
				       ATTR_TIMEOUT(&outarg), attr_version);
	}

	if (stat) {
		stat->result_mask = sx->mask & (STATX_BASIC_STATS | STATX_BTIME);
		stat->btime.tv_sec = sx->btime.tv_sec;
		stat->btime.tv_nsec = min_t(u32, sx->btime.tv_nsec, NSEC_PER_SEC - 1);
		fuse_fillattr(idmap, inode, &attr, stat);
		stat->result_mask |= STATX_TYPE;
	}

	return 0;
}

int fuse_update_get_attr(struct mnt_idmap *idmap, struct inode *inode,
				struct file *file, const struct path *path, struct kstat *stat,
				u32 request_mask, unsigned int flags)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_conn *fc = get_fuse_conn(inode);
	int err = 0;
	bool sync;
	u32 inval_mask = READ_ONCE(fi->inval_mask);
	u32 cache_mask = fuse_get_cache_mask(inode);

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_getattr_io,
			       fuse_getattr_initialize,	fuse_getattr_backing,
			       fuse_getattr_finalize,
			       path->dentry, stat, request_mask, flags);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	/* FUSE only supports basic stats and possibly btime */
	request_mask &= STATX_BASIC_STATS | STATX_BTIME;
retry:
	if (fc->no_statx)
		request_mask &= STATX_BASIC_STATS;

	if (!request_mask)
		sync = false;
	else if (flags & AT_STATX_FORCE_SYNC)
		sync = true;
	else if (flags & AT_STATX_DONT_SYNC)
		sync = false;
	else if (request_mask & inval_mask & ~cache_mask)
		sync = true;
	else
		sync = time_before64(fi->i_time, get_jiffies_64());

	if (sync) {
		forget_all_cached_acls(inode);
		/* Try statx if BTIME is requested */
		if (!fc->no_statx && (request_mask & ~STATX_BASIC_STATS)) {
			err = fuse_do_statx(idmap, inode, file, stat);
			if (err == -ENOSYS) {
				fc->no_statx = 1;
				err = 0;
				goto retry;
			}
		} else {
			err = fuse_do_getattr(idmap, inode, stat, file);
		}
	} else if (stat) {
		generic_fillattr(idmap, request_mask, inode, stat);
		stat->mode = fi->orig_i_mode;
		stat->ino = fi->orig_ino;
		if (test_bit(FUSE_I_BTIME, &fi->state)) {
			stat->btime = fi->i_btime;
			stat->result_mask |= STATX_BTIME;
		}
	}

	return err;
}

static int fuse_access(struct inode *inode, int mask)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	struct fuse_access_in inarg;
	int err;

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_access_in,
			       fuse_access_initialize, fuse_access_backing,
			       fuse_access_finalize, inode, mask);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	BUG_ON(mask & MAY_NOT_BLOCK);

	/*
	 * We should not send FUSE_ACCESS to the userspace
	 * when idmapped mounts are enabled as for this case
	 * we have fc->default_permissions = 1 and access
	 * permission checks are done on the kernel side.
	 */
	WARN_ON_ONCE(!(fm->sb->s_iflags & SB_I_NOIDMAP));

	if (fm->fc->no_access)
		return 0;

	memset(&inarg, 0, sizeof(inarg));
	inarg.mask = mask & (MAY_READ | MAY_WRITE | MAY_EXEC);
	args.opcode = FUSE_ACCESS;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	err = fuse_simple_request(fm, &args);
	if (err == -ENOSYS) {
		fm->fc->no_access = 1;
		err = 0;
	}
	return err;
}

static int fuse_perm_getattr(struct inode *inode, int mask)
{
	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;

	forget_all_cached_acls(inode);
	return fuse_do_getattr(&nop_mnt_idmap, inode, NULL, NULL);
}

int fuse_permission(struct mnt_idmap *idmap,
			   struct inode *inode, int mask)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	bool refreshed = false;
	int err = 0;
	struct fuse_inode *fi = get_fuse_inode(inode);
#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;
#endif

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc))
		return -EACCES;

#ifdef CONFIG_FUSE_BPF
	fer = fuse_bpf_backing(inode, struct fuse_access_in,
			       fuse_access_initialize, fuse_access_backing,
			       fuse_access_finalize, inode, mask);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	/*
	 * If attributes are needed, refresh them before proceeding
	 */
	if (fc->default_permissions ||
	    ((mask & MAY_EXEC) && S_ISREG(inode->i_mode))) {
		u32 perm_mask = STATX_MODE | STATX_UID | STATX_GID;

		if (perm_mask & READ_ONCE(fi->inval_mask) ||
		    time_before64(fi->i_time, get_jiffies_64())) {
			refreshed = true;

			err = fuse_perm_getattr(inode, mask);
			if (err)
				return err;
		}
	}

	if (fc->default_permissions) {
		err = generic_permission(idmap, inode, mask);

		/* If permission is denied, try to refresh file
		   attributes.  This is also needed, because the root
		   node will at first have no permissions */
		if (err == -EACCES && !refreshed) {
			err = fuse_perm_getattr(inode, mask);
			if (!err)
				err = generic_permission(idmap,
							 inode, mask);
		}

		/* Note: the opposite of the above test does not
		   exist.  So if permissions are revoked this won't be
		   noticed immediately, only after the attribute
		   timeout has expired */
	} else if (mask & (MAY_ACCESS | MAY_CHDIR)) {
		err = fuse_access(inode, mask);
	} else if ((mask & MAY_EXEC) && S_ISREG(inode->i_mode)) {
		if (!(inode->i_mode & S_IXUGO)) {
			if (refreshed)
				return -EACCES;

			err = fuse_perm_getattr(inode, mask);
			if (!err && !(inode->i_mode & S_IXUGO))
				return -EACCES;
		}
	}
	return err;
}

int fuse_setattr(struct mnt_idmap *idmap, struct dentry *entry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(entry);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct file *file = (attr->ia_valid & ATTR_FILE) ? attr->ia_file : NULL;
	int ret;

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(get_fuse_conn(inode)))
		return -EACCES;

	if (attr->ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID)) {
		attr->ia_valid &= ~(ATTR_KILL_SUID | ATTR_KILL_SGID |
				    ATTR_MODE);

		/*
		 * The only sane way to reliably kill suid/sgid is to do it in
		 * the userspace filesystem
		 *
		 * This should be done on write(), truncate() and chown().
		 */
		if (!fc->handle_killpriv && !fc->handle_killpriv_v2) {
#ifdef CONFIG_FUSE_BPF
			struct fuse_err_ret fer;

			/*
			 * ia_mode calculation may have used stale i_mode.
			 * Refresh and recalculate.
			 */
			fer = fuse_bpf_backing(inode, struct fuse_getattr_io,
					       fuse_getattr_initialize,	fuse_getattr_backing,
					       fuse_getattr_finalize,
					       entry, NULL, 0, 0);
			if (fer.ret)
				ret = PTR_ERR(fer.result);
			else
#endif
				ret = fuse_do_getattr(idmap, inode, NULL, file);
			if (ret)
				return ret;

			attr->ia_mode = inode->i_mode;
			if (inode->i_mode & S_ISUID) {
				attr->ia_valid |= ATTR_MODE;
				attr->ia_mode &= ~S_ISUID;
			}
			if ((inode->i_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
				attr->ia_valid |= ATTR_MODE;
				attr->ia_mode &= ~S_ISGID;
			}
		}
	}
	if (!attr->ia_valid)
		return 0;

	ret = fuse_do_setattr(idmap, entry, attr, file);
	if (!ret) {
		/*
		 * If filesystem supports acls it may have updated acl xattrs in
		 * the filesystem, so forget cached acls for the inode.
		 */
		if (fc->posix_acl)
			forget_all_cached_acls(inode);

		/* Directory mode changed, may need to revalidate access */
		if (d_is_dir(entry) && (attr->ia_valid & ATTR_MODE))
			fuse_invalidate_entry_cache(entry);
	}
	return ret;
}

int fuse_getattr(struct mnt_idmap *idmap,
			const struct path *path, struct kstat *stat,
			u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct fuse_conn *fc = get_fuse_conn(inode);

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc)) {
		if (!request_mask) {
			/*
			 * If user explicitly requested *nothing* then don't
			 * error out, but return st_dev only.
			 */
			stat->result_mask = 0;
			stat->dev = inode->i_sb->s_dev;
			return 0;
		}
		return -EACCES;
	}

	return fuse_update_get_attr(idmap, inode, NULL, path, stat, request_mask,
				    flags);
}
