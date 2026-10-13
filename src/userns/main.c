// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_userns_test: " fmt
#include <linux/module.h>
#include <linux/of.h>
#include <linux/utsname.h>
#include <linux/cred.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/security.h>
#include "test.h"

/* Never attach these namespaces to a task on a CONFIG_USER_NS=n host. */
static struct user_namespace *test_ns[3];
static struct proc_dir_entry *test_dir;
static const char * const test_names[] = { "root", "large", "owner" };

static struct user_namespace *new_isolated_ns(struct user_namespace *parent,
					     kuid_t owner, kgid_t group)
{
	struct cred *cred = prepare_creds();
	struct user_namespace *ns;
	int ret;

	if (!cred)
		return ERR_PTR(-ENOMEM);
	/* Use the module-owned credential lifecycle, including userns references. */
	put_user_ns(cred->user_ns);
	cred->user_ns = get_user_ns(parent);
	cred->euid = owner;
	cred->egid = group;
	ret = create_user_ns(cred);
	ns = cred->user_ns;
	cred->user_ns = NULL; /* Transfer the namespace reference to our caller. */
	abort_creds(cred);
	if (ret) {
		put_user_ns(ns);
		return ERR_PTR(ret);
	}
	return ns;
}

static int limit_checks(void)
{
	struct user_namespace *chain[33] = {0}, *ns, *parent = &init_user_ns;
	int i, ret = -EINVAL;
	unsigned int refs = refcount_read(&init_user_ns.ns.count);

	for (i = 0; i < ARRAY_SIZE(chain); i++) {
		ns = new_isolated_ns(parent, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);
		if (IS_ERR(ns)) { ret = PTR_ERR(ns); goto out; }
		chain[i] = ns;
		ns->uid_map.extent[0] = (struct uid_gid_extent){0, 0, 1};
		ns->uid_map.nr_extents = 1;
		ns->gid_map.extent[0] = (struct uid_gid_extent){0, 0, 1};
		ns->gid_map.nr_extents = 1;
		parent = ns;
	}
	ns = new_isolated_ns(parent, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);
	if (!IS_ERR(ns)) { put_user_ns(ns); goto out; }
	if (PTR_ERR(ns) != -ENOSPC)
		goto out;
	/* No child is alive under chain[0] after dropping the descendants. */
	for (i = ARRAY_SIZE(chain) - 1; i > 0; i--) {
		put_user_ns(chain[i]);
		chain[i] = NULL;
	}
	ds_userns_core_drain();
	chain[0]->ucount_max[UCOUNT_USER_NAMESPACES] = 0;
	ns = new_isolated_ns(chain[0], GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);
	if (!IS_ERR(ns)) { put_user_ns(ns); goto out; }
	if (PTR_ERR(ns) != -ENOSPC)
		goto out;
	chain[0]->ucount_max[UCOUNT_USER_NAMESPACES] = INT_MAX;
	ns = new_isolated_ns(chain[0], KUIDT_INIT(1000), GLOBAL_ROOT_GID);
	if (!IS_ERR(ns)) { put_user_ns(ns); goto out; }
	if (PTR_ERR(ns) != -EPERM)
		goto out;
	ret = 0;
out:
	for (i = ARRAY_SIZE(chain) - 1; i >= 0; i--)
		put_user_ns(chain[i]);
	ds_userns_core_drain();
	if (ds_userns_core_live() || ds_userns_creds_live() ||
	    refcount_read(&init_user_ns.ns.count) != refs)
		ret = -EINVAL;
	if (!ret)
		pr_info("LIMIT_PASS: nesting limit, namespace quota, unmapped owner rejection and failure cleanup\n");
	return ret;
}

/* Compare with native ownership without publishing either object to a task. */
extern struct uts_namespace *native_copy_utsname(unsigned long flags,
	struct user_namespace *owner, struct uts_namespace *old) __asm__("copy_utsname");
extern void native_free_uts_ns(struct uts_namespace *ns) __asm__("free_uts_ns");

static int uts_checks(void)
{
	struct user_namespace *owner;
	struct uts_namespace *ns = NULL, *native = NULL, *shared;
	unsigned int refs = refcount_read(&init_user_ns.ns.count);
	int ret = -EINVAL;

	owner = new_isolated_ns(&init_user_ns, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);
	if (IS_ERR(owner))
		return PTR_ERR(owner);
	ns = copy_utsname(CLONE_NEWUTS, owner, &init_uts_ns);
	if (IS_ERR(ns)) { ret = PTR_ERR(ns); ns = NULL; goto out; }
	if (ns->user_ns != owner || refcount_read(&owner->ns.count) != 2 ||
	    ns->ns.ops->owner(&ns->ns) != owner ||
	    memcmp(&ns->name, &init_uts_ns.name, sizeof(ns->name)))
		goto out;
	shared = copy_utsname(0, owner, ns);
	if (shared != ns || refcount_read(&ns->ns.count) != 2) {
		if (!IS_ERR(shared))
			put_uts_ns(shared);
		goto out;
	}
	put_uts_ns(shared);
	ns->name.nodename[0] ^= 1;
	if (ns->name.nodename[0] == init_uts_ns.name.nodename[0])
		goto out;
	native = native_copy_utsname(CLONE_NEWUTS, owner, &init_uts_ns);
	if (IS_ERR(native)) { ret = PTR_ERR(native); native = NULL; goto out; }
	if (native->user_ns != &init_user_ns || refcount_read(&owner->ns.count) != 2)
		goto out;
	ret = 0;
out:
	if (native)
		native_free_uts_ns(native);
	if (ns)
		put_uts_ns(ns);
	put_user_ns(owner);
	ds_userns_core_drain();
	if (ds_userns_uts_live() || ds_userns_core_live() || ds_userns_creds_live() ||
	    refcount_read(&init_user_ns.ns.count) != refs)
		ret = -EINVAL;
	if (!ret)
		pr_info("UTS_PASS: correct private owner, sharing, name isolation, cleanup; native constructor still assigns init_user_ns\n");
	return ret;
}

static int core_checks(void)
{
	struct user_namespace *parent, *child = NULL, *sibling = NULL;
	struct cred *cred = NULL;
	struct cred *copy;
	int ret = -EINVAL;
	unsigned int refs = refcount_read(&init_user_ns.ns.count);

	parent = new_isolated_ns(&init_user_ns, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	if (uid_valid(make_kuid(parent, 0)) || gid_valid(make_kgid(parent, 0)) ||
	    userns_may_setgroups(parent))
		goto out;
	parent->uid_map.extent[0] = (struct uid_gid_extent){0, 100000, 10};
	parent->uid_map.nr_extents = 1;
	parent->gid_map.extent[0] = (struct uid_gid_extent){0, 200000, 10};
	parent->gid_map.nr_extents = 1;
	if (__kuid_val(make_kuid(parent, 3)) != 100003 ||
	    from_kuid(parent, KUIDT_INIT(100003)) != 3 ||
	    uid_valid(make_kuid(parent, 10)) ||
	    from_kuid(parent, GLOBAL_ROOT_UID) != (uid_t)-1 ||
	    __kgid_val(make_kgid(parent, 2)) != 200002 ||
	    from_kgid(parent, KGIDT_INIT(200002)) != 2 ||
	    from_kuid_munged(parent, GLOBAL_ROOT_UID) != overflowuid)
		goto out;
	child = new_isolated_ns(parent, KUIDT_INIT(100000), KGIDT_INIT(200000));
	if (IS_ERR(child)) { ret = PTR_ERR(child); child = NULL; goto out; }
	sibling = new_isolated_ns(parent, KUIDT_INIT(100000), KGIDT_INIT(200000));
	if (IS_ERR(sibling)) { ret = PTR_ERR(sibling); sibling = NULL; goto out; }
	if (child->level != parent->level + 1 || !in_userns(parent, child) ||
	    in_userns(child, parent) || in_userns(child, sibling))
		goto out;
	cred = prepare_creds();
	if (!cred) { ret = -ENOMEM; goto out; }
	put_user_ns(cred->user_ns);
	cred->user_ns = get_user_ns(child);
	cred->cap_effective = CAP_FULL_SET;
	if (cap_capable(cred, child, CAP_SYS_ADMIN, CAP_OPT_NONE) ||
	    cap_capable(cred, parent, CAP_SYS_ADMIN, CAP_OPT_NONE) != -EPERM ||
	    cap_capable(cred, &init_user_ns, CAP_SYS_ADMIN, CAP_OPT_NONE) != -EPERM ||
	    cap_capable(cred, sibling, CAP_SYS_ADMIN, CAP_OPT_NONE) != -EPERM)
		goto out;
	/* Copies must retain the noninitial namespace until their RCU callback. */
	copy = ds_userns_prepare_creds_from(cred);
	if (!copy) { ret = -ENOMEM; goto out; }
	if (copy->user_ns != child || refcount_read(&child->ns.count) != 3) {
		abort_creds(copy);
		goto out;
	}
	abort_creds(copy);
	ds_userns_core_drain();
	if (refcount_read(&child->ns.count) != 2)
		goto out;
	copy = ds_userns_prepare_creds_from(cred);
	if (!copy) { ret = -ENOMEM; goto out; }
	copy->non_rcu = 1;
	abort_creds(copy);
	if (refcount_read(&child->ns.count) != 2)
		goto out;
	copy = NULL;
	ret = unshare_userns(CLONE_NEWUSER, &copy);
	if (ret)
		goto out;
	if (!copy || copy->user_ns == &init_user_ns) {
		if (copy)
			abort_creds(copy);
		ret = -EINVAL;
		goto out;
	}
	abort_creds(copy);
	ret = 0;
out:
	if (cred)
		abort_creds(cred);
	put_user_ns(child);
	put_user_ns(sibling);
	put_user_ns(parent);
	ds_userns_core_drain();
	if (ds_userns_core_live() || ds_userns_creds_live() ||
	    refcount_read(&init_user_ns.ns.count) != refs)
		ret = -EINVAL;
	if (!ret)
		pr_info("CORE_PASS: empty maps, UID/GID translation, ancestry, native capability boundaries, private credentials, deferred release\n");
	return ret;
}

static int map_open(struct inode *inode, struct file *file,
		    const struct seq_operations *ops)
{
	int ret;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	ret = seq_open(file, ops);
	if (ret) { module_put(THIS_MODULE); return ret; }
	((struct seq_file *)file->private_data)->private = get_user_ns(pde_data(inode));
	return 0;
}
static int uid_open(struct inode *i, struct file *f) { return map_open(i, f, &proc_uid_seq_operations); }
static int gid_open(struct inode *i, struct file *f) { return map_open(i, f, &proc_gid_seq_operations); }
static int projid_open(struct inode *i, struct file *f) { return map_open(i, f, &proc_projid_seq_operations); }
static int map_release(struct inode *inode, struct file *file)
{
	put_user_ns(((struct seq_file *)file->private_data)->private);
	seq_release(inode, file);
	module_put(THIS_MODULE);
	return 0;
}
static int groups_open(struct inode *inode, struct file *file)
{
	struct user_namespace *ns = pde_data(inode);
	int ret;
	if ((file->f_mode & FMODE_WRITE) && !ns_capable(ns, CAP_SYS_ADMIN))
		return -EACCES;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	ret = single_open(file, proc_setgroups_show, get_user_ns(ns));
	if (ret) { put_user_ns(ns); module_put(THIS_MODULE); }
	return ret;
}
static int groups_release(struct inode *inode, struct file *file)
{
	put_user_ns(((struct seq_file *)file->private_data)->private);
	single_release(inode, file);
	module_put(THIS_MODULE);
	return 0;
}
#define MAP_OPS(kind) static const struct proc_ops kind##_ops = { \
	.proc_open = kind##_open, .proc_release = map_release, \
	.proc_read = seq_read, .proc_lseek = seq_lseek, .proc_write = proc_##kind##_map_write }
MAP_OPS(uid);
MAP_OPS(gid);
MAP_OPS(projid);
static const struct proc_ops groups_ops = {
	.proc_open = groups_open, .proc_release = groups_release,
	.proc_read = seq_read, .proc_lseek = seq_lseek, .proc_write = proc_setgroups_write,
};

static int check_show(struct seq_file *seq, void *unused)
{
	struct user_namespace *ns = seq->private;
	struct uid_gid_map *map = &ns->uid_map;
	unsigned int i, count = READ_ONCE(map->nr_extents);
	smp_rmb();
	for (i = 0; i < count; i++) {
		const struct uid_gid_extent *e = count <= UID_GID_MAP_MAX_BASE_EXTENTS ?
			&map->extent[i] : &map->forward[i];
		if (__kuid_val(make_kuid(ns, e->first)) != e->lower_first ||
		    from_kuid(ns, KUIDT_INIT(e->lower_first)) != e->first ||
		    __kuid_val(make_kuid(ns, e->first + e->count - 1)) != e->lower_first + e->count - 1 ||
		    from_kuid(ns, KUIDT_INIT(e->lower_first + e->count - 1)) != e->first + e->count - 1)
			return -EINVAL;
	}
	seq_printf(seq, "checked=%u\n", count);
	return 0;
}
static int check_open(struct inode *inode, struct file *file)
{
	struct user_namespace *ns = pde_data(inode);
	int ret;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	ret = single_open(file, check_show, get_user_ns(ns));
	if (ret) { put_user_ns(ns); module_put(THIS_MODULE); }
	return ret;
}
static const struct proc_ops check_ops = {
	.proc_open = check_open, .proc_release = groups_release,
	.proc_read = seq_read, .proc_lseek = seq_lseek,
};

static void remove_tests(void)
{
	int i;
	proc_remove(test_dir);
	for (i = 0; i < ARRAY_SIZE(test_ns); i++) {
		put_user_ns(test_ns[i]);
		test_ns[i] = NULL;
	}
	ds_userns_core_drain();
}

static int __init userns_test_init(void)
{
	int i, ret;
	/* Test endpoints and permissive file modes must never appear on a phone. */
	if (!of_machine_is_compatible("linux,dummy-virt") ||
	    (strcmp(utsname()->release, "6.12.76-4k-gae4e2f4f997e-dirty") &&
	     strcmp(utsname()->release, "6.12.69-android16-6-g242e1a07f878-ab15865446-4k")))
		return -EOPNOTSUPP;
	ret = ds_userns_core_init();
	if (ret)
		return ret;
	ret = ds_userns_uts_init();
	if (ret) {
		ds_userns_core_exit();
		return ret;
	}
	ret = core_checks();
	if (ret)
		goto fail;
	ret = limit_checks();
	if (ret)
		goto fail;
	ret = uts_checks();
	if (ret)
		goto fail;
	ret = -ENOMEM;
	test_dir = proc_mkdir("ds_userns_test", NULL);
	if (!test_dir)
		goto fail;
	for (i = 0; i < ARRAY_SIZE(test_ns); i++) {
		struct proc_dir_entry *dir;
		struct user_namespace *ns = new_isolated_ns(&init_user_ns,
			KUIDT_INIT(i == 2 ? 1000 : 0), KGIDT_INIT(i == 2 ? 1000 : 0));
		if (IS_ERR(ns)) { ret = PTR_ERR(ns); goto fail_tests; }
		test_ns[i] = ns;
		dir = proc_mkdir(test_names[i], test_dir);
		if (!dir || !proc_create_data("uid_map", 0666, dir, &uid_ops, ns) ||
		    !proc_create_data("gid_map", 0666, dir, &gid_ops, ns) ||
		    !proc_create_data("projid_map", 0666, dir, &projid_ops, ns) ||
		    !proc_create_data("setgroups", 0666, dir, &groups_ops, ns) ||
		    !proc_create_data("check", 0444, dir, &check_ops, ns))
			goto fail_tests;
	}
	pr_info("isolated test endpoints ready; no task credentials or native entries changed\n");
	return 0;
fail_tests:
	remove_tests();
fail:
	ds_userns_uts_exit();
	ds_userns_core_exit();
	return ret;
}
static void __exit userns_test_exit(void)
{
	remove_tests();
	pr_info("CORE_EXIT: remaining=%d creds=%d\n", ds_userns_core_live(), ds_userns_creds_live());
	ds_userns_uts_exit();
	ds_userns_core_exit();
}
module_init(userns_test_init);
module_exit(userns_test_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shikochin");
MODULE_DESCRIPTION("QEMU-only isolated user namespace core tests; no Sandboxing activation");
