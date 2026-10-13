// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2020 Facebook; selected task_iter.c callbacks. */
#include <linux/bpf.h>
#include <linux/pid_namespace.h>
struct bpf_iter_seq_task_common {
	struct pid_namespace *ns;
};

int ds_bpf_init_seq_pidns(void *priv_data, struct bpf_iter_aux_info *aux)
{
	struct bpf_iter_seq_task_common *common = priv_data;

	common->ns = get_pid_ns(task_active_pid_ns(current));
	return 0;
}

void ds_bpf_fini_seq_pidns(void *priv_data)
{
	struct bpf_iter_seq_task_common *common = priv_data;

	put_pid_ns(common->ns);
}
