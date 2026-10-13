/* SPDX-License-Identifier: GPL-2.0-only */
/* Only a checked, position-independent native prologue is copied here. */
.text
.balign 16
.global ds_userns_sk_getsockopt_original
.type ds_userns_sk_getsockopt_original, %function
ds_userns_sk_getsockopt_original:
.rept 8
brk #0
.endr
.size ds_userns_sk_getsockopt_original, .-ds_userns_sk_getsockopt_original
