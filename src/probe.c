// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_probe: " fmt

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "core.h"
#include "hint.h"

static unsigned int timeout_ms = 5000;
module_param(timeout_ms, uint, 0400);
MODULE_PARM_DESC(timeout_ms, "KallRecon discovery timeout in ms (1..30000)");

static int __init droidspaces_probe_init(void)
{
	static const char *const symbols[] = {
		"copy_namespaces", "copy_pid_ns", "copy_ipcs", "free_ipc_ns",
		"init_ipc_ns", "ipcns_operations", "pidns_operations",
		"__arm64_sys_shmget", "__arm64_sys_semget", "__arm64_sys_msgget",
		"__arm64_sys_mq_open", "devtmpfs_mount",
	};
	struct kallrecon_hint hint = { .timeout_ms = timeout_ms };
	unsigned int i;
	unsigned long ni_syscall;
	int ret;

	if (!timeout_ms || timeout_ms > 30000)
		return -EINVAL;
	ret = kallrecon_supply(&hint);
	if (ret)
		return ret;
	find_kallsyms_base();
	if (kallrecon_fail_reason() != KALLRECON_OK || !klnum_val ||
	    !kallrecon_klp) {
		pr_err("symbol discovery failed: reason=%d\n",
		       kallrecon_fail_reason());
		return -ENODATA;
	}
	/* Use the local decoder, without calling functions through recovered pointers. */
	if (kallsyms_name_to_addr("sprint_symbol") != sprint_addr) {
		pr_err("symbol discovery failed anchor verification\n");
		return -EINVAL;
	}
	pr_info("KallRecon ready: symbols=%u layout=%d\n", klnum_val, kl_layout);
	ni_syscall = kallsyms_name_to_addr("__arm64_sys_ni_syscall");
	for (i = 0; i < ARRAY_SIZE(symbols); i++) {
		unsigned long addr = kallsyms_name_to_addr(symbols[i]);

		pr_info("symbol %s: %s\n", symbols[i], !addr ? "not found" :
			addr == ni_syscall ? "ENOSYS stub" : "present (not functionally tested)");
	}
	pr_info("probe only; no namespace or IPC functionality was installed\n");
	return 0;
}

static void __exit droidspaces_probe_exit(void)
{
	pr_info("unloaded\n");
}

module_init(droidspaces_probe_init);
module_exit(droidspaces_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Droidspaces prerequisite symbol probe using KallRecon");
