/* SPDX-License-Identifier: GPL-2.0-only */
#include "names.h"
#define copy_creds ds_userns_copy_creds
#define prepare_kernel_cred ds_userns_prepare_kernel_cred
#define alloc_ucounts ds_un_alloc_ucounts
#define put_ucounts ds_un_put_ucounts
#define get_ucounts ds_un_get_ucounts
#define inc_ucount ds_un_inc_ucount
#define dec_ucount ds_un_dec_ucount
#define inc_rlimit_ucounts ds_un_inc_rlimit_ucounts
#define dec_rlimit_ucounts ds_un_dec_rlimit_ucounts
#define inc_rlimit_get_ucounts ds_un_inc_rlimit_get_ucounts
#define dec_rlimit_put_ucounts ds_un_dec_rlimit_put_ucounts
#define is_rlimit_overlimit ds_un_is_rlimit_overlimit
#define put_fs_context ds_userns_put_fs_context
#define vfs_dup_fs_context ds_userns_vfs_dup_fs_context
#define sget_fc ds_userns_sget_fc
#define sget ds_userns_sget

#define free_time_ns ds_userns_free_time_ns
