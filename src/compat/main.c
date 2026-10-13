// SPDX-License-Identifier: GPL-2.0-only
/* Balance the PID reference leaked by the validated Oplus signal callback. */
#define pr_fmt(fmt) "droidspaces_compat: " fmt
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pid.h>
#include <linux/stop_machine.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <asm/patching.h>

#include "oplus-signal-fingerprint.h"

extern struct mutex module_mutex;

static struct module *vendor;
static unsigned long callback, callsite;
static u32 saved_call, replacement_call;
static atomic_long_t balanced = ATOMIC_LONG_INIT(0);

static int balanced_get(char *buffer, const struct kernel_param *param)
{
	return scnprintf(buffer, PAGE_SIZE, "%ld\n", atomic_long_read(&balanced));
}
static const struct kernel_param_ops balanced_ops = { .get = balanced_get };
module_param_cb(balanced, &balanced_ops, NULL, 0400);

/* The tracepoint's RCU read-side section and the target's live thread group
 * protect the borrowed task returned by pid_task(). The vendor callback uses
 * only that task afterwards; it discards the owned PID returned by get_task_pid.
 * Consume exactly that reference here, including the pid_task() == NULL case.
 */
static struct task_struct *pid_task_put(struct pid *pid, enum pid_type type)
{
	struct task_struct *task = pid_task(pid, type);

	put_pid(pid);
	atomic_long_inc(&balanced);
	return task;
}

static int read_code(unsigned long address, void *buffer, size_t size)
{
	return copy_from_kernel_nofault(buffer, (const void *)address, size);
}

/* ARM64 module PLT: ADRP x16; ADD x16,x16,#lo12; BR x16. */
static bool calls(unsigned long pc, unsigned long expected)
{
	u32 op, plt[3];
	unsigned long target;
	s64 pages;

	if (read_code(pc, &op, sizeof(op)) || (op & 0xfc000000) != 0x94000000)
		return false;
	target = pc + sign_extend64(op & 0x03ffffff, 25) * 4;
	if (target == expected)
		return true;
	if (!within_module_core(target, vendor) || read_code(target, plt, sizeof(plt)) ||
	    (plt[0] & 0x9f00001f) != 0x90000010 ||
	    (plt[1] & 0xffc003ff) != 0x91000210 || plt[2] != 0xd61f0200)
		return false;
	pages = sign_extend64(((plt[0] >> 5) & 0x7ffff) << 2 |
			      ((plt[0] >> 29) & 3), 20);
	return (target & ~4095UL) + pages * 4096 +
		((plt[1] >> 10) & 4095) == expected;
}

static int validate_callback(void)
{
	unsigned long size, offset;
	unsigned int i;
	u32 word;
	s64 displacement;

	if (!kallsyms_lookup_size_offset(callback, &size, &offset) || offset ||
	    size != ARRAY_SIZE(signal_fingerprint) * sizeof(u32))
		return -EOPNOTSUPP;
	for (i = 0; i < ARRAY_SIZE(signal_fingerprint); i++) {
		if (read_code(callback + i * sizeof(u32), &word, sizeof(word)) ||
		    (word & signal_fingerprint[i].mask) != signal_fingerprint[i].opcode)
			return -EOPNOTSUPP;
	}
	if (!calls(callback + 0xcc, (unsigned long)get_task_pid) ||
	    !calls(callback + 0xd8, (unsigned long)pid_task))
		return -EOPNOTSUPP;
	callsite = callback + 0xd8;
	if (read_code(callsite, &saved_call, sizeof(saved_call)))
		return -EFAULT;
	displacement = (unsigned long)pid_task_put - callsite;
	if ((displacement & 3) || displacement < -(1LL << 27) ||
	    displacement >= (1LL << 27))
		return -ERANGE;
	replacement_call = 0x94000000 | ((displacement >> 2) & 0x03ffffff);
	return 0;
}

static int install_stopped(void *unused)
{
	u32 current_call;

	if (read_code(callsite, &current_call, sizeof(current_call)) ||
	    current_call != saved_call)
		return -EBUSY;
	return aarch64_insn_patch_text_nosync((void *)callsite, replacement_call);
}

static int __init compat_init(void)
{
	bool vm = of_machine_is_compatible("linux,dummy-virt");
	int ret;

	if (!vm && (!of_machine_is_compatible("qcom,canoe") ||
	    strcmp(utsname()->release, "6.12.69-android16-6-g242e1a07f878-ab15865446-4k")))
		return -EOPNOTSUPP;
	/* Hold the exact vendor module while resolving, validating and patching it. */
	mutex_lock(&module_mutex);
	vendor = find_module("oplus_sys_stability_helper");
	if (vendor && !try_module_get(vendor))
		vendor = NULL;
	mutex_unlock(&module_mutex);
	if (!vendor)
		return -ENODEV;
	callback = module_kallsyms_lookup_name("oplus_sys_stability_helper:send_signal_catcher");
	if (!callback || !within_module_core(callback, vendor)) {
		ret = -ENOENT;
		goto fail;
	}
	ret = validate_callback();
	if (ret)
		goto fail;
	ret = stop_machine(install_stopped, NULL, NULL);
	if (ret)
		goto fail;
	pr_info("Oplus signal callback PID ownership corrected; active until reboot\n");
	return 0;
fail:
	module_put(vendor);
	vendor = NULL;
	pr_err("vendor callback validation/installation failed: %d\n", ret);
	return ret;
}
/* No unload: a patched vendor callback must never call freed module text. */
module_init(compat_init);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shikochin");
MODULE_DESCRIPTION("PID reference fix for the validated PLZ110 Oplus signal callback");
