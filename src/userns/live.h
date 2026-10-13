/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_USERNS_LIVE_H
#define DS_USERNS_LIVE_H
#include "test.h"
#include <linux/fs.h>
#include <linux/sockptr.h>
#include <linux/fs_context.h>
void ds_userns_key_change_session_keyring(struct callback_head *);
void ds_userns_put_cred_rcu(struct rcu_head *rcu);
int ds_userns_proc_prepare(void);
void ds_userns_proc_cleanup(void);
struct dentry *ds_userns_tgid_lookup(struct inode *, struct dentry *, unsigned int);
struct dentry *ds_userns_tid_lookup(struct inode *, struct dentry *, unsigned int);
int ds_userns_tgid_readdir(struct file *, struct dir_context *);
int ds_userns_tid_readdir(struct file *, struct dir_context *);
struct mnt_namespace;
struct user_namespace;
struct mnt_namespace *ds_userns_alloc_mnt_ns(struct user_namespace *, bool);
void ds_userns_mnt_ns_release(struct mnt_namespace *);
void ds_userns_free_mnt_ns(struct mnt_namespace *);
struct fs_context *ds_userns_alloc_fs_context(struct file_system_type *,
    struct dentry *, unsigned int, unsigned int, enum fs_context_purpose);
struct super_block *ds_userns_alloc_super(struct file_system_type *, int,
    struct user_namespace *);
void ds_userns_destroy_super_work(struct work_struct *);
struct stat;
struct statx;
int ds_userns_cp_statx(const struct kstat *, struct statx __user *);
struct linux_binprm;
struct cpu_vfs_cap_data;
int ds_userns_cap_task_fix_setuid(struct cred *, const struct cred *, int);
int ds_userns_cap_bprm_creds_from_file(struct linux_binprm *, const struct file *);
int ds_userns_get_vfs_caps_from_disk(struct mnt_idmap *, const struct dentry *, struct cpu_vfs_cap_data *);
bool ds_userns_mnt_may_suid(struct vfsmount *);
int ds_userns_begin_new_exec(struct linux_binprm *);
void ds_userns_would_dump(struct linux_binprm *, struct file *);
struct mm_struct *ds_userns_mm_init(struct mm_struct *, struct task_struct *, struct user_namespace *);
void ds_userns_mmdrop(struct mm_struct *);
long ds_userns_do_faccessat(int, const char __user *, int, int);
int ds_userns_chown_common(const struct path *, uid_t, gid_t);
bool ds_userns_privileged_wrt_inode_uidgid(struct user_namespace *, struct mnt_idmap *, const struct inode *);
bool ds_userns_capable_wrt_inode_uidgid(struct mnt_idmap *, const struct inode *, int);
bool ds_userns_inode_owner_or_capable(struct mnt_idmap *, const struct inode *);
struct ctl_table_set *ds_userns_set_lookup(struct ctl_table_root *);
int ds_userns_set_is_seen(struct ctl_table_set *);
int ds_userns_cap_inode_getsecurity(struct mnt_idmap *, struct inode *, const char *, void **, bool);
int ds_userns_cap_convert_nscap(struct mnt_idmap *, struct dentry *, const void **, size_t);
long ds_userns_ns_ioctl(struct file *, unsigned int, unsigned long);
struct net;
void ds_userns_net_ns_get_ownership(const struct net *, kuid_t *, kgid_t *);
int ds_userns_proc_pid_status(struct seq_file *, struct pid_namespace *, struct pid *, struct task_struct *);
struct cgroup_namespace;
struct time_namespace;
struct cgroup_namespace *ds_userns_copy_cgroup_ns(unsigned long, struct user_namespace *, struct cgroup_namespace *);
void ds_userns_free_cgroup_ns(struct cgroup_namespace *);
struct time_namespace *ds_userns_copy_time_ns(unsigned long, struct user_namespace *, struct time_namespace *);
void ds_userns_free_time_ns(struct time_namespace *);
int ds_userns_cgroup_init_fs_context(struct fs_context *);
int ds_userns_sysfs_init_fs_context(struct fs_context *);
struct nsset;
struct nsproxy;
void ds_userns_timens_put(struct ns_common *);
int ds_userns_timens_install(struct nsset *, struct ns_common *);
void ds_userns_timens_on_fork(struct nsproxy *, struct task_struct *);
struct sock;
struct socket;
struct msghdr;
struct scm_cookie;
struct unix_stream_read_state;
int ds_userns_sk_getsockopt(struct sock *, int, int, sockptr_t, sockptr_t);
int ds_userns_scm_send(struct socket *, struct msghdr *, struct scm_cookie *);
int ds_userns_unix_dgram_recvmsg(struct sock *, struct msghdr *, size_t, int);
int ds_userns_unix_stream_read_generic(struct unix_stream_read_state *, bool);
int ds_userns_unix_dgram_recvmsg_entry(struct socket *, struct msghdr *, size_t, int);
int ds_userns_unix_seqpacket_recvmsg(struct socket *, struct msghdr *, size_t, int);
int ds_userns_fuse_get_tree(struct fs_context *);
int ds_userns_netlink_recvmsg(struct socket *, struct msghdr *, size_t, int);
#endif
