// SPDX-License-Identifier: GPL-2.0-only
/* Android common fs/super.c: restore ownership and references, including
 * inlined destroy_unused_super() paths in sget/sget_fc. Native lists, locks,
 * reclaim callbacks and filesystem/LSM policies are retained.
 */
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/backing-dev.h>
#include <linux/cleancache.h>
#include <linux/fsnotify.h>
#include <linux/user_namespace.h>
#include <linux/fs_context.h>
#include <uapi/linux/mount.h>
#include "fs/internal.h"
#include "live.h"
extern struct list_head super_blocks;
extern spinlock_t sb_lock;
extern bool grab_super(struct super_block *);
extern unsigned long super_cache_scan(struct shrinker *, struct shrink_control *);
extern unsigned long super_cache_count(struct shrinker *, struct shrink_control *);
#define alloc_super ds_userns_alloc_super
#define destroy_super_work ds_userns_destroy_super_work
static char *sb_writers_name[SB_FREEZE_LEVELS] = {
	"sb_writers", "sb_pagefaults", "sb_internal",
};
static inline void super_unlock(struct super_block *sb, bool excl)
{
	if (excl)
		up_write(&sb->s_umount);
	else
		up_read(&sb->s_umount);
}

static inline void super_unlock_excl(struct super_block *sb)
{
	super_unlock(sb, true);
}

void destroy_super_work(struct work_struct *work)
{
	struct super_block *s = container_of(work, struct super_block,
							destroy_work);
	fsnotify_sb_free(s);
	security_sb_free(s);
	put_user_ns(s->s_user_ns);
	kfree(s->s_subtype);
	for (int i = 0; i < SB_FREEZE_LEVELS; i++)
		percpu_free_rwsem(&s->s_writers.rw_sem[i]);
	kfree(s);
}

static void destroy_unused_super(struct super_block *s)
{
	if (!s)
		return;
	super_unlock_excl(s);
	list_lru_destroy(&s->s_dentry_lru);
	list_lru_destroy(&s->s_inode_lru);
	shrinker_free(s->s_shrink);
	/* no delays needed */
	destroy_super_work(&s->destroy_work);
}

struct super_block *alloc_super(struct file_system_type *type, int flags,
				       struct user_namespace *user_ns)
{
	struct super_block *s = kzalloc(sizeof(struct super_block), GFP_KERNEL);
	static const struct super_operations default_op;
	int i;

	if (!s)
		return NULL;

	INIT_LIST_HEAD(&s->s_mounts);
	s->s_user_ns = get_user_ns(user_ns);
	init_rwsem(&s->s_umount);
	lockdep_set_class(&s->s_umount, &type->s_umount_key);
	/*
	 * sget() can have s_umount recursion.
	 *
	 * When it cannot find a suitable sb, it allocates a new
	 * one (this one), and tries again to find a suitable old
	 * one.
	 *
	 * In case that succeeds, it will acquire the s_umount
	 * lock of the old one. Since these are clearly distrinct
	 * locks, and this object isn't exposed yet, there's no
	 * risk of deadlocks.
	 *
	 * Annotate this by putting this lock in a different
	 * subclass.
	 */
	down_write_nested(&s->s_umount, SINGLE_DEPTH_NESTING);

	if (security_sb_alloc(s))
		goto fail;

	for (i = 0; i < SB_FREEZE_LEVELS; i++) {
		if (__percpu_init_rwsem(&s->s_writers.rw_sem[i],
					sb_writers_name[i],
					&type->s_writers_key[i]))
			goto fail;
	}
	s->s_bdi = &noop_backing_dev_info;
	s->s_flags = flags;
	if (s->s_user_ns != &init_user_ns)
		s->s_iflags |= SB_I_NODEV;
	INIT_HLIST_NODE(&s->s_instances);
	INIT_HLIST_BL_HEAD(&s->s_roots);
	mutex_init(&s->s_sync_lock);
	INIT_LIST_HEAD(&s->s_inodes);
	spin_lock_init(&s->s_inode_list_lock);
	INIT_LIST_HEAD(&s->s_inodes_wb);
	spin_lock_init(&s->s_inode_wblist_lock);

	s->s_count = 1;
	atomic_set(&s->s_active, 1);
	mutex_init(&s->s_vfs_rename_mutex);
	lockdep_set_class(&s->s_vfs_rename_mutex, &type->s_vfs_rename_key);
	init_rwsem(&s->s_dquot.dqio_sem);
	s->s_maxbytes = MAX_NON_LFS;
	s->s_op = &default_op;
	s->s_time_gran = 1000000000;
	s->s_time_min = TIME64_MIN;
	s->s_time_max = TIME64_MAX;
	s->cleancache_poolid = CLEANCACHE_NO_POOL;

	s->s_shrink = shrinker_alloc(SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE,
				     "sb-%s", type->name);
	if (!s->s_shrink)
		goto fail;

	s->s_shrink->scan_objects = super_cache_scan;
	s->s_shrink->count_objects = super_cache_count;
	s->s_shrink->batch = 1024;
	s->s_shrink->private_data = s;

	if (list_lru_init_memcg(&s->s_dentry_lru, s->s_shrink))
		goto fail;
	if (list_lru_init_memcg(&s->s_inode_lru, s->s_shrink))
		goto fail;
	return s;

fail:
	destroy_unused_super(s);
	return NULL;
}

struct super_block *sget_fc(struct fs_context *fc,
			    int (*test)(struct super_block *, struct fs_context *),
			    int (*set)(struct super_block *, struct fs_context *))
{
	struct super_block *s = NULL;
	struct super_block *old;
	struct user_namespace *user_ns = fc->global ? &init_user_ns : fc->user_ns;
	int err;

	/*
	 * Never allow s_user_ns != &init_user_ns when FS_USERNS_MOUNT is
	 * not set, as the filesystem is likely unprepared to handle it.
	 * This can happen when fsconfig() is called from init_user_ns with
	 * an fs_fd opened in another user namespace.
	 */
	if (user_ns != &init_user_ns && !(fc->fs_type->fs_flags & FS_USERNS_MOUNT)) {
		errorfc(fc, "VFS: Mounting from non-initial user namespace is not allowed");
		return ERR_PTR(-EPERM);
	}

retry:
	spin_lock(&sb_lock);
	if (test) {
		hlist_for_each_entry(old, &fc->fs_type->fs_supers, s_instances) {
			if (test(old, fc))
				goto share_extant_sb;
		}
	}
	if (!s) {
		spin_unlock(&sb_lock);
		s = alloc_super(fc->fs_type, fc->sb_flags, user_ns);
		if (!s)
			return ERR_PTR(-ENOMEM);
		goto retry;
	}

	s->s_fs_info = fc->s_fs_info;
	err = set(s, fc);
	if (err) {
		s->s_fs_info = NULL;
		spin_unlock(&sb_lock);
		destroy_unused_super(s);
		return ERR_PTR(err);
	}
	fc->s_fs_info = NULL;
	s->s_type = fc->fs_type;
	s->s_iflags |= fc->s_iflags;
	strscpy(s->s_id, s->s_type->name, sizeof(s->s_id));
	/*
	 * Make the superblock visible on @super_blocks and @fs_supers.
	 * It's in a nascent state and users should wait on SB_BORN or
	 * SB_DYING to be set.
	 */
	list_add_tail(&s->s_list, &super_blocks);
	hlist_add_head(&s->s_instances, &s->s_type->fs_supers);
	spin_unlock(&sb_lock);
	get_filesystem(s->s_type);
	shrinker_register(s->s_shrink);
	return s;

share_extant_sb:
	if (user_ns != old->s_user_ns || fc->exclusive) {
		spin_unlock(&sb_lock);
		destroy_unused_super(s);
		if (fc->exclusive)
			warnfc(fc, "reusing existing filesystem not allowed");
		else
			warnfc(fc, "reusing existing filesystem in another namespace not allowed");
		return ERR_PTR(-EBUSY);
	}
	if (!grab_super(old))
		goto retry;
	destroy_unused_super(s);
	return old;
}

struct super_block *sget(struct file_system_type *type,
			int (*test)(struct super_block *,void *),
			int (*set)(struct super_block *,void *),
			int flags,
			void *data)
{
	struct user_namespace *user_ns = current_user_ns();
	struct super_block *s = NULL;
	struct super_block *old;
	int err;

	/* We don't yet pass the user namespace of the parent
	 * mount through to here so always use &init_user_ns
	 * until that changes.
	 */
	if (flags & SB_SUBMOUNT)
		user_ns = &init_user_ns;

retry:
	spin_lock(&sb_lock);
	if (test) {
		hlist_for_each_entry(old, &type->fs_supers, s_instances) {
			if (!test(old, data))
				continue;
			if (user_ns != old->s_user_ns) {
				spin_unlock(&sb_lock);
				destroy_unused_super(s);
				return ERR_PTR(-EBUSY);
			}
			if (!grab_super(old))
				goto retry;
			destroy_unused_super(s);
			return old;
		}
	}
	if (!s) {
		spin_unlock(&sb_lock);
		s = alloc_super(type, (flags & ~SB_SUBMOUNT), user_ns);
		if (!s)
			return ERR_PTR(-ENOMEM);
		goto retry;
	}

	err = set(s, data);
	if (err) {
		spin_unlock(&sb_lock);
		destroy_unused_super(s);
		return ERR_PTR(err);
	}
	s->s_type = type;
	strscpy(s->s_id, type->name, sizeof(s->s_id));
	list_add_tail(&s->s_list, &super_blocks);
	hlist_add_head(&s->s_instances, &type->fs_supers);
	spin_unlock(&sb_lock);
	get_filesystem(type);
	shrinker_register(s->s_shrink);
	return s;
}

