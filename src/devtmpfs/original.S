/* SPDX-License-Identifier: GPL-2.0-only */
/* Populated before hooks become reachable; only checked position-independent prologues. */
.text
.balign 16
.global ds_device_add_original
.type ds_device_add_original, %function
ds_device_add_original:
.rept 8
brk #0
.endr
.size ds_device_add_original, .-ds_device_add_original
.balign 16
.global ds_device_del_original
.type ds_device_del_original, %function
ds_device_del_original:
.rept 8
brk #0
.endr
.size ds_device_del_original, .-ds_device_del_original
