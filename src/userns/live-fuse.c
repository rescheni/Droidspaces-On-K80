/* SPDX-License-Identifier: GPL-2.0-only */
/* FUSE user-namespace paths from the pinned android16-6.12 DDK.
 * Copyright (C) 2001-2008 Miklos Szeredi <miklos@szeredi.hu>
 */
#include "fs/fuse/fuse_i.h"
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include "live-fuse.h"

#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>
#include <linux/filter.h>
#define FUSE_DEFAULT_BLKSIZE 512
extern int ds_userns_fuse_get_tree_original(struct fs_context *);
extern void ds_fuse_conn_init(struct fuse_conn *, struct fuse_mount *,
    struct user_namespace *, const struct fuse_iqueue_ops *, void *);
extern const struct fuse_iqueue_ops ds_userns_fuse_iq_ops;
enum {
	OPT_SOURCE,
	OPT_SUBTYPE,
	OPT_FD,
	OPT_ROOTMODE,
	OPT_USER_ID,
	OPT_GROUP_ID,
	OPT_DEFAULT_PERMISSIONS,
	OPT_ALLOW_OTHER,
	OPT_MAX_READ,
	OPT_BLKSIZE,
	OPT_ROOT_BPF,
	OPT_ROOT_DIR,
	OPT_NO_DAEMON,
	OPT_ERR
};

static const struct fs_parameter_spec fuse_fs_parameters[] = {
	fsparam_string	("source",		OPT_SOURCE),
	fsparam_u32	("fd",			OPT_FD),
	fsparam_u32oct	("rootmode",		OPT_ROOTMODE),
	fsparam_u32	("user_id",		OPT_USER_ID),
	fsparam_u32	("group_id",		OPT_GROUP_ID),
	fsparam_flag	("default_permissions",	OPT_DEFAULT_PERMISSIONS),
	fsparam_flag	("allow_other",		OPT_ALLOW_OTHER),
	fsparam_u32	("max_read",		OPT_MAX_READ),
	fsparam_u32	("blksize",		OPT_BLKSIZE),
	fsparam_string	("subtype",		OPT_SUBTYPE),
	fsparam_u32	("root_bpf",		OPT_ROOT_BPF),
	fsparam_u32	("root_dir",		OPT_ROOT_DIR),
	fsparam_flag	("no_daemon",		OPT_NO_DAEMON),
	{}
};

int ds_userns_fuse_parse_param(struct fs_context *fsc, struct fs_parameter *param)
{
	struct fs_parse_result result;
	struct fuse_fs_context *ctx = fsc->fs_private;
	int opt;
	kuid_t kuid;
	kgid_t kgid;

	if (fsc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
		/*
		 * Ignore options coming from mount(MS_REMOUNT) for backward
		 * compatibility.
		 */
		if (fsc->oldapi)
			return 0;

		return invalfc(fsc, "No changes allowed in reconfigure");
	}

	opt = fs_parse(fsc, fuse_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	/* Android BPF backing is retained for host mounts only. */
	if (fsc->user_ns != &init_user_ns &&
	    (opt == OPT_ROOT_BPF || opt == OPT_ROOT_DIR || opt == OPT_NO_DAEMON))
		return -EOPNOTSUPP;

	switch (opt) {
	case OPT_SOURCE:
		if (fsc->source)
			return invalfc(fsc, "Multiple sources specified");
		fsc->source = param->string;
		param->string = NULL;
		break;

	case OPT_SUBTYPE:
		if (ctx->subtype)
			return invalfc(fsc, "Multiple subtypes specified");
		ctx->subtype = param->string;
		param->string = NULL;
		return 0;

	case OPT_FD:
		ctx->fd = result.uint_32;
		ctx->fd_present = true;
		break;

	case OPT_ROOTMODE:
		if (!fuse_valid_type(result.uint_32))
			return invalfc(fsc, "Invalid rootmode");
		ctx->rootmode = result.uint_32;
		ctx->rootmode_present = true;
		break;

	case OPT_USER_ID:
		kuid = make_kuid(current_user_ns(), result.uint_32);
		/*
		 * The requested uid must be representable in the
		 * filesystem's idmapping.
		 */
		if (!kuid_has_mapping(fsc->user_ns, kuid))
			return invalfc(fsc, "Invalid user_id");
		ctx->user_id = kuid;
		ctx->user_id_present = true;
		break;

	case OPT_GROUP_ID:
		kgid = make_kgid(current_user_ns(), result.uint_32);
		/*
		 * The requested gid must be representable in the
		 * filesystem's idmapping.
		 */
		if (!kgid_has_mapping(fsc->user_ns, kgid))
			return invalfc(fsc, "Invalid group_id");
		ctx->group_id = kgid;
		ctx->group_id_present = true;
		break;

	case OPT_DEFAULT_PERMISSIONS:
		ctx->default_permissions = true;
		break;

	case OPT_ALLOW_OTHER:
		ctx->allow_other = true;
		break;

	case OPT_MAX_READ:
		ctx->max_read = result.uint_32;
		break;

	case OPT_BLKSIZE:
		if (!ctx->is_bdev)
			return invalfc(fsc, "blksize only supported for fuseblk");
		ctx->blksize = result.uint_32;
		break;

	case OPT_ROOT_BPF:
		ctx->root_bpf = bpf_prog_get_type_dev(result.uint_32,
						BPF_PROG_TYPE_FUSE, false);
		if (IS_ERR(ctx->root_bpf)) {
			ctx->root_bpf = NULL;
			return invalfc(fsc, "Unable to open bpf program");
		}
		break;

	case OPT_ROOT_DIR:
		ctx->root_dir = fget(result.uint_32);
		if (!ctx->root_dir)
			return invalfc(fsc, "Unable to open root directory");
		break;

	case OPT_NO_DAEMON:
		ctx->no_daemon = true;
		ctx->fd_present = true;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}
int ds_userns_fuse_show_options(struct seq_file *m, struct dentry *root)
{
	struct super_block *sb = root->d_sb;
	struct fuse_conn *fc = get_fuse_conn_super(sb);

	if (fc->legacy_opts_show) {
		seq_printf(m, ",user_id=%u",
			   from_kuid_munged(fc->user_ns, fc->user_id));
		seq_printf(m, ",group_id=%u",
			   from_kgid_munged(fc->user_ns, fc->group_id));
		if (fc->default_permissions)
			seq_puts(m, ",default_permissions");
		if (fc->allow_other)
			seq_puts(m, ",allow_other");
		if (fc->max_read != ~0)
			seq_printf(m, ",max_read=%u", fc->max_read);
		if (sb->s_bdev && sb->s_blocksize != FUSE_DEFAULT_BLKSIZE)
			seq_printf(m, ",blksize=%lu", sb->s_blocksize);
	}
#ifdef CONFIG_FUSE_DAX
	if (fc->dax_mode == FUSE_DAX_ALWAYS)
		seq_puts(m, ",dax=always");
	else if (fc->dax_mode == FUSE_DAX_NEVER)
		seq_puts(m, ",dax=never");
	else if (fc->dax_mode == FUSE_DAX_INODE_USER)
		seq_puts(m, ",dax=inode");
#endif

	return 0;
}
static int fuse_fill_super(struct super_block *sb, struct fs_context *fsc)
{
	struct fuse_fs_context *ctx = fsc->fs_private;
	int err;

	if (!ctx->no_daemon) {
		if (!ctx->file || !ctx->rootmode_present ||
		    !ctx->user_id_present || !ctx->group_id_present)
			return -EINVAL;

		/*
		 * Require mount to happen from the same user namespace which
		 * opened /dev/fuse to prevent potential attacks.
		 */
		if ((ctx->file->f_op != &fuse_dev_operations) ||
		    (ctx->file->f_cred->user_ns != sb->s_user_ns))
			return -EINVAL;
		ctx->fudptr = &ctx->file->private_data;
	}

	err = fuse_fill_super_common(sb, ctx);
	if (err)
		return err;
	/* file->private_data shall be visible on all CPUs after this */
	smp_mb();
	fuse_send_init(get_fuse_mount_super(sb));
	return 0;
}
static int fuse_set_no_super(struct super_block *sb, struct fs_context *fsc)
{
	return -ENOTCONN;
}
static int fuse_test_super(struct super_block *sb, struct fs_context *fsc)
{

	return fsc->sget_key == get_fuse_conn_super(sb);
}
int ds_userns_fuse_get_tree(struct fs_context *fsc)
{
	struct fuse_fs_context *ctx = fsc->fs_private;
	struct fuse_dev *fud;
	struct fuse_conn *fc;
	struct fuse_mount *fm;
	struct super_block *sb;
	int err;

	/* Preserve the native host ABI, including its eliminated x2 argument. */
	if (fsc->user_ns == &init_user_ns)
		return ds_userns_fuse_get_tree_original(fsc);
	if (ctx->is_bdev || ctx->no_daemon || ctx->root_dir || ctx->root_bpf)
		return -EOPNOTSUPP;

	fc = kmalloc(sizeof(*fc), GFP_KERNEL);
	if (!fc)
		return -ENOMEM;

	fm = kzalloc(sizeof(*fm), GFP_KERNEL);
	if (!fm) {
		kfree(fc);
		return -ENOMEM;
	}

	ds_fuse_conn_init(fc, fm, &init_user_ns, &ds_userns_fuse_iq_ops, NULL);
	/* Only this ported caller supplies a live, explicitly owned namespace. */
	fc->user_ns = get_user_ns(fsc->user_ns);
	fc->release = fuse_free_conn;

	fsc->s_fs_info = fm;

	if (ctx->fd_present)
		ctx->file = fget(ctx->fd);

	if (IS_ENABLED(CONFIG_BLOCK) && ctx->is_bdev) {
		err = get_tree_bdev(fsc, fuse_fill_super);
		goto out;
	}
	/*
	 * While block dev mount can be initialized with a dummy device fd
	 * (found by device name), normal fuse mounts can't
	 */
	err = -EINVAL;
	if (!ctx->file)
		goto out;

	/*
	 * Allow creating a fuse mount with an already initialized fuse
	 * connection
	 */
	fud = READ_ONCE(ctx->file->private_data);
	if (ctx->file->f_op == &fuse_dev_operations && fud) {
		fsc->sget_key = fud->fc;
		sb = sget_fc(fsc, fuse_test_super, fuse_set_no_super);
		err = PTR_ERR_OR_ZERO(sb);
		if (!IS_ERR(sb))
			fsc->root = dget(sb->s_root);
	} else {
		err = get_tree_nodev(fsc, fuse_fill_super);
	}
out:
	if (fsc->s_fs_info)
		fuse_mount_destroy(fm);
	if (ctx->file)
		fput(ctx->file);
	return err;
}
