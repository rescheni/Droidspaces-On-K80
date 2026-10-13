// SPDX-License-Identifier: GPL-2.0-or-later
/* Receive paths from net/unix/af_unix.c with USER_NS mapping enabled. */
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/fcntl.h>
#include <linux/filter.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/af_unix.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/scm.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/rtnetlink.h>
#include <linux/mount.h>
#include <net/checksum.h>
#include <linux/security.h>
#include <linux/splice.h>
#include <linux/freezer.h>
#include <linux/file.h>
#include <linux/btf_ids.h>
#include <linux/bpf-cgroup.h>

#ifdef CONFIG_SECURITY_NETWORK

static inline void unix_set_secdata(struct scm_cookie *scm, struct sk_buff *skb)
{
	scm->secid = UNIXCB(skb).secid;
}

static inline bool unix_secdata_eq(struct scm_cookie *scm, struct sk_buff *skb)
{
	return (scm->secid == UNIXCB(skb).secid);
}
#else

static inline void unix_set_secdata(struct scm_cookie *scm, struct sk_buff *skb)
{ }

static inline bool unix_secdata_eq(struct scm_cookie *scm, struct sk_buff *skb)
{
	return true;
}
#endif /* CONFIG_SECURITY_NETWORK */

static unsigned int unix_skb_len(const struct sk_buff *skb)
{
	return skb->len - UNIXCB(skb).consumed;
}


static void unix_detach_fds(struct scm_cookie *scm, struct sk_buff *skb)
{
	scm->fp = UNIXCB(skb).fp;
	UNIXCB(skb).fp = NULL;

	unix_destroy_fpl(scm->fp);
}


static void unix_peek_fds(struct scm_cookie *scm, struct sk_buff *skb)
{
	scm->fp = scm_fp_dup(UNIXCB(skb).fp);
}


static bool unix_skb_scm_eq(struct sk_buff *skb,
			    struct scm_cookie *scm)
{
	return UNIXCB(skb).pid == scm->pid &&
	       uid_eq(UNIXCB(skb).uid, scm->creds.uid) &&
	       gid_eq(UNIXCB(skb).gid, scm->creds.gid) &&
	       unix_secdata_eq(scm, skb);
}


static void scm_stat_del(struct sock *sk, struct sk_buff *skb)
{
	struct scm_fp_list *fp = UNIXCB(skb).fp;
	struct unix_sock *u = unix_sk(sk);

	if (unlikely(fp && fp->count)) {
		atomic_sub(fp->count, &u->scm_stat.nr_fds);
		unix_del_edges(fp);
	}
}


static void unix_copy_addr(struct msghdr *msg, struct sock *sk)
{
	struct unix_address *addr = smp_load_acquire(&unix_sk(sk)->addr);

	if (addr) {
		msg->msg_namelen = addr->len;
		memcpy(msg->msg_name, addr->name, addr->len);
	}
}


int ds_userns_unix_dgram_recvmsg(struct sock *sk, struct msghdr *msg, size_t size,
			 int flags)
{
	struct scm_cookie scm;
	struct socket *sock = sk->sk_socket;
	struct unix_sock *u = unix_sk(sk);
	struct sk_buff *skb, *last;
	long timeo;
	int skip;
	int err;

	err = -EOPNOTSUPP;
	if (flags&MSG_OOB)
		goto out;

	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	do {
		mutex_lock(&u->iolock);

		skip = sk_peek_offset(sk, flags);
		skb = __skb_try_recv_datagram(sk, &sk->sk_receive_queue, flags,
					      &skip, &err, &last);
		if (skb) {
			if (!(flags & MSG_PEEK))
				scm_stat_del(sk, skb);
			break;
		}

		mutex_unlock(&u->iolock);

		if (err != -EAGAIN)
			break;
	} while (timeo &&
		 !__skb_wait_for_more_packets(sk, &sk->sk_receive_queue,
					      &err, &timeo, last));

	if (!skb) { /* implies iolock unlocked */
		unix_state_lock(sk);
		/* Signal EOF on disconnected non-blocking SEQPACKET socket. */
		if (sk->sk_type == SOCK_SEQPACKET && err == -EAGAIN &&
		    (sk->sk_shutdown & RCV_SHUTDOWN))
			err = 0;
		unix_state_unlock(sk);
		goto out;
	}

	if (wq_has_sleeper(&u->peer_wait))
		wake_up_interruptible_sync_poll(&u->peer_wait,
						EPOLLOUT | EPOLLWRNORM |
						EPOLLWRBAND);

	if (msg->msg_name) {
		unix_copy_addr(msg, skb->sk);

		BPF_CGROUP_RUN_PROG_UNIX_RECVMSG_LOCK(sk,
						      msg->msg_name,
						      &msg->msg_namelen);
	}

	if (size > skb->len - skip)
		size = skb->len - skip;
	else if (size < skb->len - skip)
		msg->msg_flags |= MSG_TRUNC;

	err = skb_copy_datagram_msg(skb, skip, msg, size);
	if (err)
		goto out_free;

	if (sock_flag(sk, SOCK_RCVTSTAMP))
		__sock_recv_timestamp(msg, sk, skb);

	memset(&scm, 0, sizeof(scm));

	scm_set_cred(&scm, UNIXCB(skb).pid, UNIXCB(skb).uid, UNIXCB(skb).gid);
	unix_set_secdata(&scm, skb);

	if (!(flags & MSG_PEEK)) {
		if (UNIXCB(skb).fp)
			unix_detach_fds(&scm, skb);

		sk_peek_offset_bwd(sk, skb->len);
	} else {
		/* It is questionable: on PEEK we could:
		   - do not return fds - good, but too simple 8)
		   - return fds, and do not return them on read (old strategy,
		     apparently wrong)
		   - clone fds (I chose it for now, it is the most universal
		     solution)

		   POSIX 1003.1g does not actually define this clearly
		   at all. POSIX 1003.1g doesn't define a lot of things
		   clearly however!

		*/

		sk_peek_offset_fwd(sk, size);

		if (UNIXCB(skb).fp)
			unix_peek_fds(&scm, skb);
	}
	err = (flags & MSG_TRUNC) ? skb->len - skip : size;

	scm_recv_unix(sock, msg, &scm, flags);

out_free:
	skb_free_datagram(sk, skb);
	mutex_unlock(&u->iolock);
out:
	return err;
}


static long unix_stream_data_wait(struct sock *sk, long timeo,
				  struct sk_buff *last, unsigned int last_len,
				  bool freezable)
{
	unsigned int state = TASK_INTERRUPTIBLE | freezable * TASK_FREEZABLE;
	struct sk_buff *tail;
	DEFINE_WAIT(wait);

	unix_state_lock(sk);

	for (;;) {
		prepare_to_wait(sk_sleep(sk), &wait, state);

		tail = skb_peek_tail(&sk->sk_receive_queue);
		if (tail != last ||
		    (tail && tail->len != last_len) ||
		    sk->sk_err ||
		    (sk->sk_shutdown & RCV_SHUTDOWN) ||
		    signal_pending(current) ||
		    !timeo)
			break;

		sk_set_bit(SOCKWQ_ASYNC_WAITDATA, sk);
		unix_state_unlock(sk);
		timeo = schedule_timeout(timeo);
		unix_state_lock(sk);

		if (sock_flag(sk, SOCK_DEAD))
			break;

		sk_clear_bit(SOCKWQ_ASYNC_WAITDATA, sk);
	}

	finish_wait(sk_sleep(sk), &wait);
	unix_state_unlock(sk);
	return timeo;
}

struct unix_stream_read_state {
	int (*recv_actor)(struct sk_buff *, int, int,
			  struct unix_stream_read_state *);
	struct socket *socket;
	struct msghdr *msg;
	struct pipe_inode_info *pipe;
	size_t size;
	int flags;
	unsigned int splice_flags;
};

#if IS_ENABLED(CONFIG_AF_UNIX_OOB)
static int unix_stream_recv_urg(struct unix_stream_read_state *state)
{
	struct sk_buff *oob_skb, *read_skb = NULL;
	struct socket *sock = state->socket;
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);
	int chunk = 1;

	mutex_lock(&u->iolock);
	unix_state_lock(sk);
	spin_lock(&sk->sk_receive_queue.lock);

	if (sock_flag(sk, SOCK_URGINLINE) || !u->oob_skb) {
		spin_unlock(&sk->sk_receive_queue.lock);
		unix_state_unlock(sk);
		mutex_unlock(&u->iolock);
		return -EINVAL;
	}

	oob_skb = u->oob_skb;

	if (!(state->flags & MSG_PEEK)) {
		WRITE_ONCE(u->oob_skb, NULL);

		if (oob_skb->prev != (struct sk_buff *)&sk->sk_receive_queue &&
		    !unix_skb_len(oob_skb->prev)) {
			read_skb = oob_skb->prev;
			__skb_unlink(read_skb, &sk->sk_receive_queue);
		}
	}

	spin_unlock(&sk->sk_receive_queue.lock);
	unix_state_unlock(sk);

	chunk = state->recv_actor(oob_skb, 0, chunk, state);

	if (!(state->flags & MSG_PEEK))
		UNIXCB(oob_skb).consumed += 1;

	mutex_unlock(&u->iolock);

	consume_skb(read_skb);

	if (chunk < 0)
		return -EFAULT;

	state->msg->msg_flags |= MSG_OOB;
	return 1;
}

static struct sk_buff *manage_oob(struct sk_buff *skb, struct sock *sk,
				  int flags, int copied)
{
	struct sk_buff *read_skb = NULL, *unread_skb = NULL;
	struct unix_sock *u = unix_sk(sk);

	if (likely(unix_skb_len(skb) && skb != READ_ONCE(u->oob_skb)))
		return skb;

	spin_lock(&sk->sk_receive_queue.lock);

	if (!unix_skb_len(skb)) {
		if (copied && (!u->oob_skb || skb == u->oob_skb)) {
			skb = NULL;
		} else if (flags & MSG_PEEK) {
			skb = skb_peek_next(skb, &sk->sk_receive_queue);
		} else {
			read_skb = skb;
			skb = skb_peek_next(skb, &sk->sk_receive_queue);
			__skb_unlink(read_skb, &sk->sk_receive_queue);
		}

		if (!skb)
			goto unlock;
	}

	if (skb != u->oob_skb)
		goto unlock;

	if (copied) {
		skb = NULL;
	} else if (!(flags & MSG_PEEK)) {
		WRITE_ONCE(u->oob_skb, NULL);

		if (!sock_flag(sk, SOCK_URGINLINE)) {
			__skb_unlink(skb, &sk->sk_receive_queue);
			unread_skb = skb;
			skb = skb_peek(&sk->sk_receive_queue);
		}
	} else if (!sock_flag(sk, SOCK_URGINLINE)) {
		skb = skb_peek_next(skb, &sk->sk_receive_queue);
	}

unlock:
	spin_unlock(&sk->sk_receive_queue.lock);

	consume_skb(read_skb);
	kfree_skb(unread_skb);

	return skb;
}
#endif

int ds_userns_unix_stream_read_generic(struct unix_stream_read_state *state,
				    bool freezable)
{
	int noblock = state->flags & MSG_DONTWAIT;
	struct socket *sock = state->socket;
	struct msghdr *msg = state->msg;
	struct sock *sk = sock->sk;
	size_t size = state->size;
	int flags = state->flags;
	bool check_creds = false;
	struct scm_cookie scm;
	unsigned int last_len;
	struct unix_sock *u;
	int copied = 0;
	int err = 0;
	long timeo;
	int target;
	int skip;

	if (unlikely(READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)) {
		err = -EINVAL;
		goto out;
	}

	if (unlikely(flags & MSG_OOB)) {
		err = -EOPNOTSUPP;
#if IS_ENABLED(CONFIG_AF_UNIX_OOB)
		err = unix_stream_recv_urg(state);
#endif
		goto out;
	}

	target = sock_rcvlowat(sk, flags & MSG_WAITALL, size);
	timeo = sock_rcvtimeo(sk, noblock);

	memset(&scm, 0, sizeof(scm));

	u = unix_sk(sk);

redo:
	/* Lock the socket to prevent queue disordering
	 * while sleeps in memcpy_tomsg
	 */
	mutex_lock(&u->iolock);

	skip = max(sk_peek_offset(sk, flags), 0);

	do {
		struct sk_buff *skb, *last;
		int chunk;

		unix_state_lock(sk);
		if (sock_flag(sk, SOCK_DEAD)) {
			err = -ECONNRESET;
			goto unlock;
		}
		last = skb = skb_peek(&sk->sk_receive_queue);
		last_len = last ? last->len : 0;

again:
#if IS_ENABLED(CONFIG_AF_UNIX_OOB)
		if (skb) {
			skb = manage_oob(skb, sk, flags, copied);
			if (!skb && copied) {
				unix_state_unlock(sk);
				break;
			}
		}
#endif
		if (skb == NULL) {
			if (copied >= target)
				goto unlock;

			/*
			 *	POSIX 1003.1g mandates this order.
			 */

			err = sock_error(sk);
			if (err)
				goto unlock;
			if (sk->sk_shutdown & RCV_SHUTDOWN)
				goto unlock;

			unix_state_unlock(sk);
			if (!timeo) {
				err = -EAGAIN;
				break;
			}

			mutex_unlock(&u->iolock);

			timeo = unix_stream_data_wait(sk, timeo, last,
						      last_len, freezable);

			if (signal_pending(current)) {
				err = sock_intr_errno(timeo);
				scm_destroy(&scm);
				goto out;
			}

			goto redo;
unlock:
			unix_state_unlock(sk);
			break;
		}

		while (skip >= unix_skb_len(skb)) {
			skip -= unix_skb_len(skb);
			last = skb;
			last_len = skb->len;
			skb = skb_peek_next(skb, &sk->sk_receive_queue);
			if (!skb)
				goto again;
		}

		unix_state_unlock(sk);

		if (check_creds) {
			/* Never glue messages from different writers */
			if (!unix_skb_scm_eq(skb, &scm))
				break;
		} else if (test_bit(SOCK_PASSCRED, &sock->flags) ||
			   test_bit(SOCK_PASSPIDFD, &sock->flags)) {
			/* Copy credentials */
			scm_set_cred(&scm, UNIXCB(skb).pid, UNIXCB(skb).uid, UNIXCB(skb).gid);
			unix_set_secdata(&scm, skb);
			check_creds = true;
		}

		/* Copy address just once */
		if (msg && msg->msg_name) {
			DECLARE_SOCKADDR(struct sockaddr_un *, sunaddr, msg->msg_name);

			unix_copy_addr(msg, skb->sk);
			BPF_CGROUP_RUN_PROG_UNIX_RECVMSG_LOCK(sk, msg->msg_name,
							      &msg->msg_namelen);

			sunaddr = NULL;
		}

		chunk = min_t(unsigned int, unix_skb_len(skb) - skip, size);
		chunk = state->recv_actor(skb, skip, chunk, state);
		if (chunk < 0) {
			if (copied == 0)
				copied = -EFAULT;
			break;
		}
		copied += chunk;
		size -= chunk;

		/* Mark read part of skb as used */
		if (!(flags & MSG_PEEK)) {
			UNIXCB(skb).consumed += chunk;

			sk_peek_offset_bwd(sk, chunk);

			if (UNIXCB(skb).fp) {
				scm_stat_del(sk, skb);
				unix_detach_fds(&scm, skb);
			}

			if (unix_skb_len(skb))
				break;

			skb_unlink(skb, &sk->sk_receive_queue);
			consume_skb(skb);

			if (scm.fp)
				break;
		} else {
			/* It is questionable, see note in unix_dgram_recvmsg.
			 */
			if (UNIXCB(skb).fp)
				unix_peek_fds(&scm, skb);

			sk_peek_offset_fwd(sk, chunk);

			if (UNIXCB(skb).fp)
				break;

			skip = 0;
			last = skb;
			last_len = skb->len;
			unix_state_lock(sk);
			skb = skb_peek_next(skb, &sk->sk_receive_queue);
			if (skb)
				goto again;
			unix_state_unlock(sk);
			break;
		}
	} while (size);

	mutex_unlock(&u->iolock);
	if (msg)
		scm_recv_unix(sock, msg, &scm, flags);
	else
		scm_destroy(&scm);
out:
	return copied ? : err;
}

int ds_userns_unix_dgram_recvmsg_entry(struct socket *sock, struct msghdr *msg, size_t size,
			      int flags)
{
	struct sock *sk = sock->sk;

#ifdef CONFIG_BPF_SYSCALL
	const struct proto *prot = READ_ONCE(sk->sk_prot);

	if (prot != &unix_dgram_proto)
		return prot->recvmsg(sk, msg, size, flags, NULL);
#endif
	return ds_userns_unix_dgram_recvmsg(sk, msg, size, flags);
}


int ds_userns_unix_seqpacket_recvmsg(struct socket *sock, struct msghdr *msg,
				  size_t size, int flags)
{
	struct sock *sk = sock->sk;

	if (READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
		return -ENOTCONN;

	return ds_userns_unix_dgram_recvmsg_entry(sock, msg, size, flags);
}
