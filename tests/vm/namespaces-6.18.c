// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    dprintf(2, "FAIL 6.18 line %d: %s (errno=%d)\n", __LINE__, #expr, errno); \
    _exit(1); } } while (0)

/* Published 64-byte PIDFD_INFO_SIZE_VER0 ABI; independent of libc headers. */
struct info_v0 {
    uint64_t mask, cgroupid;
    uint32_t pid, tgid, ppid;
    uint32_t ruid, rgid, euid, egid, suid, sgid, fsuid, fsgid;
    int32_t exit_code;
};
_Static_assert(sizeof(struct info_v0) == 64, "pidfd UAPI size");
static uint64_t host_pid_id, host_ipc_id;

static uint64_t namespace_id(const char *path)
{
    uint64_t id = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(ioctl(fd, _IOR(0xb7, 13, uint64_t), &id) == 0);
    CHECK(id != 0);
    CHECK(close(fd) == 0);
    return id;
}

static void native_pidfd(void)
{
    int fd = syscall(SYS_pidfd_open, getpid(), 0);
    uint32_t generation;
    struct info_v0 info = {};
    CHECK(fd >= 0);
    CHECK(ioctl(fd, FS_IOC_GETVERSION, &generation) == 0);
    CHECK(ioctl(fd, _IOWR(0xff, 11, struct info_v0), &info) == 0);
    CHECK((info.mask & 3) == 3);
    CHECK(info.pid == (uint32_t)getpid() && info.tgid == info.pid);
    CHECK(info.euid == geteuid() && info.egid == getegid());
    /* A native namespace command must still reach the original handler. */
    int mnt = ioctl(fd, _IO(0xff, 3), 0);
    CHECK(mnt >= 0);
    CHECK(close(mnt) == 0);
    CHECK(close(fd) == 0);
}

static int child(void *unused)
{
    (void)unused;
    CHECK(getpid() == 1);
    /* procfs here is the parent's mount: self resolves to the actual task. */
    CHECK(namespace_id("/proc/self/ns/pid") != host_pid_id);
    CHECK(namespace_id("/proc/self/ns/ipc") != host_ipc_id);
    native_pidfd();
    return 0;
}

int main(void)
{
    host_pid_id = namespace_id("/proc/self/ns/pid");
    host_ipc_id = namespace_id("/proc/self/ns/ipc");
    CHECK(host_pid_id != host_ipc_id);
    native_pidfd();
    void *stack = malloc(65536);
    CHECK(stack != NULL);
    pid_t pid = clone(child, (char *)stack + 65536, CLONE_NEWPID | CLONE_NEWIPC | SIGCHLD, NULL);
    CHECK(pid >= 0);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(stack);
    puts("DS_618_INTERFACES_PASS: namespace IDs, native pidfd info, generation and mount namespace");
    return 0;
}
