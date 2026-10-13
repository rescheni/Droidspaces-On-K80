/* SPDX-License-Identifier: GPL-2.0-only */
/* Only a checked, position-independent native prologue is copied here. */
.text
.balign 16
.global ds_userns_fuse_get_tree_original
.type ds_userns_fuse_get_tree_original, %function
ds_userns_fuse_get_tree_original:
.rept 8
brk #0
.endr
.size ds_userns_fuse_get_tree_original, .-ds_userns_fuse_get_tree_original

.balign 16
.global ds_userns_fuse_simple_request_original
.type ds_userns_fuse_simple_request_original, %function
ds_userns_fuse_simple_request_original:
.rept 8
brk #0
.endr
.size ds_userns_fuse_simple_request_original, .-ds_userns_fuse_simple_request_original

.balign 16
.global ds_userns_fuse_do_set_acl_original
.type ds_userns_fuse_do_set_acl_original, %function
ds_userns_fuse_do_set_acl_original:
.rept 8
brk #0
.endr
 .size ds_userns_fuse_do_set_acl_original, .-ds_userns_fuse_do_set_acl_original

.balign 16
.global ds_userns_fuse_do_get_acl_original
.type ds_userns_fuse_do_get_acl_original, %function
ds_userns_fuse_do_get_acl_original:
.rept 8
brk #0
.endr
 .size ds_userns_fuse_do_get_acl_original, .-ds_userns_fuse_do_get_acl_original
