// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NETLINK      Kernel-user communication protocol.
 *
 * 		Authors:	Alan Cox <alan@lxorguk.ukuu.org.uk>
 * 				Alexey Kuznetsov <kuznet@ms2.inr.ac.ru>
 * 				Patrick McHardy <kaber@trash.net>
 *
 * Tue Jun 26 14:36:48 MEST 2001 Herbert "herp" Rosmanith
 *                               added netlink_proto_exit
 * Tue Jan 22 18:32:44 BRST 2002 Arnaldo C. de Melo <acme@conectiva.com.br>
 * 				 use nlk_sk, as sk->protinfo is on a diet 8)
 * Fri Jul 22 19:51:12 MEST 2005 Harald Welte <laforge@gnumonks.org>
 * 				 - inc module use count of module that owns
 * 				   the kernel socket in case userspace opens
 * 				   socket of same protocol
 * 				 - remove all module support, since netlink is
 * 				   mandatory if CONFIG_NET=y these days
 */

#include <linux/module.h>

#include <linux/bpf.h>
#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/filter.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/fcntl.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/notifier.h>
#include <linux/security.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/audit.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/if_arp.h>
#include <linux/rhashtable.h>
#include <asm/cacheflush.h>
#include <linux/hash.h>
#include <linux/net_namespace.h>
#include <linux/nospec.h>
#include <linux/btf_ids.h>

#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/sock.h>
#include <net/scm.h>
#include <net/netlink.h>
#include "net/netlink/af_netlink.h"
#define NETLINK_S_CONGESTED 0x0
extern int netlink_dump(struct sock *, bool);
static inline u32 netlink_group_mask(u32 group)
{
	if (group > 32)
		return 0;
	return group ? 1 << (group - 1) : 0;
}


static void netlink_rcv_wake(struct sock *sk)
{
	struct netlink_sock *nlk = nlk_sk(sk);

	if (skb_queue_empty_lockless(&sk->sk_receive_queue))
		clear_bit(NETLINK_S_CONGESTED, &nlk->state);
	if (!test_bit(NETLINK_S_CONGESTED, &nlk->state))
		wake_up_interruptible(&nlk->wait);
}


static void netlink_cmsg_recv_pktinfo(struct msghdr *msg, struct sk_buff *skb)
{
	struct nl_pktinfo info;

	info.group = NETLINK_CB(skb).dst_group;
	put_cmsg(msg, SOL_NETLINK, NETLINK_PKTINFO, sizeof(info), &info);
}


static void netlink_cmsg_listen_all_nsid(struct sock *sk, struct msghdr *msg,
					 struct sk_buff *skb)
{
	if (!NETLINK_CB(skb).nsid_is_set)
		return;

	put_cmsg(msg, SOL_NETLINK, NETLINK_LISTEN_ALL_NSID, sizeof(int),
		 &NETLINK_CB(skb).nsid);
}


int ds_userns_netlink_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
			   int flags)
{
	struct scm_cookie scm;
	struct sock *sk = sock->sk;
	struct netlink_sock *nlk = nlk_sk(sk);
	size_t copied, max_recvmsg_len;
	struct sk_buff *skb, *data_skb;
	int err, ret;

	if (flags & MSG_OOB)
		return -EOPNOTSUPP;

	copied = 0;

	skb = skb_recv_datagram(sk, flags, &err);
	if (skb == NULL)
		goto out;

	data_skb = skb;

#ifdef CONFIG_COMPAT_NETLINK_MESSAGES
	if (unlikely(skb_shinfo(skb)->frag_list)) {
		/*
		 * If this skb has a frag_list, then here that means that we
		 * will have to use the frag_list skb's data for compat tasks
		 * and the regular skb's data for normal (non-compat) tasks.
		 *
		 * If we need to send the compat skb, assign it to the
		 * 'data_skb' variable so that it will be used below for data
		 * copying. We keep 'skb' for everything else, including
		 * freeing both later.
		 */
		if (flags & MSG_CMSG_COMPAT)
			data_skb = skb_shinfo(skb)->frag_list;
	}
#endif

	/* Record the max length of recvmsg() calls for future allocations */
	max_recvmsg_len = max(READ_ONCE(nlk->max_recvmsg_len), len);
	max_recvmsg_len = min_t(size_t, max_recvmsg_len,
				SKB_WITH_OVERHEAD(32768));
	WRITE_ONCE(nlk->max_recvmsg_len, max_recvmsg_len);

	copied = data_skb->len;
	if (len < copied) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	err = skb_copy_datagram_msg(data_skb, 0, msg, copied);

	if (msg->msg_name) {
		DECLARE_SOCKADDR(struct sockaddr_nl *, addr, msg->msg_name);
		addr->nl_family = AF_NETLINK;
		addr->nl_pad    = 0;
		addr->nl_pid	= NETLINK_CB(skb).portid;
		addr->nl_groups	= netlink_group_mask(NETLINK_CB(skb).dst_group);
		msg->msg_namelen = sizeof(*addr);
	}

	if (nlk_test_bit(RECV_PKTINFO, sk))
		netlink_cmsg_recv_pktinfo(msg, skb);
	if (nlk_test_bit(LISTEN_ALL_NSID, sk))
		netlink_cmsg_listen_all_nsid(sk, msg, skb);

	memset(&scm, 0, sizeof(scm));
	scm.creds = *NETLINK_CREDS(skb);
	if (flags & MSG_TRUNC)
		copied = data_skb->len;

	skb_free_datagram(sk, skb);

	if (READ_ONCE(nlk->cb_running) &&
	    atomic_read(&sk->sk_rmem_alloc) <= sk->sk_rcvbuf / 2) {
		ret = netlink_dump(sk, false);
		if (ret) {
			WRITE_ONCE(sk->sk_err, -ret);
			sk_error_report(sk);
		}
	}

	scm_recv(sock, msg, &scm, flags);
out:
	netlink_rcv_wake(sk);
	return err ? : copied;
}
