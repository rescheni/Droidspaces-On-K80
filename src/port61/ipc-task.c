// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include "ipc-backend.h"
#include "ipc-util.h"

static DEFINE_HASHTABLE(task_states, 8);
static DEFINE_SPINLOCK(task_states_lock);
atomic_t ds_ipc_task_live = ATOMIC_INIT(0);
bool ds_ipc_active;

struct ds_ipc_task *ds_ipc_task_find(struct task_struct *task)
{
	struct ds_ipc_task *state;
	unsigned long flags;
	spin_lock_irqsave(&task_states_lock, flags);
	hash_for_each_possible(task_states, state, node, (unsigned long)task) {
		if (state->task == task) {
			spin_unlock_irqrestore(&task_states_lock, flags);
			return state;
		}
	}
	spin_unlock_irqrestore(&task_states_lock, flags);
	return NULL;
}

void ds_ipc_task_install(struct ds_ipc_task *state, struct task_struct *task)
{
	unsigned long flags;
	state->task = task;
	INIT_LIST_HEAD(&state->shm_clist);
	spin_lock_irqsave(&task_states_lock, flags);
	hash_add(task_states, &state->node, (unsigned long)task);
	atomic_inc(&ds_ipc_task_live);
	spin_unlock_irqrestore(&task_states_lock, flags);
}

struct ds_ipc_task *ds_ipc_task_get_current(void)
{
	struct ds_ipc_task *state = ds_ipc_task_find(current);
	if (state)
		return state;
	state = kzalloc(sizeof(*state), GFP_KERNEL_ACCOUNT);
	if (state)
		ds_ipc_task_install(state, current);
	return state;
}

void ds_ipc_task_release(struct task_struct *task)
{
	struct ds_ipc_task *state = ds_ipc_task_find(task);
	unsigned long flags;
	if (!state)
		return;
	WARN_ON(state->undo_list || !list_empty(&state->shm_clist));
	spin_lock_irqsave(&task_states_lock, flags);
	hash_del(&state->node);
	atomic_dec(&ds_ipc_task_live);
	spin_unlock_irqrestore(&task_states_lock, flags);
	kfree(state);
}

/* Native POSIX queues and IPC namespaces retain their original identity. */
static DEFINE_MUTEX(ipc_init_lock);
extern atomic_t ds_ipc_live;

int ds_ipc_ensure(struct ipc_namespace *ns)
{
	int ret = 0;

	if (smp_load_acquire(&ns->ipc_sysctls))
		return 0;
	mutex_lock(&ipc_init_lock);
	if (ns->ipc_sysctls)
		goto out;
	ret = msg_init_ns(ns);
	if (ret)
		goto out;
	sem_init_ns(ns);
	shm_init_ns(ns);
	if (!setup_ipc_sysctls(ns)) {
		sem_exit_ns(ns);
		msg_exit_ns(ns);
		shm_exit_ns(ns);
		ret = -ENOMEM;
		goto out;
	}
	if (ns != &init_ipc_ns) {
		__module_get(THIS_MODULE);
		atomic_inc(&ds_ipc_live);
	}
out:
	mutex_unlock(&ipc_init_lock);
	return ret;
}

int ds_ipc_backend_init(void)
{
	int ret;
	ds_ipc_empty_set_init();
	ret = ds_ipc_ensure(&init_ipc_ns);
	if (ret)
		return ret;
	ret = ds_ipc_proc_init();
	if (ret) {
		retire_ipc_sysctls(&init_ipc_ns);
		init_ipc_ns.ipc_sysctls = NULL;
		sem_exit_ns(&init_ipc_ns);
		msg_exit_ns(&init_ipc_ns);
		shm_exit_ns(&init_ipc_ns);
	}
	return ret;
}

void ds_ipc_backend_exit(void)
{
	ds_ipc_proc_exit();
	retire_ipc_sysctls(&init_ipc_ns);
	init_ipc_ns.ipc_sysctls = NULL;
	sem_exit_ns(&init_ipc_ns);
	msg_exit_ns(&init_ipc_ns);
	shm_exit_ns(&init_ipc_ns);
	rcu_barrier();
	WARN_ON(!hash_empty(task_states));
}
