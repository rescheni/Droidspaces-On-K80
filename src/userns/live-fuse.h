/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_USERNS_LIVE_FUSE_H
#define DS_USERNS_LIVE_FUSE_H
#include "fs/fuse/fuse_i.h"
struct fs_parameter;
struct fuse_req *ds_userns_fuse_get_req(struct mnt_idmap *, struct fuse_mount *, bool);
ssize_t ds_userns_fuse_simple_request(struct mnt_idmap *, struct fuse_mount *, struct fuse_args *);
int ds_userns_fuse_parse_param(struct fs_context *, struct fs_parameter *);
int ds_userns_fuse_show_options(struct seq_file *, struct dentry *);
void ds_userns_fuse_change_attributes_common(struct inode *, struct fuse_attr *, struct fuse_statx *, u64, u32);
void ds_userns_fuse_fillattr(struct mnt_idmap *, struct inode *, struct fuse_attr *, struct kstat *);
bool ds_userns_fuse_allow_current_process(struct fuse_conn *);
int ds_userns_fuse_do_setattr(struct mnt_idmap *, struct dentry *, struct iattr *, struct file *);
int ds_userns_fuse_get_create_ext(struct mnt_idmap *, struct fuse_args *, struct inode *, struct dentry *, umode_t);
struct posix_acl *ds_userns_fuse_get_acl(struct mnt_idmap *, struct dentry *, int);
struct posix_acl *ds_userns_fuse_get_inode_acl(struct inode *, int, bool);
int ds_userns_fuse_set_acl(struct mnt_idmap *, struct dentry *, struct posix_acl *, int);
int ds_userns_fuse_permission(struct mnt_idmap *, struct inode *, int);
int ds_userns_fuse_setattr(struct mnt_idmap *, struct dentry *, struct iattr *);
int ds_userns_fuse_getattr(struct mnt_idmap *, const struct path *, struct kstat *, u32, unsigned int);
int ds_userns_fuse_update_get_attr(struct mnt_idmap *, struct inode *, struct file *, const struct path *, struct kstat *, u32, unsigned int);
int ds_userns_fuse_do_set_acl(struct mnt_idmap *, struct dentry *, const char *, const void *, size_t);
ssize_t ds_userns_fuse_do_get_acl(struct mnt_idmap *, struct dentry *, const char *, void *, size_t);
#endif
