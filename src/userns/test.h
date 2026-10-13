/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_USERNS_TEST_H
#define DS_USERNS_TEST_H
struct cred;
struct cred *ds_userns_prepare_creds_from(const struct cred *old);
int ds_userns_creds_live(void);
int ds_userns_uts_init(void);
void ds_userns_uts_exit(void);
int ds_userns_uts_live(void);
int ds_userns_core_init(void);
void ds_userns_core_drain(void);
int ds_userns_core_live(void);
void ds_userns_core_exit(void);
#endif
