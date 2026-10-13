// SPDX-License-Identifier: GPL-2.0-only
/* A bounded, unprivileged boot-time check of real user namespace behavior. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int mapping(const char *name, const char *value, int expected_error)
{
    char path[80];
    snprintf(path, sizeof(path), "/proc/self/%s", name);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    int error = errno;
    close(fd);
    return expected_error ? !(n == -1 && error == expected_error) :
                            n != (ssize_t)strlen(value);
}

static int probe(int parent_ns, ino_t parent_inode)
{
    struct stat current;
    alarm(10);
    if (setgroups(0, NULL) || setgid(1000) || setuid(1000) ||
        prctl(PR_SET_DUMPABLE, 1) || unshare(CLONE_NEWUSER) || getuid() != 65534 ||
        fstatat(AT_FDCWD, "/proc/self/ns/user", &current, 0) ||
        current.st_ino == parent_inode)
        return 1;
    if (mapping("uid_map", "0 0 1\n", EPERM) ||
        mapping("uid_map", "0 1000 1\n", 0) ||
        mapping("setgroups", "deny\n", 0) ||
        mapping("gid_map", "0 1000 1\n", 0) ||
        getuid() || geteuid() || getgid() || getegid())
        return 2;
    if (setns(parent_ns, CLONE_NEWUSER) != -1 || errno != EPERM)
        return 3;
    if (unshare(CLONE_NEWUTS) || sethostname("ds-sandbox-check", 16))
        return 4;
    return 0;
}

int main(void)
{
    struct stat parent;
    int status;
    if (geteuid() != 0) return 1;
    int fd = open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &parent)) return 1;
    pid_t child = fork();
    if (child < 0) { close(fd); return 1; }
    if (!child) _exit(probe(fd, parent.st_ino));
    close(fd);
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "Sandboxing functional check failed (wait=%d, status=%d)\n",
                (int)result, result == child ? status : -1);
        return 1;
    }
    puts("DS_SANDBOX_READY");
    return 0;
}
