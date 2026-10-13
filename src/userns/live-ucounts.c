// SPDX-License-Identifier: GPL-2.0-only
/* From kernel/ucount.c: retain the native table while restoring namespace references. */
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/hash.h>
#include <linux/user_namespace.h>
#define UCOUNTS_HASHTABLE_BITS 10
extern struct hlist_head ucounts_hashtable[(1 << UCOUNTS_HASHTABLE_BITS)];
extern spinlock_t ucounts_lock;

#define ucounts_hashfn(ns, uid)						\
	hash_long((unsigned long)__kuid_val(uid) + (unsigned long)(ns), \
		  UCOUNTS_HASHTABLE_BITS)
#define ucounts_hashentry(ns, uid)	\
	(ucounts_hashtable + ucounts_hashfn(ns, uid))

static struct ucounts *find_ucounts(struct user_namespace *ns, kuid_t uid, struct hlist_head *hashent)
{
	struct ucounts *ucounts;

	hlist_for_each_entry(ucounts, hashent, node) {
		if (uid_eq(ucounts->uid, uid) && (ucounts->ns == ns))
			return ucounts;
	}
	return NULL;
}

static inline bool get_ucounts_or_wrap(struct ucounts *ucounts)
{
	/* Returns true on a successful get, false if the count wraps. */
	return !atomic_add_negative(1, &ucounts->count);
}

struct ucounts *alloc_ucounts(struct user_namespace *ns, kuid_t uid)
{
	struct hlist_head *hashent = ucounts_hashentry(ns, uid);
	struct ucounts *ucounts, *new;
	bool wrapped;

	spin_lock_irq(&ucounts_lock);
	ucounts = find_ucounts(ns, uid, hashent);
	if (!ucounts) {
		spin_unlock_irq(&ucounts_lock);

		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return NULL;

		new->ns = ns;
		new->uid = uid;
		atomic_set(&new->count, 1);

		spin_lock_irq(&ucounts_lock);
		ucounts = find_ucounts(ns, uid, hashent);
		if (ucounts) {
			kfree(new);
		} else {
			hlist_add_head(&new->node, hashent);
			get_user_ns(new->ns);
			spin_unlock_irq(&ucounts_lock);
			return new;
		}
	}
	wrapped = !get_ucounts_or_wrap(ucounts);
	spin_unlock_irq(&ucounts_lock);
	if (wrapped) {
		put_ucounts(ucounts);
		return NULL;
	}
	return ucounts;
}

void put_ucounts(struct ucounts *ucounts)
{
	unsigned long flags;

	if (atomic_dec_and_lock_irqsave(&ucounts->count, &ucounts_lock, flags)) {
		hlist_del_init(&ucounts->node);
		spin_unlock_irqrestore(&ucounts_lock, flags);
		put_user_ns(ucounts->ns);
		kfree(ucounts);
	}
}

struct ucounts *get_ucounts(struct ucounts *ucounts)
{
	if (!get_ucounts_or_wrap(ucounts)) {
		put_ucounts(ucounts);
		ucounts = NULL;
	}
	return ucounts;
}

static inline bool atomic_long_inc_below(atomic_long_t *v, long u)
{
	long c, old;
	c = atomic_long_read(v);
	for (;;) {
		if (unlikely(c >= u))
			return false;
		old = atomic_long_cmpxchg(v, c, c+1);
		if (likely(old == c))
			return true;
		c = old;
	}
}

struct ucounts *inc_ucount(struct user_namespace *ns, kuid_t uid,
			   enum ucount_type type)
{
	struct ucounts *ucounts, *iter, *bad;
	struct user_namespace *tns;
	ucounts = alloc_ucounts(ns, uid);
	for (iter = ucounts; iter; iter = tns->ucounts) {
		long max;
		tns = iter->ns;
		max = READ_ONCE(tns->ucount_max[type]);
		if (!atomic_long_inc_below(&iter->ucount[type], max))
			goto fail;
	}
	return ucounts;
fail:
	bad = iter;
	for (iter = ucounts; iter != bad; iter = iter->ns->ucounts)
		atomic_long_dec(&iter->ucount[type]);

	put_ucounts(ucounts);
	return NULL;
}

void dec_ucount(struct ucounts *ucounts, enum ucount_type type)
{
	struct ucounts *iter;
	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long dec = atomic_long_dec_if_positive(&iter->ucount[type]);
		WARN_ON_ONCE(dec < 0);
	}
	put_ucounts(ucounts);
}

long inc_rlimit_ucounts(struct ucounts *ucounts, enum rlimit_type type, long v)
{
	struct ucounts *iter;
	long max = LONG_MAX;
	long ret = 0;

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long new = atomic_long_add_return(v, &iter->rlimit[type]);
		if (new < 0 || new > max)
			ret = LONG_MAX;
		else if (iter == ucounts)
			ret = new;
		max = get_userns_rlimit_max(iter->ns, type);
	}
	return ret;
}

bool dec_rlimit_ucounts(struct ucounts *ucounts, enum rlimit_type type, long v)
{
	struct ucounts *iter;
	long new = -1; /* Silence compiler warning */
	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long dec = atomic_long_sub_return(v, &iter->rlimit[type]);
		WARN_ON_ONCE(dec < 0);
		if (iter == ucounts)
			new = dec;
	}
	return (new == 0);
}

static void do_dec_rlimit_put_ucounts(struct ucounts *ucounts,
				struct ucounts *last, enum rlimit_type type)
{
	struct ucounts *iter, *next;
	for (iter = ucounts; iter != last; iter = next) {
		long dec = atomic_long_sub_return(1, &iter->rlimit[type]);
		WARN_ON_ONCE(dec < 0);
		next = iter->ns->ucounts;
		if (dec == 0)
			put_ucounts(iter);
	}
}

void dec_rlimit_put_ucounts(struct ucounts *ucounts, enum rlimit_type type)
{
	do_dec_rlimit_put_ucounts(ucounts, NULL, type);
}

long inc_rlimit_get_ucounts(struct ucounts *ucounts, enum rlimit_type type,
			    bool override_rlimit)
{
	/* Caller must hold a reference to ucounts */
	struct ucounts *iter;
	long max = LONG_MAX;
	long dec, ret = 0;

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long new = atomic_long_add_return(1, &iter->rlimit[type]);
		if (new < 0 || new > max)
			goto dec_unwind;
		if (iter == ucounts)
			ret = new;
		if (!override_rlimit)
			max = get_userns_rlimit_max(iter->ns, type);
		/*
		 * Grab an extra ucount reference for the caller when
		 * the rlimit count was previously 0.
		 */
		if (new != 1)
			continue;
		if (!get_ucounts(iter))
			goto dec_unwind;
	}
	return ret;
dec_unwind:
	dec = atomic_long_sub_return(1, &iter->rlimit[type]);
	WARN_ON_ONCE(dec < 0);
	do_dec_rlimit_put_ucounts(ucounts, iter, type);
	return 0;
}

bool is_rlimit_overlimit(struct ucounts *ucounts, enum rlimit_type type, unsigned long rlimit)
{
	struct ucounts *iter;
	long max = rlimit;
	if (rlimit > LONG_MAX)
		max = LONG_MAX;
	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long val = get_rlimit_value(iter, type);
		if (val < 0 || val > max)
			return true;
		max = get_userns_rlimit_max(iter->ns, type);
	}
	return false;
}


/* Native callbacks were constant-folded to init_user_ns with USER_NS=n. */
struct ctl_table_set *ds_userns_set_lookup(struct ctl_table_root *root)
{
	return &current_user_ns()->set;
}
int ds_userns_set_is_seen(struct ctl_table_set *set)
{
	return &current_user_ns()->set == set;
}
