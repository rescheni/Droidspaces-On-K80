/* SPDX-License-Identifier: GPL-2.0-only */
/* Populated before hooks become reachable; only checked position-independent prologues. */
.text
.balign 16
.global ds_perf_alloc_original
.type ds_perf_alloc_original, %function
ds_perf_alloc_original:
.rept 10
brk #0
.endr
.size ds_perf_alloc_original, .-ds_perf_alloc_original
.balign 16
.global ds_perf_free_original
.type ds_perf_free_original, %function
ds_perf_free_original:
.rept 10
brk #0
.endr
.size ds_perf_free_original, .-ds_perf_free_original

.balign 16
.global ds_pidfd_original
.type ds_pidfd_original, %function
ds_pidfd_original:
.rept 10
brk #0
.endr
.size ds_pidfd_original, .-ds_pidfd_original
