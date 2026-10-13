// SPDX-License-Identifier: GPL-2.0-only
/* The 5.10 baseline predates per-PID-namespace memfd execution policy. */
int ds_memfd_init(void) { return 0; }
void ds_memfd_exit(void) { }
