// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL overlay:%d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static const char *options = "lowerdir=/test-ext4/lower,upperdir=/test-ext4/upper,workdir=/test-ext4/work";
static int live(void)
{
    int n = -1;
    FILE *f = fopen("/sys/module/droidspaces_ns/parameters/userns_live", "r");
    CHECK(f && fscanf(f, "%d", &n) == 1);
    CHECK(fclose(f) == 0);
    return n;
}
static void write_map(const char *name, const char *value)
{
    char path[80];
    snprintf(path, sizeof(path), "/proc/self/%s", name);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0 && write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    CHECK(close(fd) == 0);
}
int main(void)
{
    int before = live();
    CHECK(mkdir("/test-ext4/lower", 0755) == 0);
    CHECK(mkdir("/test-ext4/upper", 0755) == 0);
    CHECK(mkdir("/test-ext4/work", 0755) == 0);
    CHECK(mkdir("/test-ext4/merged", 0755) == 0);
    int fd = open("/test-ext4/lower/file", O_CREAT|O_WRONLY, 0644);
    CHECK(fd >= 0 && write(fd, "lower", 5) == 5 && close(fd) == 0);
    CHECK(mount("overlay", "/test-ext4/merged", "overlay", 0, options) == 0);
    fd = open("/test-ext4/merged/file", O_WRONLY|O_TRUNC);
    CHECK(fd >= 0 && write(fd, "upper", 5) == 5 && close(fd) == 0);
    char data[6] = {0};
    fd = open("/test-ext4/lower/file", O_RDONLY);
    CHECK(fd >= 0 && read(fd, data, 5) == 5 && close(fd) == 0 && !strcmp(data, "lower"));
    CHECK(unlink("/test-ext4/merged/file") == 0);
    CHECK(umount("/test-ext4/merged") == 0);
    CHECK(mount("overlay", "/test-ext4/merged", "overlay", 0, options) == 0);
    CHECK(access("/test-ext4/merged/file", F_OK) == -1 && errno == ENOENT);
    CHECK(umount("/test-ext4/merged") == 0);
    for (int i = 0; i < 32; i++) {
        pid_t p = fork();
        CHECK(p >= 0);
        if (!p) {
            CHECK(unshare(CLONE_NEWUSER) == 0);
            write_map("uid_map", "0 0 1\n");
            write_map("setgroups", "deny\n");
            write_map("gid_map", "0 0 1\n");
            CHECK(unshare(CLONE_NEWNS) == 0);
            CHECK(mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) == 0);
            CHECK(syscall(SYS_fsopen, "overlay", 1) == -1 && errno == EOPNOTSUPP);
            CHECK(mount("overlay", "/test-ext4/merged", "overlay", 0, options) == -1 && errno == EOPNOTSUPP);
            /* A supported context still acquires/releases its owner normally. */
            fd = syscall(SYS_fsopen, "tmpfs", 1);
            CHECK(fd >= 0 && close(fd) == 0);
            _exit(0);
        }
        int status;
        CHECK(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    for (int i = 0; i < 100 && live() != before; i++) usleep(100000);
    CHECK(live() == before);
    puts("DS_OVERLAY_PASS: host copy-up/whiteout/remount; 32 child mount/fsopen rejections and context cleanup");
    return 0;
}
