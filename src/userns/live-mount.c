// SPDX-License-Identifier: GPL-2.0-only
/* User namespace ownership and lifetime from Android common fs/namespace.c.
 * Share the native namespace tree, locks and sequence counter. Both explicit
 * release and callers which inline release must use the USER_NS implementation.
 */
#include <linux/syscalls.h>
#include <linux/mnt_namespace.h>
#include <linux/user_namespace.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/fs_struct.h>
#include <linux/file.h>
#include <linux/proc_ns.h>
#include <linux/proc_fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/mnt_idmapping.h>
#include "pnode.h"
#include "fs/internal.h"
#include <uapi/linux/mount.h>
#include "live.h"

extern rwlock_t mnt_ns_tree_lock;
extern struct rb_root mnt_ns_tree;
extern atomic64_t mnt_ns_seq;
extern struct rw_semaphore namespace_sem;


static inline struct mnt_namespace *node_to_mnt_ns(const struct rb_node *node)
{
	if (!node)
		return NULL;
	return rb_entry(node, struct mnt_namespace, mnt_ns_tree_node);
}

static struct mnt_namespace *mnt_ns_find_id_at(u64 mnt_ns_id)
{
	struct rb_node *node = mnt_ns_tree.rb_node;
	struct mnt_namespace *ret = NULL;

	lockdep_assert_held(&mnt_ns_tree_lock);

	while (node) {
		struct mnt_namespace *n = node_to_mnt_ns(node);

		if (mnt_ns_id <= n->seq) {
			ret = node_to_mnt_ns(node);
			if (mnt_ns_id == n->seq)
				break;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return ret;
}

static struct mnt_namespace *lookup_mnt_ns(u64 mnt_ns_id)
{
       struct mnt_namespace *ns;

       guard(read_lock)(&mnt_ns_tree_lock);
       ns = mnt_ns_find_id_at(mnt_ns_id);
       if (!ns || ns->seq != mnt_ns_id)
               return NULL;

       refcount_inc(&ns->passive);
       return ns;
}

static inline struct mount *node_to_mount(struct rb_node *node)
{
	return node ? rb_entry(node, struct mount, mnt_node) : NULL;
}

static struct mount *mnt_find_id_at(struct mnt_namespace *ns, u64 mnt_id)
{
	struct rb_node *node = ns->mounts.rb_node;
	struct mount *ret = NULL;

	while (node) {
		struct mount *m = node_to_mount(node);

		if (mnt_id <= m->mnt_id_unique) {
			ret = node_to_mount(node);
			if (mnt_id == m->mnt_id_unique)
				break;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return ret;
}

static struct mount *mnt_find_id_at_reverse(struct mnt_namespace *ns, u64 mnt_id)
{
	struct rb_node *node = ns->mounts.rb_node;
	struct mount *ret = NULL;

	while (node) {
		struct mount *m = node_to_mount(node);

		if (mnt_id >= m->mnt_id_unique) {
			ret = node_to_mount(node);
			if (mnt_id == m->mnt_id_unique)
				break;
			node = node->rb_right;
		} else {
			node = node->rb_left;
		}
	}
	return ret;
}


#define MNT_UNIQUE_ID_OFFSET (1ULL << 31)
#define alloc_mnt_ns ds_userns_alloc_mnt_ns
#define mnt_ns_release ds_userns_mnt_ns_release
#define free_mnt_ns ds_userns_free_mnt_ns
void mnt_ns_release(struct mnt_namespace *ns)
{
	lockdep_assert_not_held(&mnt_ns_tree_lock);

	/* keep alive for {list,stat}mount() */
	if (ns && refcount_dec_and_test(&ns->passive)) {
		put_user_ns(ns->user_ns);
		kfree(ns);
	}
}
DEFINE_FREE(mnt_ns_release, struct mnt_namespace *,
	    if (!IS_ERR(_T)) mnt_ns_release(_T))

static void mnt_ns_tree_remove(struct mnt_namespace *ns)
{
	/* remove from global mount namespace list */
	if (!is_anon_ns(ns)) {
		guard(write_lock)(&mnt_ns_tree_lock);
		rb_erase(&ns->mnt_ns_tree_node, &mnt_ns_tree);
	}

	mnt_ns_release(ns);
}

static struct ucounts *inc_mnt_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_MNT_NAMESPACES);
}

static void dec_mnt_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_MNT_NAMESPACES);
}

void free_mnt_ns(struct mnt_namespace *ns)
{
	if (!is_anon_ns(ns))
		ns_free_inum(&ns->ns);
	dec_mnt_namespaces(ns->ucounts);
	mnt_ns_tree_remove(ns);
}

/*
 * Assign a sequence number so we can detect when we attempt to bind
 * mount a reference to an older mount namespace into the current
 * mount namespace, preventing reference counting loops.  A 64bit
 * number incrementing at 10Ghz will take 12,427 years to wrap which
 * is effectively never, so we can ignore the possibility.
 */
struct mnt_namespace *alloc_mnt_ns(struct user_namespace *user_ns, bool anon)
{
	struct mnt_namespace *new_ns;
	struct ucounts *ucounts;
	int ret;

	ucounts = inc_mnt_namespaces(user_ns);
	if (!ucounts)
		return ERR_PTR(-ENOSPC);

	new_ns = kzalloc(sizeof(struct mnt_namespace), GFP_KERNEL_ACCOUNT);
	if (!new_ns) {
		dec_mnt_namespaces(ucounts);
		return ERR_PTR(-ENOMEM);
	}
	if (!anon) {
		ret = ns_alloc_inum(&new_ns->ns);
		if (ret) {
			kfree(new_ns);
			dec_mnt_namespaces(ucounts);
			return ERR_PTR(ret);
		}
	}
	new_ns->ns.ops = &mntns_operations;
	if (!anon)
		new_ns->seq = atomic64_add_return(1, &mnt_ns_seq);
	refcount_set(&new_ns->ns.count, 1);
	refcount_set(&new_ns->passive, 1);
	new_ns->mounts = RB_ROOT;
	RB_CLEAR_NODE(&new_ns->mnt_ns_tree_node);
	init_waitqueue_head(&new_ns->poll);
	new_ns->user_ns = get_user_ns(user_ns);
	new_ns->ucounts = ucounts;
	return new_ns;
}

static struct vfsmount *lookup_mnt_in_ns(u64 id, struct mnt_namespace *ns)
{
	struct mount *mnt = mnt_find_id_at(ns, id);

	if (!mnt || mnt->mnt_id_unique != id)
		return NULL;

	return &mnt->mnt;
}

struct kstatmount {
	struct statmount __user *buf;
	size_t bufsize;
	struct vfsmount *mnt;
	u64 mask;
	struct path root;
	struct statmount sm;
	struct seq_file seq;
};

static u64 mnt_to_attr_flags(struct vfsmount *mnt)
{
	unsigned int mnt_flags = READ_ONCE(mnt->mnt_flags);
	u64 attr_flags = 0;

	if (mnt_flags & MNT_READONLY)
		attr_flags |= MOUNT_ATTR_RDONLY;
	if (mnt_flags & MNT_NOSUID)
		attr_flags |= MOUNT_ATTR_NOSUID;
	if (mnt_flags & MNT_NODEV)
		attr_flags |= MOUNT_ATTR_NODEV;
	if (mnt_flags & MNT_NOEXEC)
		attr_flags |= MOUNT_ATTR_NOEXEC;
	if (mnt_flags & MNT_NODIRATIME)
		attr_flags |= MOUNT_ATTR_NODIRATIME;
	if (mnt_flags & MNT_NOSYMFOLLOW)
		attr_flags |= MOUNT_ATTR_NOSYMFOLLOW;

	if (mnt_flags & MNT_NOATIME)
		attr_flags |= MOUNT_ATTR_NOATIME;
	else if (mnt_flags & MNT_RELATIME)
		attr_flags |= MOUNT_ATTR_RELATIME;
	else
		attr_flags |= MOUNT_ATTR_STRICTATIME;

	if (is_idmapped_mnt(mnt))
		attr_flags |= MOUNT_ATTR_IDMAP;

	return attr_flags;
}

static u64 mnt_to_propagation_flags(struct mount *m)
{
	u64 propagation = 0;

	if (IS_MNT_SHARED(m))
		propagation |= MS_SHARED;
	if (IS_MNT_SLAVE(m))
		propagation |= MS_SLAVE;
	if (IS_MNT_UNBINDABLE(m))
		propagation |= MS_UNBINDABLE;
	if (!propagation)
		propagation |= MS_PRIVATE;

	return propagation;
}

static void statmount_sb_basic(struct kstatmount *s)
{
	struct super_block *sb = s->mnt->mnt_sb;

	s->sm.mask |= STATMOUNT_SB_BASIC;
	s->sm.sb_dev_major = MAJOR(sb->s_dev);
	s->sm.sb_dev_minor = MINOR(sb->s_dev);
	s->sm.sb_magic = sb->s_magic;
	s->sm.sb_flags = sb->s_flags & (SB_RDONLY|SB_SYNCHRONOUS|SB_DIRSYNC|SB_LAZYTIME);
}

static void statmount_mnt_basic(struct kstatmount *s)
{
	struct mount *m = real_mount(s->mnt);

	s->sm.mask |= STATMOUNT_MNT_BASIC;
	s->sm.mnt_id = m->mnt_id_unique;
	s->sm.mnt_parent_id = m->mnt_parent->mnt_id_unique;
	s->sm.mnt_id_old = m->mnt_id;
	s->sm.mnt_parent_id_old = m->mnt_parent->mnt_id;
	s->sm.mnt_attr = mnt_to_attr_flags(&m->mnt);
	s->sm.mnt_propagation = mnt_to_propagation_flags(m);
	s->sm.mnt_peer_group = IS_MNT_SHARED(m) ? m->mnt_group_id : 0;
	s->sm.mnt_master = IS_MNT_SLAVE(m) ? m->mnt_master->mnt_group_id : 0;
}

static void statmount_propagate_from(struct kstatmount *s)
{
	struct mount *m = real_mount(s->mnt);

	s->sm.mask |= STATMOUNT_PROPAGATE_FROM;
	if (IS_MNT_SLAVE(m))
		s->sm.propagate_from = get_dominating_id(m, &current->fs->root);
}

static int statmount_mnt_root(struct kstatmount *s, struct seq_file *seq)
{
	int ret;
	size_t start = seq->count;

	ret = show_path(seq, s->mnt->mnt_root);
	if (ret)
		return ret;

	if (unlikely(seq_has_overflowed(seq)))
		return -EAGAIN;

	/*
         * Unescape the result. It would be better if supplied string was not
         * escaped in the first place, but that's a pretty invasive change.
         */
	seq->buf[seq->count] = '\0';
	seq->count = start;
	seq_commit(seq, string_unescape_inplace(seq->buf + start, UNESCAPE_OCTAL));
	return 0;
}

static int statmount_mnt_point(struct kstatmount *s, struct seq_file *seq)
{
	struct vfsmount *mnt = s->mnt;
	struct path mnt_path = { .dentry = mnt->mnt_root, .mnt = mnt };
	int err;

	err = seq_path_root(seq, &mnt_path, &s->root, "");
	return err == SEQ_SKIP ? 0 : err;
}

static int statmount_fs_type(struct kstatmount *s, struct seq_file *seq)
{
	struct super_block *sb = s->mnt->mnt_sb;

	seq_puts(seq, sb->s_type->name);
	return 0;
}

static void statmount_mnt_ns_id(struct kstatmount *s, struct mnt_namespace *ns)
{
	s->sm.mask |= STATMOUNT_MNT_NS_ID;
	s->sm.mnt_ns_id = ns->seq;
}

static int statmount_mnt_opts(struct kstatmount *s, struct seq_file *seq)
{
	struct vfsmount *mnt = s->mnt;
	struct super_block *sb = mnt->mnt_sb;
	size_t start = seq->count;
	int err;

	err = security_sb_show_options(seq, sb);
	if (err)
		return err;

	if (sb->s_op->show_options) {
		err = sb->s_op->show_options(seq, mnt->mnt_root);
		if (err)
			return err;
	}

	if (unlikely(seq_has_overflowed(seq)))
		return -EAGAIN;

	if (seq->count == start)
		return 0;

	/* skip leading comma */
	memmove(seq->buf + start, seq->buf + start + 1,
		seq->count - start - 1);
	seq->count--;

	return 0;
}

static int statmount_string(struct kstatmount *s, u64 flag)
{
	int ret;
	size_t kbufsize;
	struct seq_file *seq = &s->seq;
	struct statmount *sm = &s->sm;
	u32 start, *offp;

	/* Reserve an empty string at the beginning for any unset offsets */
	if (!seq->count)
		seq_putc(seq, 0);

	start = seq->count;

	switch (flag) {
	case STATMOUNT_FS_TYPE:
		offp = &sm->fs_type;
		ret = statmount_fs_type(s, seq);
		break;
	case STATMOUNT_MNT_ROOT:
		offp = &sm->mnt_root;
		ret = statmount_mnt_root(s, seq);
		break;
	case STATMOUNT_MNT_POINT:
		offp = &sm->mnt_point;
		ret = statmount_mnt_point(s, seq);
		break;
	case STATMOUNT_MNT_OPTS:
		offp = &sm->mnt_opts;
		ret = statmount_mnt_opts(s, seq);
		break;
	default:
		WARN_ON_ONCE(true);
		return -EINVAL;
	}

	if (unlikely(check_add_overflow(sizeof(*sm), seq->count, &kbufsize)))
		return -EOVERFLOW;
	if (kbufsize >= s->bufsize)
		return -EOVERFLOW;

	/* signal a retry */
	if (unlikely(seq_has_overflowed(seq)))
		return -EAGAIN;

	if (ret)
		return ret;

	seq->buf[seq->count++] = '\0';
	sm->mask |= flag;
	*offp = start;
	return 0;
}

static int copy_statmount_to_user(struct kstatmount *s)
{
	struct statmount *sm = &s->sm;
	struct seq_file *seq = &s->seq;
	char __user *str = ((char __user *)s->buf) + sizeof(*sm);
	size_t copysize = min_t(size_t, s->bufsize, sizeof(*sm));

	if (seq->count && copy_to_user(str, seq->buf, seq->count))
		return -EFAULT;

	/* Return the number of bytes copied to the buffer */
	sm->size = copysize + seq->count;
	if (copy_to_user(s->buf, sm, copysize))
		return -EFAULT;

	return 0;
}

static struct mount *listmnt_next(struct mount *curr, bool reverse)
{
	struct rb_node *node;

	if (reverse)
		node = rb_prev(&curr->mnt_node);
	else
		node = rb_next(&curr->mnt_node);

	return node_to_mount(node);
}

static int grab_requested_root(struct mnt_namespace *ns, struct path *root)
{
	struct mount *first, *child;

	rwsem_assert_held(&namespace_sem);

	/* We're looking at our own ns, just use get_fs_root. */
	if (ns == current->nsproxy->mnt_ns) {
		get_fs_root(current->fs, root);
		return 0;
	}

	/*
	 * We have to find the first mount in our ns and use that, however it
	 * may not exist, so handle that properly.
	 */
	if (RB_EMPTY_ROOT(&ns->mounts))
		return -ENOENT;

	first = child = ns->root;
	for (;;) {
		child = listmnt_next(child, false);
		if (!child)
			return -ENOENT;
		if (child->mnt_parent == first)
			break;
	}

	root->mnt = mntget(&child->mnt);
	root->dentry = dget(root->mnt->mnt_root);
	return 0;
}

static int do_statmount(struct kstatmount *s, u64 mnt_id, u64 mnt_ns_id,
			struct mnt_namespace *ns)
{
	struct mount *m;
	int err;

	/* Has the namespace already been emptied? */
	if (mnt_ns_id && RB_EMPTY_ROOT(&ns->mounts))
		return -ENOENT;

	s->mnt = lookup_mnt_in_ns(mnt_id, ns);
	if (!s->mnt)
		return -ENOENT;

	err = grab_requested_root(ns, &s->root);
	if (err)
		return err;

	/*
	 * Don't trigger audit denials. We just want to determine what
	 * mounts to show users.
	 */
	m = real_mount(s->mnt);
	if (!is_path_reachable(m, m->mnt.mnt_root, &s->root) &&
	    !ns_capable_noaudit(ns->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	err = security_sb_statfs(s->mnt->mnt_root);
	if (err)
		return err;
	if (s->mask & STATMOUNT_SB_BASIC)
		statmount_sb_basic(s);

	if (s->mask & STATMOUNT_MNT_BASIC)
		statmount_mnt_basic(s);

	if (s->mask & STATMOUNT_PROPAGATE_FROM)
		statmount_propagate_from(s);

	if (s->mask & STATMOUNT_FS_TYPE)
		err = statmount_string(s, STATMOUNT_FS_TYPE);

	if (!err && s->mask & STATMOUNT_MNT_ROOT)
		err = statmount_string(s, STATMOUNT_MNT_ROOT);

	if (!err && s->mask & STATMOUNT_MNT_POINT)
		err = statmount_string(s, STATMOUNT_MNT_POINT);

	if (!err && s->mask & STATMOUNT_MNT_OPTS)
		err = statmount_string(s, STATMOUNT_MNT_OPTS);

	if (!err && s->mask & STATMOUNT_MNT_NS_ID)
		statmount_mnt_ns_id(s, ns);

	if (err)
		return err;

	return 0;
}

static inline bool retry_statmount(const long ret, size_t *seq_size)
{
	if (likely(ret != -EAGAIN))
		return false;
	if (unlikely(check_mul_overflow(*seq_size, 2, seq_size)))
		return false;
	if (unlikely(*seq_size > MAX_RW_COUNT))
		return false;
	return true;
}

#define STATMOUNT_STRING_REQ (STATMOUNT_MNT_ROOT | STATMOUNT_MNT_POINT | \
			      STATMOUNT_FS_TYPE | STATMOUNT_MNT_OPTS)

static int prepare_kstatmount(struct kstatmount *ks, struct mnt_id_req *kreq,
			      struct statmount __user *buf, size_t bufsize,
			      size_t seq_size)
{
	if (!access_ok(buf, bufsize))
		return -EFAULT;

	memset(ks, 0, sizeof(*ks));
	ks->mask = kreq->param;
	ks->buf = buf;
	ks->bufsize = bufsize;

	if (ks->mask & STATMOUNT_STRING_REQ) {
		if (bufsize == sizeof(ks->sm))
			return -EOVERFLOW;

		ks->seq.buf = kvmalloc(seq_size, GFP_KERNEL_ACCOUNT);
		if (!ks->seq.buf)
			return -ENOMEM;

		ks->seq.size = seq_size;
	}

	return 0;
}

static int copy_mnt_id_req(const struct mnt_id_req __user *req,
			   struct mnt_id_req *kreq)
{
	int ret;
	size_t usize;

	BUILD_BUG_ON(sizeof(struct mnt_id_req) != MNT_ID_REQ_SIZE_VER1);

	ret = get_user(usize, &req->size);
	if (ret)
		return -EFAULT;
	if (unlikely(usize > PAGE_SIZE))
		return -E2BIG;
	if (unlikely(usize < MNT_ID_REQ_SIZE_VER0))
		return -EINVAL;
	memset(kreq, 0, sizeof(*kreq));
	ret = copy_struct_from_user(kreq, sizeof(*kreq), req, usize);
	if (ret)
		return ret;
	if (kreq->mnt_ns_fd != 0 && kreq->mnt_ns_id)
		return -EINVAL;
	/* The first valid unique mount id is MNT_UNIQUE_ID_OFFSET + 1. */
	if (kreq->mnt_id <= MNT_UNIQUE_ID_OFFSET)
		return -EINVAL;
	return 0;
}

/*
 * If the user requested a specific mount namespace id, look that up and return
 * that, or if not simply grab a passive reference on our mount namespace and
 * return that.
 */
static struct mnt_namespace *grab_requested_mnt_ns(const struct mnt_id_req *kreq)
{
	struct mnt_namespace *mnt_ns;

	if (kreq->mnt_ns_id) {
		mnt_ns = lookup_mnt_ns(kreq->mnt_ns_id);
		if (!mnt_ns)
			return ERR_PTR(-ENOENT);
	} else if (kreq->mnt_ns_fd) {
		struct ns_common *ns;

		CLASS(fd, f)(kreq->mnt_ns_fd);
		if (fd_empty(f))
			return ERR_PTR(-EBADF);

		if (!proc_ns_file(fd_file(f)))
			return ERR_PTR(-EINVAL);

		ns = get_proc_ns(file_inode(fd_file(f)));
		if (ns->ops->type != CLONE_NEWNS)
			return ERR_PTR(-EINVAL);

		mnt_ns = to_mnt_ns(ns);
		refcount_inc(&mnt_ns->passive);
	} else {
		mnt_ns = current->nsproxy->mnt_ns;
		refcount_inc(&mnt_ns->passive);
	}

	return mnt_ns;
}

SYSCALL_DEFINE4(ds_statmount, const struct mnt_id_req __user *, req,
		struct statmount __user *, buf, size_t, bufsize,
		unsigned int, flags)
{
	struct mnt_namespace *ns __free(mnt_ns_release) = NULL;
	struct kstatmount *ks __free(kfree) = NULL;
	struct mnt_id_req kreq;
	/* We currently support retrieval of 3 strings. */
	size_t seq_size = 3 * PATH_MAX;
	int ret;

	if (flags)
		return -EINVAL;

	ret = copy_mnt_id_req(req, &kreq);
	if (ret)
		return ret;

	ns = grab_requested_mnt_ns(&kreq);
	if (IS_ERR(ns))
		return PTR_ERR(ns);

	if (kreq.mnt_ns_id && (ns != current->nsproxy->mnt_ns) &&
	    !ns_capable_noaudit(ns->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	ks = kmalloc(sizeof(*ks), GFP_KERNEL_ACCOUNT);
	if (!ks)
		return -ENOMEM;

retry:
	ret = prepare_kstatmount(ks, &kreq, buf, bufsize, seq_size);
	if (ret)
		return ret;

	scoped_guard(rwsem_read, &namespace_sem)
		ret = do_statmount(ks, kreq.mnt_id, kreq.mnt_ns_id, ns);

	if (!ret)
		ret = copy_statmount_to_user(ks);
	kvfree(ks->seq.buf);
	path_put(&ks->root);
	if (retry_statmount(ret, &seq_size))
		goto retry;
	return ret;
}

struct klistmount {
	u64 last_mnt_id;
	u64 mnt_parent_id;
	u64 *kmnt_ids;
	u32 nr_mnt_ids;
	struct mnt_namespace *ns;
	struct path root;
};

static ssize_t do_listmount(struct klistmount *kls, bool reverse)
{
	struct mnt_namespace *ns = kls->ns;
	u64 mnt_parent_id = kls->mnt_parent_id;
	u64 last_mnt_id = kls->last_mnt_id;
	u64 *mnt_ids = kls->kmnt_ids;
	size_t nr_mnt_ids = kls->nr_mnt_ids;
	struct path orig;
	struct mount *r, *first;
	ssize_t ret;

	rwsem_assert_held(&namespace_sem);

	ret = grab_requested_root(ns, &kls->root);
	if (ret)
		return ret;

	if (mnt_parent_id == LSMT_ROOT) {
		orig = kls->root;
	} else {
		orig.mnt = lookup_mnt_in_ns(mnt_parent_id, ns);
		if (!orig.mnt)
			return -ENOENT;
		orig.dentry = orig.mnt->mnt_root;
	}

	/*
	 * Don't trigger audit denials. We just want to determine what
	 * mounts to show users.
	 */
	if (!is_path_reachable(real_mount(orig.mnt), orig.dentry, &kls->root) &&
	    !ns_capable_noaudit(ns->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	ret = security_sb_statfs(orig.dentry);
	if (ret)
		return ret;

	if (!last_mnt_id) {
		if (reverse)
			first = node_to_mount(rb_last(&ns->mounts));
		else
			first = node_to_mount(rb_first(&ns->mounts));
	} else {
		if (reverse)
			first = mnt_find_id_at_reverse(ns, last_mnt_id - 1);
		else
			first = mnt_find_id_at(ns, last_mnt_id + 1);
	}

	for (ret = 0, r = first; r && nr_mnt_ids; r = listmnt_next(r, reverse)) {
		if (r->mnt_id_unique == mnt_parent_id)
			continue;
		if (!is_path_reachable(r, r->mnt.mnt_root, &orig))
			continue;
		*mnt_ids = r->mnt_id_unique;
		mnt_ids++;
		nr_mnt_ids--;
		ret++;
	}
	return ret;
}

static void __free_klistmount_free(const struct klistmount *kls)
{
	path_put(&kls->root);
	kvfree(kls->kmnt_ids);
	mnt_ns_release(kls->ns);
}

static inline int prepare_klistmount(struct klistmount *kls, struct mnt_id_req *kreq,
				     size_t nr_mnt_ids)
{
	u64 last_mnt_id = kreq->param;
	struct mnt_namespace *ns;

	/* The first valid unique mount id is MNT_UNIQUE_ID_OFFSET + 1. */
	if (last_mnt_id != 0 && last_mnt_id <= MNT_UNIQUE_ID_OFFSET)
		return -EINVAL;

	kls->last_mnt_id = last_mnt_id;

	kls->nr_mnt_ids = nr_mnt_ids;
	kls->kmnt_ids = kvmalloc_array(nr_mnt_ids, sizeof(*kls->kmnt_ids),
				       GFP_KERNEL_ACCOUNT);
	if (!kls->kmnt_ids)
		return -ENOMEM;

	ns = grab_requested_mnt_ns(kreq);
	if (IS_ERR(ns))
		return PTR_ERR(ns);
	kls->ns = ns;

	kls->mnt_parent_id = kreq->mnt_id;
	return 0;
}

SYSCALL_DEFINE4(ds_listmount, const struct mnt_id_req __user *, req,
		u64 __user *, mnt_ids, size_t, nr_mnt_ids, unsigned int, flags)
{
	struct klistmount kls __free(klistmount_free) = {};
	const size_t maxcount = 1000000;
	struct mnt_id_req kreq;
	ssize_t ret;

	if (flags & ~LISTMOUNT_REVERSE)
		return -EINVAL;

	/*
	 * If the mount namespace really has more than 1 million mounts the
	 * caller must iterate over the mount namespace (and reconsider their
	 * system design...).
	 */
	if (unlikely(nr_mnt_ids > maxcount))
		return -EOVERFLOW;

	if (!access_ok(mnt_ids, nr_mnt_ids * sizeof(*mnt_ids)))
		return -EFAULT;

	ret = copy_mnt_id_req(req, &kreq);
	if (ret)
		return ret;

	ret = prepare_klistmount(&kls, &kreq, nr_mnt_ids);
	if (ret)
		return ret;

	if (kreq.mnt_ns_id && (kls.ns != current->nsproxy->mnt_ns) &&
	    !ns_capable_noaudit(kls.ns->user_ns, CAP_SYS_ADMIN))
		return -ENOENT;

	scoped_guard(rwsem_read, &namespace_sem)
		ret = do_listmount(&kls, (flags & LISTMOUNT_REVERSE));
	if (ret <= 0)
		return ret;

	if (copy_to_user(mnt_ids, kls.kmnt_ids, ret * sizeof(*mnt_ids)))
		return -EFAULT;

	return ret;
}


bool ds_userns_mnt_may_suid(struct vfsmount *mnt)
{
	/*
	 * Foreign mounts (accessed via fchdir or through /proc
	 * symlinks) are always treated as if they are nosuid.  This
	 * prevents namespaces from trusting potentially unsafe
	 * suid/sgid bits, file caps, or security labels that originate
	 * in other namespaces.
	 */
	return !(mnt->mnt_flags & MNT_NOSUID) && (real_mount(mnt)->mnt_ns == current->nsproxy->mnt_ns) &&
	       current_in_userns(mnt->mnt_sb->s_user_ns);
}

