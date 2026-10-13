// SPDX-License-Identifier: GPL-2.0-only
/* Offline fixture only: invoke the vendor callback without installing hooks. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <asm/patching.h>
extern void send_signal_catcher(void *, int, struct task_struct *, struct task_struct *);
static bool bad;
module_param(bad, bool, 0400);
static int exercise(const char *value, const struct kernel_param *kp)
{
	struct task_struct *task;
	struct pid *pid;
	int nr, ret = kstrtoint(value, 0, &nr);
	if (ret)
		return ret;
	pid = find_get_pid(nr);
	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);
	if (!task)
		return -ESRCH;
	if (task->pid == task->tgid) {
		put_task_struct(task);
		return -EINVAL;
	}
	rcu_read_lock();
	send_signal_catcher(NULL, SIGKILL, current, task);
	rcu_read_unlock();
	put_task_struct(task);
	return 0;
}
static const struct kernel_param_ops exercise_ops = { .set = exercise };
module_param_cb(exercise, &exercise_ops, NULL, 0200);
static int __init fixture_init(void)
{
	if (!of_machine_is_compatible("linux,dummy-virt"))
		return -EOPNOTSUPP;
	if (bad)
		return aarch64_insn_patch_text_nosync((void *)send_signal_catcher, 0x7100803f);
	return 0;
}
static void __exit fixture_exit(void) {}
module_init(fixture_init);
module_exit(fixture_exit);
MODULE_LICENSE("GPL");
