// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_netfilter: " fmt
#include <linux/module.h>
#include <linux/of.h>
#include <linux/utsname.h>
#include <net/net_namespace.h>
#include <net/netns/nftables.h>

unsigned int ds_nft_state_id;
static struct pernet_operations ds_nft_state_ops = {
    .id = &ds_nft_state_id,
    .size = sizeof(struct netns_nftables),
};
#define DS_NF_UNIT(name) \
    extern int ds_nf_init_##name(void); \
    extern void ds_nf_exit_##name(void);
#include "units.h"
#undef DS_NF_UNIT
struct ds_nf_unit { const char *name; int (*init)(void); void (*exit)(void); };
static struct ds_nf_unit units[] = {
#define DS_NF_UNIT(name) { #name, ds_nf_init_##name, ds_nf_exit_##name },
#include "units.h"
#undef DS_NF_UNIT
};
static int __init ds_netfilter_init(void)
{
    unsigned int i;
    int err;
    bool vm = of_machine_is_compatible("linux,dummy-virt");
    bool device = of_machine_is_compatible("qcom,canoe");
    const char *release = utsname()->release;

    if ((!vm && !device) ||
        (strcmp(release, "6.12.69-android16-6-g242e1a07f878-ab15865446-4k") &&
         (!vm || strcmp(release, "6.12.76-4k-gae4e2f4f997e-dirty"))))
        return -EOPNOTSUPP;
    err = register_pernet_subsys(&ds_nft_state_ops);
    if (err)
        return err;
    for (i = 0; i < ARRAY_SIZE(units); i++) {
        err = units[i].init();
        if (err) {
            pr_err("%s registration failed: %d\n", units[i].name, err);
            while (i)
                units[--i].exit();
            unregister_pernet_subsys(&ds_nft_state_ops);
            return err;
        }
    }
    pr_info("xt_addrtype and nftables registered; private per-net state\n");
    return 0;
}
static void __exit ds_netfilter_exit(void)
{
    unsigned int i = ARRAY_SIZE(units);
    while (i)
        units[--i].exit();
    unregister_pernet_subsys(&ds_nft_state_ops);
}
module_init(ds_netfilter_init);
module_exit(ds_netfilter_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shikochin");
MODULE_DESCRIPTION("Droidspaces xt_addrtype and nftables for the validated 6.12 kernel");
