// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static ssize_t fixture_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	static const char value[] = "devtmpfs-ok\n";
	return simple_read_from_buffer(buf, len, pos, value, sizeof(value) - 1);
}
static const struct file_operations fixture_ops = {
	.owner = THIS_MODULE,
	.read = fixture_read,
};
static struct miscdevice fixture = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ds_devtmpfs_test",
	.nodename = "droidspaces/testnode",
	.mode = 0660,
	.fops = &fixture_ops,
};
static bool online;
static int set_online(const char *value, const struct kernel_param *param)
{
	bool next;
	int ret = kstrtobool(value, &next);
	if (ret || next == online) return ret;
	if (next) {
		ret = misc_register(&fixture);
		if (ret) return ret;
	} else misc_deregister(&fixture);
	online = next;
	return 0;
}
static const struct kernel_param_ops online_ops = { .set = set_online, .get = param_get_bool };
module_param_cb(online, &online_ops, &online, 0600);
static int __init fixture_init(void) { return 0; }
static void __exit fixture_exit(void) { if (online) misc_deregister(&fixture); }
module_init(fixture_init);
module_exit(fixture_exit);
MODULE_LICENSE("GPL");
