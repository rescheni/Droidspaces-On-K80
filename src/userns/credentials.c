// SPDX-License-Identifier: GPL-2.0-only
/* Android common kernel/cred.c. Private credentials in the core-test target;
 * native task/file credential lifecycle replacements in the live VM target. */
#include <linux/cred.h>
#include <linux/user_namespace.h>
#include <linux/security.h>
#include <linux/key.h>
#include <linux/slab.h>
#include "test.h"

extern struct kmem_cache *cred_jar;
static atomic_t live_creds = ATOMIC_INIT(0);

void ds_userns_put_cred_rcu(struct rcu_head *rcu)
{
	struct cred *cred = container_of(rcu, struct cred, rcu);

	if (atomic_long_read(&cred->usage) != 0)
		panic("CRED: put_cred_rcu() sees %p with usage %ld\n",
		      cred, atomic_long_read(&cred->usage));

	security_cred_free(cred);
	key_put(cred->session_keyring);
	key_put(cred->process_keyring);
	key_put(cred->thread_keyring);
	key_put(cred->request_key_auth);
	if (cred->group_info)
		put_group_info(cred->group_info);
	free_uid(cred->user);
	if (cred->ucounts)
		put_ucounts(cred->ucounts);
	put_user_ns(cred->user_ns);
	kmem_cache_free(cred_jar, cred);
#ifndef DS_USERNS_LIVE
	atomic_dec(&live_creds);
#endif
}

void ds_userns_put_cred(struct cred *cred)
{
	BUG_ON(atomic_long_read(&cred->usage) != 0);
	BUG_ON(cred == current->cred || cred == current->real_cred);
	if (cred->non_rcu)
		ds_userns_put_cred_rcu(&cred->rcu);
	else
		call_rcu(&cred->rcu, ds_userns_put_cred_rcu);
}

void ds_userns_abort_creds(struct cred *cred)
{
	BUG_ON(atomic_long_read(&cred->usage) < 1);
	put_cred(cred);
}

struct cred *ds_userns_prepare_creds_from(const struct cred *old)
{
	struct cred *new;

	new = kmem_cache_alloc(cred_jar, GFP_KERNEL);
	if (!new)
		return NULL;

	memcpy(new, old, sizeof(struct cred));

	new->non_rcu = 0;
	atomic_long_set(&new->usage, 1);
#ifndef DS_USERNS_LIVE
	atomic_inc(&live_creds);
#endif
	get_group_info(new->group_info);
	get_uid(new->user);
	get_user_ns(new->user_ns);

#ifdef CONFIG_KEYS
	key_get(new->session_keyring);
	key_get(new->process_keyring);
	key_get(new->thread_keyring);
	key_get(new->request_key_auth);
#endif

#ifdef CONFIG_SECURITY
	new->security = NULL;
#endif

	new->ucounts = get_ucounts(new->ucounts);
	if (!new->ucounts)
		goto error;

	if (security_prepare_creds(new, old, GFP_KERNEL_ACCOUNT) < 0)
		goto error;

	return new;

error:
	abort_creds(new);
	return NULL;
}

struct cred *ds_userns_prepare_creds(void)
{
	return ds_userns_prepare_creds_from(current_cred());
}

int ds_userns_creds_live(void)
{
	return atomic_read(&live_creds);
}
