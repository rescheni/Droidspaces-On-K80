// SPDX-License-Identifier: GPL-2.0
/*
 * fs/sysfs/symlink.c - operations for initializing and mounting sysfs
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * Please see Documentation/filesystems/sysfs.rst for more information.
 */

#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/fs_context.h>
#include <net/net_namespace.h>

#include "fs/sysfs/sysfs.h"

extern struct kernfs_node *sysfs_root_kn;
extern const struct fs_context_operations sysfs_fs_context_ops;
int ds_userns_sysfs_init_fs_context(struct fs_context *fc)
{
	struct kernfs_fs_context *kfc;
	struct net *netns;

	if (!(fc->sb_flags & SB_KERNMOUNT)) {
		if (!kobj_ns_current_may_mount(KOBJ_NS_TYPE_NET))
			return -EPERM;
	}

	kfc = kzalloc(sizeof(struct kernfs_fs_context), GFP_KERNEL);
	if (!kfc)
		return -ENOMEM;

	kfc->ns_tag = netns = kobj_ns_grab_current(KOBJ_NS_TYPE_NET);
	kfc->root = sysfs_root_kn->dir.root;
	kfc->magic = SYSFS_MAGIC;
	fc->fs_private = kfc;
	fc->ops = &sysfs_fs_context_ops;
	if (netns) {
		put_user_ns(fc->user_ns);
		fc->user_ns = get_user_ns(netns->user_ns);
	}
	fc->global = true;
	return 0;
}
