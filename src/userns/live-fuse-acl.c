/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2016 Canonical Ltd. <seth.forshee@canonical.com>
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/* SPDX-License-Identifier: GPL-2.0-only */
/* FUSE user-namespace paths from the pinned android16-6.12 DDK.
 * Copyright (C) 2001-2008 Miklos Szeredi <miklos@szeredi.hu>
 */
#include "fs/fuse/fuse_i.h"
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include "live-fuse.h"


#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
static int posix_acl_fix_xattr_common(const void *value, size_t size)
{
	const struct posix_acl_xattr_header *header = value;
	int count;

	if (!header)
		return -EINVAL;
	if (size < sizeof(struct posix_acl_xattr_header))
		return -EINVAL;
	if (header->a_version != cpu_to_le32(POSIX_ACL_XATTR_VERSION))
		return -EOPNOTSUPP;

	count = posix_acl_xattr_count(size);
	if (count < 0)
		return -EINVAL;
	if (count == 0)
		return 0;

	return count;
}

static struct posix_acl *ds_fuse_acl_from_xattr(struct user_namespace *userns,
				       const void *value, size_t size)
{
	const struct posix_acl_xattr_header *header = value;
	const struct posix_acl_xattr_entry *entry = (const void *)(header + 1), *end;
	int count;
	struct posix_acl *acl;
	struct posix_acl_entry *acl_e;

	count = posix_acl_fix_xattr_common(value, size);
	if (count < 0)
		return ERR_PTR(count);
	if (count == 0)
		return NULL;

	acl = posix_acl_alloc(count, GFP_NOFS);
	if (!acl)
		return ERR_PTR(-ENOMEM);
	acl_e = acl->a_entries;

	for (end = entry + count; entry != end; acl_e++, entry++) {
		acl_e->e_tag  = le16_to_cpu(entry->e_tag);
		acl_e->e_perm = le16_to_cpu(entry->e_perm);

		switch(acl_e->e_tag) {
			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				break;

			case ACL_USER:
				acl_e->e_uid = make_kuid(userns,
						le32_to_cpu(entry->e_id));
				if (!uid_valid(acl_e->e_uid))
					goto fail;
				break;
			case ACL_GROUP:
				acl_e->e_gid = make_kgid(userns,
						le32_to_cpu(entry->e_id));
				if (!gid_valid(acl_e->e_gid))
					goto fail;
				break;

			default:
				goto fail;
		}
	}
	return acl;

fail:
	posix_acl_release(acl);
	return ERR_PTR(-EINVAL);
}

static int
ds_fuse_acl_to_xattr(struct user_namespace *user_ns, const struct posix_acl *acl,
		   void *buffer, size_t size)
{
	struct posix_acl_xattr_header *ext_acl = buffer;
	struct posix_acl_xattr_entry *ext_entry;
	int real_size, n;

	real_size = posix_acl_xattr_size(acl->a_count);
	if (!buffer)
		return real_size;
	if (real_size > size)
		return -ERANGE;

	ext_entry = (void *)(ext_acl + 1);
	ext_acl->a_version = cpu_to_le32(POSIX_ACL_XATTR_VERSION);

	for (n=0; n < acl->a_count; n++, ext_entry++) {
		const struct posix_acl_entry *acl_e = &acl->a_entries[n];
		ext_entry->e_tag  = cpu_to_le16(acl_e->e_tag);
		ext_entry->e_perm = cpu_to_le16(acl_e->e_perm);
		switch(acl_e->e_tag) {
		case ACL_USER:
			ext_entry->e_id =
				cpu_to_le32(from_kuid(user_ns, acl_e->e_uid));
			break;
		case ACL_GROUP:
			ext_entry->e_id =
				cpu_to_le32(from_kgid(user_ns, acl_e->e_gid));
			break;
		default:
			ext_entry->e_id = cpu_to_le32(ACL_UNDEFINED_ID);
			break;
		}
	}
	return real_size;
}


static struct posix_acl *__fuse_get_acl(struct fuse_conn *fc,
					struct inode *inode, int type, bool rcu)
{
	int size;
	const char *name;
	void *value = NULL;
	struct posix_acl *acl;

	if (rcu)
		return ERR_PTR(-ECHILD);

	if (fuse_is_bad(inode))
		return ERR_PTR(-EIO);

	if (fc->no_getxattr)
		return NULL;

	if (type == ACL_TYPE_ACCESS)
		name = XATTR_NAME_POSIX_ACL_ACCESS;
	else if (type == ACL_TYPE_DEFAULT)
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
	else
		return ERR_PTR(-EOPNOTSUPP);

	value = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!value)
		return ERR_PTR(-ENOMEM);
	size = fuse_getxattr(inode, name, value, PAGE_SIZE);
	if (size > 0)
		acl = ds_fuse_acl_from_xattr(fc->user_ns, value, size);
	else if ((size == 0) || (size == -ENODATA) ||
		 (size == -EOPNOTSUPP && fc->no_getxattr))
		acl = NULL;
	else if (size == -ERANGE)
		acl = ERR_PTR(-E2BIG);
	else
		acl = ERR_PTR(size);

	kfree(value);
	return acl;
}

static inline bool fuse_no_acl(const struct fuse_conn *fc,
			       const struct inode *inode)
{
	/*
	 * Refuse interacting with POSIX ACLs for daemons that
	 * don't support FUSE_POSIX_ACL and are not mounted on
	 * the host to retain backwards compatibility.
	 */
	return !fc->posix_acl && (i_user_ns(inode) != &init_user_ns);
}

struct posix_acl *ds_userns_fuse_get_acl(struct mnt_idmap *idmap,
			       struct dentry *dentry, int type)
{
	struct inode *inode = d_inode(dentry);
	struct fuse_conn *fc = get_fuse_conn(inode);

	if (fuse_no_acl(fc, inode))
		return ERR_PTR(-EOPNOTSUPP);

	return __fuse_get_acl(fc, inode, type, false);
}

struct posix_acl *ds_userns_fuse_get_inode_acl(struct inode *inode, int type, bool rcu)
{
	struct fuse_conn *fc = get_fuse_conn(inode);

	/*
	 * FUSE daemons before FUSE_POSIX_ACL was introduced could get and set
	 * POSIX ACLs without them being used for permission checking by the
	 * vfs. Retain that behavior for backwards compatibility as there are
	 * filesystems that do all permission checking for acls in the daemon
	 * and not in the kernel.
	 */
	if (!fc->posix_acl)
		return NULL;
	return __fuse_get_acl(fc,  inode, type, rcu);
}

int ds_userns_fuse_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	struct fuse_conn *fc = get_fuse_conn(inode);
	const char *name;
	int ret;

	if (fuse_is_bad(inode))
		return -EIO;

	if (fc->no_setxattr || fuse_no_acl(fc, inode))
		return -EOPNOTSUPP;

	if (type == ACL_TYPE_ACCESS)
		name = XATTR_NAME_POSIX_ACL_ACCESS;
	else if (type == ACL_TYPE_DEFAULT)
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
	else
		return -EINVAL;

	if (acl) {
		unsigned int extra_flags = 0;
		/*
		 * Fuse userspace is responsible for updating access
		 * permissions in the inode, if needed. fuse_setxattr
		 * invalidates the inode attributes, which will force
		 * them to be refreshed the next time they are used,
		 * and it also updates i_ctime.
		 */
		size_t size = posix_acl_xattr_size(acl->a_count);
		void *value;

		if (size > PAGE_SIZE)
			return -E2BIG;

		value = kmalloc(size, GFP_KERNEL);
		if (!value)
			return -ENOMEM;

		ret = ds_fuse_acl_to_xattr(fc->user_ns, acl, value, size);
		if (ret < 0) {
			kfree(value);
			return ret;
		}

		/*
		 * Fuse daemons without FUSE_POSIX_ACL never changed the passed
		 * through POSIX ACLs. Such daemons don't expect setgid bits to
		 * be stripped.
		 */
		if (fc->posix_acl &&
		    !in_group_or_capable(idmap, inode,
					 i_gid_into_vfsgid(idmap, inode)))
			extra_flags |= FUSE_SETXATTR_ACL_KILL_SGID;

		ret = fuse_setxattr(inode, name, value, size, 0, extra_flags);
		kfree(value);
	} else {
		ret = fuse_removexattr(inode, name);
	}

	if (fc->posix_acl) {
		/*
		 * Fuse daemons without FUSE_POSIX_ACL never cached POSIX ACLs
		 * and didn't invalidate attributes. Retain that behavior.
		 */
		forget_all_cached_acls(inode);
		fuse_invalidate_attr(inode);
	}

	return ret;
}

/* These VFS syscall boundaries also contain inlined !USER_NS conversion.
 * Keep other filesystems on their existing path; child FUSE ACLs must use
 * the caller's mapping before/after the filesystem-specific conversion. */
#include <uapi/linux/magic.h>
extern int ds_userns_fuse_do_set_acl_original(struct mnt_idmap *, struct dentry *,
					const char *, const void *, size_t);
extern ssize_t ds_userns_fuse_do_get_acl_original(struct mnt_idmap *, struct dentry *,
					const char *, void *, size_t);
static bool ds_child_fuse_acl(struct dentry *dentry)
{
	return dentry->d_sb->s_magic == FUSE_SUPER_MAGIC &&
		dentry->d_sb->s_user_ns != &init_user_ns;
}

int ds_userns_fuse_do_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
			     const char *name, const void *value, size_t size)
{
	struct posix_acl *acl = NULL;
	int error;

	if (!ds_child_fuse_acl(dentry))
		return ds_userns_fuse_do_set_acl_original(idmap, dentry, name, value, size);
	if (size) {
		acl = ds_fuse_acl_from_xattr(current_user_ns(), value, size);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
	}
	error = vfs_set_acl(idmap, dentry, name, acl);
	posix_acl_release(acl);
	return error;
}

ssize_t ds_userns_fuse_do_get_acl(struct mnt_idmap *idmap, struct dentry *dentry,
				 const char *name, void *value, size_t size)
{
	struct posix_acl *acl;
	ssize_t error;

	if (!ds_child_fuse_acl(dentry))
		return ds_userns_fuse_do_get_acl_original(idmap, dentry, name, value, size);
	/* Child FUSE connections never negotiate FUSE_ALLOW_IDMAP. */
	if (idmap != &nop_mnt_idmap)
		return -EOPNOTSUPP;
	acl = vfs_get_acl(idmap, dentry, name);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	error = ds_fuse_acl_to_xattr(current_user_ns(), acl, value, size);
	posix_acl_release(acl);
	return error;
}
