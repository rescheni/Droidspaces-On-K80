// SPDX-License-Identifier: GPL-2.0-or-later
/* Namespace-aware peer credentials from net/core/sock.c. */
#include <linux/user_namespace.h>
#include <linux/pid_namespace.h>
#include <linux/socket.h>
#include <net/sock.h>
#include "live.h"
extern int ds_userns_sk_getsockopt_original(struct sock *, int, int, sockptr_t, sockptr_t);
extern const struct cred *sk_get_peer_cred(struct sock *);
int ds_userns_sk_getsockopt(struct sock *sk, int level, int option,
                           sockptr_t value, sockptr_t length)
{
    struct user_namespace *ns = current_user_ns();
    int len;

    if (option != SO_PEERCRED && option != SO_PEERGROUPS)
        return ds_userns_sk_getsockopt_original(sk, level, option, value, length);
    if (copy_from_sockptr(&len, length, sizeof(len)))
        return -EFAULT;
    if (len < 0)
        return -EINVAL;
    if (option == SO_PEERCRED) {
        struct ucred peer;
        len = min_t(int, len, sizeof(peer));
        spin_lock(&sk->sk_peer_lock);
        peer.pid = pid_vnr(sk->sk_peer_pid);
        peer.uid = peer.gid = -1;
        if (sk->sk_peer_cred) {
            peer.uid = from_kuid_munged(ns, sk->sk_peer_cred->euid);
            peer.gid = from_kgid_munged(ns, sk->sk_peer_cred->egid);
        }
        spin_unlock(&sk->sk_peer_lock);
        if (copy_to_sockptr(value, &peer, len))
            return -EFAULT;
    } else {
        const struct cred *cred = sk_get_peer_cred(sk);
        int i, required;
        if (!cred)
            return -ENODATA;
        required = cred->group_info->ngroups * sizeof(gid_t);
        if (len < required) {
            put_cred(cred);
            return copy_to_sockptr(length, &required, sizeof(required)) ? -EFAULT : -ERANGE;
        }
        len = required;
        for (i = 0; i < cred->group_info->ngroups; i++) {
            gid_t gid = from_kgid_munged(ns, cred->group_info->gid[i]);
            if (copy_to_sockptr_offset(value, i * sizeof(gid), &gid, sizeof(gid))) {
                put_cred(cred);
                return -EFAULT;
            }
        }
        put_cred(cred);
    }
    return copy_to_sockptr(length, &len, sizeof(len)) ? -EFAULT : 0;
}
