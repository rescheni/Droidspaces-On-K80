// SPDX-License-Identifier: GPL-2.0-only
/* Android common fs/exec.c: restore unmapped set-ID rejection and mm ownership.
 * Local helpers are included because both target kernels inline them into begin_new_exec. */
#include <linux/kernel_read_file.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/swap.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/sched/signal.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/task.h>
#include <linux/page_size_compat.h>
#include <linux/pagemap.h>
#include <linux/perf_event.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include <linux/key.h>
#include <linux/personality.h>
#include <linux/binfmts.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/audit.h>
#include <linux/kmod.h>
#include <linux/fsnotify.h>
#include <linux/fs_struct.h>
#include <linux/oom.h>
#include <linux/compat.h>
#include <linux/vmalloc.h>
#include <linux/io_uring.h>
#include <linux/syscall_user_dispatch.h>
#include <linux/coredump.h>
#include <linux/time_namespace.h>
#include <linux/user_events.h>
#include <linux/rseq.h>
#include <linux/ksm.h>

#include <linux/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/tlb.h>

#include <trace/events/task.h>
#include "fs/internal.h"
#include "../port/port.h"
#include "live.h"

#include <trace/events/sched.h>
#include <trace/hooks/sched.h>

static void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
	struct mm_struct *mm = current->mm;
	long diff = (long)(pages - bprm->vma_pages);

	if (!mm || !diff)
		return;

	bprm->vma_pages = pages;
	add_mm_counter(mm, MM_ANONPAGES, diff);
}

static int exec_mmap(struct mm_struct *mm)
{
	struct task_struct *tsk;
	struct mm_struct *old_mm, *active_mm;
	int ret;

	/* Notify parent that we're no longer interested in the old VM */
	tsk = current;
	old_mm = current->mm;
	exec_mm_release(tsk, old_mm);

	ret = down_write_killable(&tsk->signal->exec_update_lock);
	if (ret)
		return ret;

	if (old_mm) {
		/*
		 * If there is a pending fatal signal perhaps a signal
		 * whose default action is to create a coredump get
		 * out and die instead of going through with the exec.
		 */
		ret = mmap_read_lock_killable(old_mm);
		if (ret) {
			up_write(&tsk->signal->exec_update_lock);
			return ret;
		}
	}

	task_lock(tsk);
	membarrier_exec_mmap(mm);

	local_irq_disable();
	active_mm = tsk->active_mm;
	tsk->active_mm = mm;
	tsk->mm = mm;
	mm_init_cid(mm);
	/*
	 * This prevents preemption while active_mm is being loaded and
	 * it and mm are being updated, which could cause problems for
	 * lazy tlb mm refcounting when these are updated by context
	 * switches. Not all architectures can handle irqs off over
	 * activate_mm yet.
	 */
	if (!IS_ENABLED(CONFIG_ARCH_WANT_IRQS_OFF_ACTIVATE_MM))
		local_irq_enable();
	activate_mm(active_mm, mm);
	if (IS_ENABLED(CONFIG_ARCH_WANT_IRQS_OFF_ACTIVATE_MM))
		local_irq_enable();
	lru_gen_add_mm(mm);
	task_unlock(tsk);
	lru_gen_use_mm(mm);
	if (old_mm) {
		mmap_read_unlock(old_mm);
		BUG_ON(active_mm != old_mm);
		setmax_mm_hiwater_rss(&tsk->signal->maxrss, old_mm);
		mm_update_next_owner(old_mm);
		mmput(old_mm);
		return 0;
	}
	mmdrop_lazy_tlb(active_mm);
	return 0;
}

static int de_thread(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct sighand_struct *oldsighand = tsk->sighand;
	spinlock_t *lock = &oldsighand->siglock;

	if (thread_group_empty(tsk))
		goto no_thread_group;

	/*
	 * Kill all other threads in the thread group.
	 */
	spin_lock_irq(lock);
	if ((sig->flags & SIGNAL_GROUP_EXIT) || sig->group_exec_task) {
		/*
		 * Another group action in progress, just
		 * return so that the signal is processed.
		 */
		spin_unlock_irq(lock);
		return -EAGAIN;
	}

	sig->group_exec_task = tsk;
	sig->notify_count = zap_other_threads(tsk);
	if (!thread_group_leader(tsk))
		sig->notify_count--;

	while (sig->notify_count) {
		__set_current_state(TASK_KILLABLE);
		spin_unlock_irq(lock);
		schedule();
		if (__fatal_signal_pending(tsk))
			goto killed;
		spin_lock_irq(lock);
	}
	spin_unlock_irq(lock);

	/*
	 * At this point all other threads have exited, all we have to
	 * do is to wait for the thread group leader to become inactive,
	 * and to assume its PID:
	 */
	if (!thread_group_leader(tsk)) {
		struct task_struct *leader = tsk->group_leader;

		for (;;) {
			cgroup_threadgroup_change_begin(tsk);
			write_lock_irq(&tasklist_lock);
			/*
			 * Do this under tasklist_lock to ensure that
			 * exit_notify() can't miss ->group_exec_task
			 */
			sig->notify_count = -1;
			if (likely(leader->exit_state))
				break;
			__set_current_state(TASK_KILLABLE);
			write_unlock_irq(&tasklist_lock);
			cgroup_threadgroup_change_end(tsk);
			schedule();
			if (__fatal_signal_pending(tsk))
				goto killed;
		}

		/*
		 * The only record we have of the real-time age of a
		 * process, regardless of execs it's done, is start_time.
		 * All the past CPU time is accumulated in signal_struct
		 * from sister threads now dead.  But in this non-leader
		 * exec, nothing survives from the original leader thread,
		 * whose birth marks the true age of this process now.
		 * When we take on its identity by switching to its PID, we
		 * also take its birthdate (always earlier than our own).
		 */
		tsk->start_time = leader->start_time;
		tsk->start_boottime = leader->start_boottime;

		BUG_ON(!same_thread_group(leader, tsk));
		/*
		 * An exec() starts a new thread group with the
		 * TGID of the previous thread group. Rehash the
		 * two threads with a switched PID, and release
		 * the former thread group leader:
		 */

		/* Become a process group leader with the old leader's pid.
		 * The old leader becomes a thread of the this thread group.
		 */
		exchange_tids(tsk, leader);
		transfer_pid(leader, tsk, PIDTYPE_TGID);
		transfer_pid(leader, tsk, PIDTYPE_PGID);
		transfer_pid(leader, tsk, PIDTYPE_SID);

		list_replace_rcu(&leader->tasks, &tsk->tasks);
		list_replace_init(&leader->sibling, &tsk->sibling);

		tsk->group_leader = tsk;
		leader->group_leader = tsk;

		tsk->exit_signal = SIGCHLD;
		leader->exit_signal = -1;

		BUG_ON(leader->exit_state != EXIT_ZOMBIE);
		leader->exit_state = EXIT_DEAD;
		/*
		 * We are going to release_task()->ptrace_unlink() silently,
		 * the tracer can sleep in do_wait(). EXIT_DEAD guarantees
		 * the tracer won't block again waiting for this thread.
		 */
		if (unlikely(leader->ptrace))
			__wake_up_parent(leader, leader->parent);
		write_unlock_irq(&tasklist_lock);
		cgroup_threadgroup_change_end(tsk);

		release_task(leader);
	}

	sig->group_exec_task = NULL;
	sig->notify_count = 0;

no_thread_group:
	/* we have changed execution domain */
	tsk->exit_signal = SIGCHLD;

	BUG_ON(!thread_group_leader(tsk));
	return 0;

killed:
	/* protects against exit_notify() and __exit_signal() */
	read_lock(&tasklist_lock);
	sig->group_exec_task = NULL;
	sig->notify_count = 0;
	read_unlock(&tasklist_lock);
	return -EAGAIN;
}

static int unshare_sighand(struct task_struct *me)
{
	struct sighand_struct *oldsighand = me->sighand;

	if (refcount_read(&oldsighand->count) != 1) {
		struct sighand_struct *newsighand;
		/*
		 * This ->sighand is shared with the CLONE_SIGHAND
		 * but not CLONE_THREAD task, switch to the new one.
		 */
		newsighand = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
		if (!newsighand)
			return -ENOMEM;

		refcount_set(&newsighand->count, 1);

		write_lock_irq(&tasklist_lock);
		spin_lock(&oldsighand->siglock);
		memcpy(newsighand->action, oldsighand->action,
		       sizeof(newsighand->action));
		rcu_assign_pointer(me->sighand, newsighand);
		spin_unlock(&oldsighand->siglock);
		write_unlock_irq(&tasklist_lock);

		__cleanup_sighand(oldsighand);
	}
	return 0;
}

static void bprm_fill_uid(struct linux_binprm *bprm, struct file *file)
{
	/* Handle suid and sgid on files */
	struct mnt_idmap *idmap;
	struct inode *inode = file_inode(file);
	unsigned int mode;
	vfsuid_t vfsuid;
	vfsgid_t vfsgid;
	int err;

	if (!mnt_may_suid(file->f_path.mnt))
		return;

	if (task_no_new_privs(current))
		return;

	mode = READ_ONCE(inode->i_mode);
	if (!(mode & (S_ISUID|S_ISGID)))
		return;

	idmap = file_mnt_idmap(file);

	/* Be careful if suid/sgid is set */
	inode_lock(inode);

	/* Atomically reload and check mode/uid/gid now that lock held. */
	mode = inode->i_mode;
	vfsuid = i_uid_into_vfsuid(idmap, inode);
	vfsgid = i_gid_into_vfsgid(idmap, inode);
	err = inode_permission(idmap, inode, MAY_EXEC);
	inode_unlock(inode);

	/* Did the exec bit vanish out from under us? Give up. */
	if (err)
		return;

	/* We ignore suid/sgid if there are no mappings for them in the ns */
	if (!vfsuid_has_mapping(bprm->cred->user_ns, vfsuid) ||
	    !vfsgid_has_mapping(bprm->cred->user_ns, vfsgid))
		return;

	if (mode & S_ISUID) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->euid = vfsuid_into_kuid(vfsuid);
	}

	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->egid = vfsgid_into_kgid(vfsgid);
	}
}

static int bprm_creds_from_file(struct linux_binprm *bprm)
{
	/* Compute creds based on which file? */
	struct file *file = bprm->execfd_creds ? bprm->executable : bprm->file;

	bprm_fill_uid(bprm, file);
	return security_bprm_creds_from_file(bprm, file);
}

void ds_userns_would_dump(struct linux_binprm *bprm, struct file *file)
{
	struct inode *inode = file_inode(file);
	struct mnt_idmap *idmap = file_mnt_idmap(file);
	if (inode_permission(idmap, inode, MAY_READ) < 0) {
		struct user_namespace *old, *user_ns;
		bprm->interp_flags |= BINPRM_FLAGS_ENFORCE_NONDUMP;

		/* Ensure mm->user_ns contains the executable */
		user_ns = old = bprm->mm->user_ns;
		while ((user_ns != &init_user_ns) &&
		       !privileged_wrt_inode_uidgid(user_ns, idmap, inode))
			user_ns = user_ns->parent;

		if (old != user_ns) {
			bprm->mm->user_ns = get_user_ns(user_ns);
			put_user_ns(old);
		}
	}
}

int ds_userns_begin_new_exec(struct linux_binprm * bprm)
{
	struct task_struct *me = current;
	int retval;

	/* Once we are committed compute the creds */
	retval = bprm_creds_from_file(bprm);
	if (retval)
		return retval;

	/*
	 * This tracepoint marks the point before flushing the old exec where
	 * the current task is still unchanged, but errors are fatal (point of
	 * no return). The later "sched_process_exec" tracepoint is called after
	 * the current task has successfully switched to the new exec.
	 */
	trace_sched_prepare_exec(current, bprm);

	/*
	 * Ensure all future errors are fatal.
	 */
	bprm->point_of_no_return = true;

	/* Make this the only thread in the thread group */
	retval = de_thread(me);
	if (retval)
		goto out;
	/* see the comment in check_unsafe_exec() */
	current->fs->in_exec = 0;
	/*
	 * Cancel any io_uring activity across execve
	 */
	io_uring_task_cancel();

	/* Ensure the files table is not shared. */
	retval = unshare_files();
	if (retval)
		goto out;

	/*
	 * Must be called _before_ exec_mmap() as bprm->mm is
	 * not visible until then. Doing it here also ensures
	 * we don't race against replace_mm_exe_file().
	 */
	retval = set_mm_exe_file(bprm->mm, bprm->file);
	if (retval)
		goto out;

	/* If the binary is not readable then enforce mm->dumpable=0 */
	ds_userns_would_dump(bprm, bprm->file);
	if (bprm->have_execfd)
		ds_userns_would_dump(bprm, bprm->executable);

	/*
	 * Release all of the old mmap stuff
	 */
	acct_arg_size(bprm, 0);
	retval = exec_mmap(bprm->mm);
	if (retval)
		goto out;

	bprm->mm = NULL;

	retval = exec_task_namespaces();
	if (retval)
		goto out_unlock;

#ifdef CONFIG_POSIX_TIMERS
	spin_lock_irq(&me->sighand->siglock);
	posix_cpu_timers_exit(me);
	spin_unlock_irq(&me->sighand->siglock);
	exit_itimers(me);
	flush_itimer_signals();
#endif

	/*
	 * Make the signal table private.
	 */
	retval = unshare_sighand(me);
	if (retval)
		goto out_unlock;

	me->flags &= ~(PF_RANDOMIZE | PF_FORKNOEXEC |
					PF_NOFREEZE | PF_NO_SETAFFINITY);
	flush_thread();
	me->personality &= ~bprm->per_clear;

	clear_syscall_work_syscall_user_dispatch(me);

	/*
	 * We have to apply CLOEXEC before we change whether the process is
	 * dumpable (in setup_new_exec) to avoid a race with a process in userspace
	 * trying to access the should-be-closed file descriptors of a process
	 * undergoing exec(2).
	 */
	do_close_on_exec(me->files);

	if (bprm->secureexec) {
		/* Make sure parent cannot signal privileged process. */
		me->pdeath_signal = 0;

		/*
		 * For secureexec, reset the stack limit to sane default to
		 * avoid bad behavior from the prior rlimits. This has to
		 * happen before arch_pick_mmap_layout(), which examines
		 * RLIMIT_STACK, but after the point of no return to avoid
		 * needing to clean up the change on failure.
		 */
		if (bprm->rlim_stack.rlim_cur > _STK_LIM)
			bprm->rlim_stack.rlim_cur = _STK_LIM;
	}

	me->sas_ss_sp = me->sas_ss_size = 0;

	/*
	 * Figure out dumpability. Note that this checking only of current
	 * is wrong, but userspace depends on it. This should be testing
	 * bprm->secureexec instead.
	 */
	if (bprm->interp_flags & BINPRM_FLAGS_ENFORCE_NONDUMP ||
	    !(uid_eq(current_euid(), current_uid()) &&
	      gid_eq(current_egid(), current_gid())))
		set_dumpable(current->mm, suid_dumpable);
	else
		set_dumpable(current->mm, SUID_DUMP_USER);

	perf_event_exec();

	/*
	 * If the original filename was empty, alloc_bprm() made up a path
	 * that will probably not be useful to admins running ps or similar.
	 * Let's fix it up to be something reasonable.
	 */
	if (bprm->comm_from_dentry) {
		/*
		 * Hold RCU lock to keep the name from being freed behind our back.
		 * Use acquire semantics to make sure the terminating NUL from
		 * __d_alloc() is seen.
		 *
		 * Note, we're deliberately sloppy here. We don't need to care about
		 * detecting a concurrent rename and just want a terminated name.
		 */
		rcu_read_lock();
		__set_task_comm(me, smp_load_acquire(&bprm->file->f_path.dentry->d_name.name),
				true);
		rcu_read_unlock();
	} else {
		__set_task_comm(me, kbasename(bprm->filename), true);
	}

	/* An exec changes our domain. We are no longer part of the thread
	   group */
	WRITE_ONCE(me->self_exec_id, me->self_exec_id + 1);
	flush_signal_handlers(me, 0);

	retval = set_cred_ucounts(bprm->cred);
	if (retval < 0)
		goto out_unlock;

	/*
	 * install the new credentials for this executable
	 */
	security_bprm_committing_creds(bprm);

	commit_creds(bprm->cred);
	bprm->cred = NULL;

	/*
	 * Disable monitoring for regular users
	 * when executing setuid binaries. Must
	 * wait until new credentials are committed
	 * by commit_creds() above
	 */
	if (get_dumpable(me->mm) != SUID_DUMP_USER)
		perf_event_exit_task(me);
	/*
	 * cred_guard_mutex must be held at least to this point to prevent
	 * ptrace_attach() from altering our determination of the task's
	 * credentials; any time after this it may be unlocked.
	 */
	security_bprm_committed_creds(bprm);

	/* Pass the opened binary to the interpreter. */
	if (bprm->have_execfd) {
		retval = get_unused_fd_flags(0);
		if (retval < 0)
			goto out_unlock;
		fd_install(retval, bprm->executable);
		bprm->executable = NULL;
		bprm->execfd = retval;
	}
	return 0;

out_unlock:
	up_write(&me->signal->exec_update_lock);
	if (!bprm->cred)
		mutex_unlock(&me->signal->cred_guard_mutex);

out:
	return retval;
}
