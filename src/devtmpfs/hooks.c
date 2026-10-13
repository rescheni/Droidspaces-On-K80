// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_devtmpfs: " fmt
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/stop_machine.h>
#include <asm/patching.h>
#include "core.h"
#include "port.h"

struct ds_hook {
	const char *name;
	unsigned long replacement;
	unsigned long address;
	u32 saved[4];
	u32 patch[4];
	u32 *original;
};

static struct ds_hook hooks[] = {
	{ "device_add", (unsigned long)ds_device_add, 0, {0}, {0}, (u32 *)ds_device_add_original },
	{ "device_del", (unsigned long)ds_device_del, 0, {0}, {0}, (u32 *)ds_device_del_original },
};

static bool movable_prologue(u32 op)
{
	/* NOP/PACIASP; stack allocation/frame pointer; stack stores, no PC operands. */
	return op == 0xd503201f || op == 0xd503233f ||
		op == 0xf800865e || /* SCS push */
		(op & 0xffc003ff) == 0xd10003ff ||
		(op & 0xffc003ff) == 0x910003fd ||
		(op & 0xffc003e0) == 0xf90003e0 || /* STR xN,[sp,#imm] */
		(op & 0xff8003e0) == 0xa90003e0 ||
		(op & 0xff8003e0) == 0xa98003e0;
}

static int prepare_hook(struct ds_hook *hook)
{
	char symbol[KSYM_SYMBOL_LEN];
	char *start, *end;
	unsigned long offset, size;

	hook->address = kallsyms_name_to_addr(hook->name);
	if (!hook->address || (hook->address & 3) ||
	    safe_read(hook->saved, (void *)hook->address, sizeof(hook->saved)))
		return -ENOENT;
	/* Direct entry branches preserve the host prefix. Local direct-call-only
	 * functions can lack a KCFI word; compare hashes only for pointer slots.
	 */
	sprint_symbol(symbol, hook->address);
	start = strchr(symbol, '+');
	if (!start)
		return -EINVAL;
	offset = simple_strtoul(start + 1, &end, 0);
	if (end == start + 1 || *end != '/')
		return -EINVAL;
	start = end + 1;
	size = simple_strtoul(start, &end, 0);
	if (end == start || offset || size < sizeof(hook->saved))
		return -EINVAL;
	if (hook->original) {
		unsigned int i;
		for (i = 0; i < ARRAY_SIZE(hook->saved); i++) {
			if (!movable_prologue(hook->saved[i])) {
				pr_err("unsupported original prologue %s word %u: %08x\n",
				       hook->name, i, hook->saved[i]);
				return -EOPNOTSUPP;
			}
		}
	}
	/* ldr x16, .+8; br x16; .quad destination. CFI type prefixes stay intact. */
	hook->patch[0] = 0x58000050;
	hook->patch[1] = 0xd61f0200;
	hook->patch[2] = lower_32_bits(hook->replacement);
	hook->patch[3] = upper_32_bits(hook->replacement);
	return 0;
}

static int write_hook(struct ds_hook *hook, bool restore)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hook->saved); i++) {
		ret = aarch64_insn_patch_text_nosync((void *)(hook->address + i * 4),
					 restore ? hook->saved[i] : hook->patch[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int install_stopped(void *unused)
{
	unsigned int i, j;
	int ret;
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		struct ds_hook *h = &hooks[i];
		u32 code[8];
		memcpy(code, h->saved, sizeof(h->saved));
		code[4] = 0x58000050;
		code[5] = 0xd61f0200;
		code[6] = lower_32_bits(h->address + sizeof(h->saved));
		code[7] = upper_32_bits(h->address + sizeof(h->saved));
		for (j = 0; j < ARRAY_SIZE(code); j++) {
			ret = aarch64_insn_patch_text_nosync(&h->original[j], code[j]);
			if (ret) return ret; /* No native entry changed yet. */
		}
	}
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		ret = write_hook(&hooks[i], false);
		if (ret) {
			for (j = 0; j <= i; j++)
				if (write_hook(&hooks[j], true))
					panic("droidspaces_devtmpfs: rollback failed");
			return ret;
		}
	}
	return 0;
}

int ds_hooks_prepare(void)
{
	unsigned int i;
	if (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL)) return -EOPNOTSUPP;
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		int ret = prepare_hook(&hooks[i]);
		if (ret) return ret;
	}
	return 0;
}

int ds_hooks_install(void)
{
	return stop_machine(install_stopped, NULL, cpu_online_mask);
}
