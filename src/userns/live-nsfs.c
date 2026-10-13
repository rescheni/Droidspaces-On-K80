// SPDX-License-Identifier: GPL-2.0
/* Restore USER_NS-dependent nsfs ioctls and network inode ownership. */
#include <linux/fs.h>
#include <linux/proc_ns.h>
#include <linux/user_namespace.h>
#include <linux/nsfs.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>
#include "live.h"
extern int open_related_ns(struct ns_common *, struct ns_common *(*)(struct ns_common *));
extern long ds_userns_ns_ioctl_original(struct file *, unsigned int, unsigned long);
long ds_userns_ns_ioctl(struct file *file, unsigned int request, unsigned long arg)
{
	struct ns_common *ns = get_proc_ns(file_inode(file));

	if (request == NS_GET_USERNS)
		return open_related_ns(ns, ns_get_owner);
	if (request == NS_GET_OWNER_UID) {
		struct user_namespace *owner;
		uid_t uid;

		if (ns->ops->type != CLONE_NEWUSER)
			return -EINVAL;
		owner = container_of(ns, struct user_namespace, ns);
		uid = from_kuid_munged(current_user_ns(), owner->owner);
		return put_user(uid, (uid_t __user *)arg);
	}
	return ds_userns_ns_ioctl_original(file, request, arg);
}

void ds_userns_net_ns_get_ownership(const struct net *net, kuid_t *uid, kgid_t *gid)
{
	if (net) {
		kuid_t ns_root_uid = make_kuid(net->user_ns, 0);
		kgid_t ns_root_gid = make_kgid(net->user_ns, 0);

		if (uid_valid(ns_root_uid))
			*uid = ns_root_uid;

		if (gid_valid(ns_root_gid))
			*gid = ns_root_gid;
	} else {
		*uid = GLOBAL_ROOT_UID;
		*gid = GLOBAL_ROOT_GID;
	}
}
