// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(expr) do { if (!(expr)) { dprintf(2, "FAIL userns-core line %d: %s errno=%d\n", __LINE__, #expr, errno); exit(1); } } while (0)
static int open_map(const char *group, const char *name, int flags)
{
    char path[160];
    snprintf(path, sizeof(path), "/proc/ds_userns_test/%s/%s", group, name);
    return open(path, flags);
}
static void put_map(const char *group, const char *name, const char *value, int error)
{
    int fd = open_map(group, name, O_WRONLY);
    CHECK(fd >= 0);
    ssize_t ret = write(fd, value, strlen(value));
    if (error) CHECK(ret == -1 && errno == error);
    else CHECK(ret == (ssize_t)strlen(value));
    CHECK(close(fd) == 0);
}
static void contains(const char *group, const char *name, const char *text)
{
    char buffer[2048];
    int fd = open_map(group, name, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t len = read(fd, buffer, sizeof(buffer) - 1);
    CHECK(len >= 0);
    buffer[len] = 0;
    CHECK(strstr(buffer, text) != NULL);
    CHECK(close(fd) == 0);
}
static void unprivileged_tests(int after_deny)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(setgid(1000) == 0 && setuid(1000) == 0);
        put_map("root", "uid_map", "0 1000 1\n", EPERM);
        int fd = open_map("root", "setgroups", O_WRONLY);
        CHECK(fd == -1 && errno == EACCES); // Neither the owner nor privileged in the parent.
        if (!after_deny) {
            put_map("owner", "uid_map", "0 0 1\n", EPERM);
            put_map("owner", "uid_map", "0 1001 1\n", EPERM);
            put_map("owner", "gid_map", "0 1000 1\n", EPERM);
        } else {
            put_map("owner", "uid_map", "0 1000 1\n", 0);
            put_map("owner", "gid_map", "0 1000 1\n", 0);
            contains("owner", "check", "checked=1");
        }
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(void)
{
    CHECK(access("/proc/self/ns/user", F_OK) == -1 && errno == ENOENT);
    CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EINVAL);
    put_map("root", "uid_map", "0 100000 0\n", EINVAL);
    put_map("root", "uid_map", "4294967295 100000 1\n", EINVAL);
    put_map("root", "uid_map", "4294967294 100000 2\n", EINVAL);
    put_map("root", "uid_map", "0 100000 10\n5 200000 10\n", EINVAL);
    put_map("root", "uid_map", "0 100000 10\n20 100005 10\n", EINVAL);
    put_map("root", "uid_map", "0 100000 10 junk\n", EINVAL);
    int fd = open_map("root", "uid_map", O_WRONLY);
    CHECK(fd >= 0);
    CHECK(syscall(SYS_write, fd, (void *)1, 8) == -1 && errno == EFAULT);
    CHECK(lseek(fd, 1, SEEK_SET) == 1);
    CHECK(write(fd, "0 100000 1\n", 11) == -1 && errno == EINVAL);
    CHECK(close(fd) == 0);
    unprivileged_tests(0);
    put_map("owner", "setgroups", "deny\n", 0);
    put_map("owner", "setgroups", "allow\n", EPERM);
    contains("owner", "setgroups", "deny");
    unprivileged_tests(1);
    put_map("root", "uid_map", "0 100000 10\n", 0);
    contains("root", "check", "checked=1");
    put_map("root", "uid_map", "20 200000 1\n", EPERM);
    put_map("root", "gid_map", "0 200000 10\n", 0);
    put_map("root", "setgroups", "deny\n", EPERM); // Cannot revoke after installing gid_map.
    put_map("root", "projid_map", "0 300000 10\n", 0);
    put_map("large", "uid_map", "50 100500 3\n0 100000 3\n40 100400 3\n10 100100 3\n30 100300 3\n20 100200 3\n", 0);
    contains("large", "check", "checked=6");
    contains("large", "uid_map", "100500");
    CHECK(access("/proc/self/ns/user", F_OK) == -1 && errno == ENOENT);
    CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EINVAL);
    puts("DS_USERNS_CORE_PASS: mapping validation, ownership, privilege rejection, setgroups, large maps; task attachment remains disabled");
    return 0;
}
