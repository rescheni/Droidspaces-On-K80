// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
static int write_map(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    int result = n == (ssize_t)strlen(value) ? 0 : -1;
    close(fd);
    return result;
}

int main(int argc, char **argv) {
    int sandbox = argc == 2 && !strcmp(argv[1], "--sandbox");
    if (sandbox) {
        CHECK(!unshare(CLONE_NEWUSER));
        CHECK(!write_map("/proc/self/uid_map", "0 0 1\n"));
        CHECK(!write_map("/proc/self/setgroups", "deny\n"));
        CHECK(!write_map("/proc/self/gid_map", "0 0 1\n"));
    }
    char options[128];
    setbuf(stdout, NULL);
    CHECK(!unshare(CLONE_NEWNS));
    CHECK(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL));
    CHECK(!mkdir("/fuse-test",0700));
    int fd=open("/dev/fuse",O_RDWR);
    CHECK(fd>=0);
    snprintf(options,sizeof(options),"fd=%d,rootmode=40000,user_id=0,group_id=0",fd);
    puts("DS_FUSE_MOUNT_BEGIN");
    int result = mount("ds-test","/fuse-test","fuse",MS_NOSUID|MS_NODEV,options);
    CHECK(!result);
    puts("DS_FUSE_MOUNT_OK");
    CHECK(!close(fd));
    CHECK(!umount2("/fuse-test",MNT_DETACH));
    CHECK(!rmdir("/fuse-test"));
    puts(sandbox ? "DS_FUSE_SANDBOX_MOUNT_PASS" : "DS_FUSE_PASS");
    return 0;
}
