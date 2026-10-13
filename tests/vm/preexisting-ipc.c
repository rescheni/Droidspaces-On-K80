// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"PREEXISTING IPC FAIL %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while (0)
int main(void)
{
    CHECK(unshare(CLONE_NEWIPC) == 0);
    int ns = open("/proc/self/ns/ipc", O_RDONLY);
    CHECK(ns >= 0);
    struct mq_attr attr = {.mq_maxmsg=4, .mq_msgsize=16};
    mqd_t mq = mq_open("/ds-before-module", O_CREAT | O_RDWR, 0600, &attr);
    CHECK(mq != (mqd_t)-1);
    CHECK(mq_send(mq, "before", 7, 1) == 0);
    int ready = creat("/old-ipc-ready", 0600);
    CHECK(ready >= 0 && close(ready) == 0);
    while (access("/old-ipc-go", F_OK)) usleep(10000);
    /* This task and both native handles predate module activation. */
    char data[16];
    CHECK(mq_receive(mq, data, sizeof(data), NULL) == 7);
    int msg = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    CHECK(msg >= 0);
    CHECK(access("/proc/sys/kernel/msgmax", F_OK) == 0);
    int sem = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    CHECK(sem >= 0);
    int shm = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    CHECK(shm >= 0);
    CHECK(setns(ns, CLONE_NEWIPC) == 0);
    CHECK(msgctl(msg, IPC_RMID, NULL) == 0);
    CHECK(semctl(sem, 0, IPC_RMID) == 0);
    CHECK(shmctl(shm, IPC_RMID, NULL) == 0);
    CHECK(close(ns) == 0 && mq_close(mq) == 0);
    CHECK(mq_unlink("/ds-before-module") == 0);
    puts("DS_PREEXISTING_IPC_PASS: native namespace, namespace FD and mqueue survive activation; SysV state initialized lazily");
    return 0;
}
