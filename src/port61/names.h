/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_PORT_NAMES_H
#define DS_PORT_NAMES_H

/* Keep the transplanted implementation separate from the running kernel. */
#define copy_pid_ns ds_copy_pid_ns
#define put_pid_ns ds_put_pid_ns
#define zap_pid_ns_processes ds_zap_pid_ns_processes
#define reboot_pid_ns ds_reboot_pid_ns
#define pidns_operations ds_pidns_operations
#define pidns_for_children_operations ds_pidns_for_children_operations
#define copy_ipcs ds_copy_ipcs
#define put_ipc_ns ds_put_ipc_ns
#define free_ipcs ds_free_ipcs
#define ipcns_operations ds_ipcns_operations
#define copy_namespaces ds_copy_namespaces
#define free_nsproxy ds_free_nsproxy
#define unshare_nsproxy_namespaces ds_unshare_nsproxy_namespaces
#define switch_task_namespaces ds_switch_task_namespaces
#define exit_task_namespaces ds_exit_task_namespaces
#define exec_task_namespaces ds_exec_task_namespaces
#define alloc_pid ds_alloc_pid
#define put_pid ds_put_pid
#define do_exit ds_do_exit
#define release_task ds_release_task
#define rcuwait_wake_up ds_rcuwait_wake_up
#define mm_update_next_owner ds_mm_update_next_owner
#define proc_ns_dir_operations ds_proc_ns_dir_operations
#define proc_ns_dir_inode_operations ds_proc_ns_dir_inode_operations

#endif
