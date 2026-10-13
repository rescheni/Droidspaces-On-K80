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
#include <linux/posix_acl.h>
#define fuse_change_attributes_common ds_userns_fuse_change_attributes_common
#define fuse_fillattr ds_userns_fuse_fillattr
static ino_t fuse_squash_ino(u64 ino64)
{
	ino_t ino = (ino_t) ino64;
	if (sizeof(ino_t) < sizeof(u64))
		ino ^= ino64 >> (sizeof(u64) - sizeof(ino_t)) * 8;
	return ino;
}


void fuse_change_attributes_common(struct inode *inode, struct fuse_attr *attr,
				   struct fuse_statx *sx,
				   u64 attr_valid, u32 cache_mask)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);

	lockdep_assert_held(&fi->lock);

	fi->attr_version = atomic64_inc_return(&fc->attr_version);
	fi->i_time = attr_valid;
	/* Clear basic stats from invalid mask */
	set_mask_bits(&fi->inval_mask, STATX_BASIC_STATS, 0);

	inode->i_ino     = fuse_squash_ino(attr->ino);
	inode->i_mode    = (inode->i_mode & S_IFMT) | (attr->mode & 07777);
	set_nlink(inode, attr->nlink);
	inode->i_uid     = make_kuid(fc->user_ns, attr->uid);
	inode->i_gid     = make_kgid(fc->user_ns, attr->gid);
	inode->i_blocks  = attr->blocks;

	/* Sanitize nsecs */
	attr->atimensec = min_t(u32, attr->atimensec, NSEC_PER_SEC - 1);
	attr->mtimensec = min_t(u32, attr->mtimensec, NSEC_PER_SEC - 1);
	attr->ctimensec = min_t(u32, attr->ctimensec, NSEC_PER_SEC - 1);

	inode_set_atime(inode, attr->atime, attr->atimensec);
	/* mtime from server may be stale due to local buffered write */
	if (!(cache_mask & STATX_MTIME)) {
		inode_set_mtime(inode, attr->mtime, attr->mtimensec);
	}
	if (!(cache_mask & STATX_CTIME)) {
		inode_set_ctime(inode, attr->ctime, attr->ctimensec);
	}
	if (sx) {
		/* Sanitize nsecs */
		sx->btime.tv_nsec =
			min_t(u32, sx->btime.tv_nsec, NSEC_PER_SEC - 1);

		/*
		 * Btime has been queried, cache is valid (whether or not btime
		 * is available or not) so clear STATX_BTIME from inval_mask.
		 *
		 * Availability of the btime attribute is indicated in
		 * FUSE_I_BTIME
		 */
		set_mask_bits(&fi->inval_mask, STATX_BTIME, 0);
		if (sx->mask & STATX_BTIME) {
			set_bit(FUSE_I_BTIME, &fi->state);
			fi->i_btime.tv_sec = sx->btime.tv_sec;
			fi->i_btime.tv_nsec = sx->btime.tv_nsec;
		}
	}

	if (attr->blksize != 0)
		inode->i_blkbits = ilog2(attr->blksize);
	else
		inode->i_blkbits = inode->i_sb->s_blocksize_bits;

	/*
	 * Don't set the sticky bit in i_mode, unless we want the VFS
	 * to check permissions.  This prevents failures due to the
	 * check in may_delete().
	 */
	fi->orig_i_mode = inode->i_mode;
	if (!fc->default_permissions)
		inode->i_mode &= ~S_ISVTX;

	fi->orig_ino = attr->ino;

	/*
	 * We are refreshing inode data and it is possible that another
	 * client set suid/sgid or security.capability xattr. So clear
	 * S_NOSEC. Ideally, we could have cleared it only if suid/sgid
	 * was set or if security.capability xattr was set. But we don't
	 * know if security.capability has been set or not. So clear it
	 * anyway. Its less efficient but should be safe.
	 */
	inode->i_flags &= ~S_NOSEC;
}


void fuse_fillattr(struct mnt_idmap *idmap, struct inode *inode,
			  struct fuse_attr *attr, struct kstat *stat)
{
	unsigned int blkbits;
	struct fuse_conn *fc = get_fuse_conn(inode);
	vfsuid_t vfsuid = make_vfsuid(idmap, fc->user_ns,
				      make_kuid(fc->user_ns, attr->uid));
	vfsgid_t vfsgid = make_vfsgid(idmap, fc->user_ns,
				      make_kgid(fc->user_ns, attr->gid));

	stat->dev = inode->i_sb->s_dev;
	stat->ino = attr->ino;
	stat->mode = (inode->i_mode & S_IFMT) | (attr->mode & 07777);
	stat->nlink = attr->nlink;
	stat->uid = vfsuid_into_kuid(vfsuid);
	stat->gid = vfsgid_into_kgid(vfsgid);
	stat->rdev = inode->i_rdev;
	stat->atime.tv_sec = attr->atime;
	stat->atime.tv_nsec = attr->atimensec;
	stat->mtime.tv_sec = attr->mtime;
	stat->mtime.tv_nsec = attr->mtimensec;
	stat->ctime.tv_sec = attr->ctime;
	stat->ctime.tv_nsec = attr->ctimensec;
	stat->size = attr->size;
	stat->blocks = attr->blocks;

	if (attr->blksize != 0)
		blkbits = ilog2(attr->blksize);
	else
		blkbits = inode->i_sb->s_blocksize_bits;

	stat->blksize = 1 << blkbits;
}
