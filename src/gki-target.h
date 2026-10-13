/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_GKI_TARGET_H
#define DS_GKI_TARGET_H

#ifdef DS_GKI_ANDROID
#include <generated/utsrelease.h>
#include <linux/ctype.h>

static bool gki;
module_param(gki, bool, 0400);
MODULE_PARM_DESC(gki, "Enable the compiled GKI family after userspace ABI validation");

static bool ds_gki_target_supported(void)
{
    const char *release = utsname()->release;
    const char *patch;

    if (of_machine_is_compatible("linux,dummy-virt") && !strcmp(release, UTS_RELEASE))
        return true;
    if (!gki || strncmp(release, DS_GKI_VERSION ".", sizeof(DS_GKI_VERSION)))
        return false;
    patch = release + sizeof(DS_GKI_VERSION);
    if (!isdigit(*patch))
        return false;
    while (isdigit(*patch))
        patch++;
    return !strncmp(patch, "-" DS_GKI_ANDROID "-", sizeof(DS_GKI_ANDROID) + 1);
}
#endif
#endif
