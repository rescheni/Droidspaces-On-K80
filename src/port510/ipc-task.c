// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
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

int ds_ipc_backend_init(void)
{
	int ret;
	msg_init_ns(&init_ipc_ns);
	sem_init_ns(&init_ipc_ns);
	shm_init_ns(&init_ipc_ns);
	if (!setup_ipc_sysctls(&init_ipc_ns)) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = ds_mqueue_init();
	if (ret) {
		retire_ipc_sysctls(&init_ipc_ns);
		goto fail;
	}
	ret = ds_ipc_proc_init();
	if (ret) {
		ds_mqueue_exit();
		retire_ipc_sysctls(&init_ipc_ns);
		goto fail;
	}
	return 0;
fail:
	sem_exit_ns(&init_ipc_ns);
	msg_exit_ns(&init_ipc_ns);
	shm_exit_ns(&init_ipc_ns);
	return ret;
}

void ds_ipc_backend_exit(void)
{
	ds_ipc_proc_exit();
	ds_mqueue_exit();
	retire_ipc_sysctls(&init_ipc_ns);
	sem_exit_ns(&init_ipc_ns);
	msg_exit_ns(&init_ipc_ns);
	shm_exit_ns(&init_ipc_ns);
	rcu_barrier();
	WARN_ON(!hash_empty(task_states));
}
