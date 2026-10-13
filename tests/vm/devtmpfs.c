// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#define CHECK(expr) do { if (!(expr)) { \
    dprintf(2, "FAIL devtmpfs line %d: %s errno=%d\n", __LINE__, #expr, errno); exit(1); \
} } while (0)
static void online(int value)
{
    int fd = open("/sys/module/droidspaces_devtmpfs_fixture/parameters/online", O_WRONLY);
    CHECK(fd >= 0);
    CHECK(write(fd, value ? "1" : "0", 1) == 1);
    CHECK(close(fd) == 0);
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        CHECK(chdir(argv[1]) == 0);
        CHECK(unshare(CLONE_NEWNS) == 0);
        CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    }
    struct stat a, b;
    char buffer[32];
    CHECK(mkdir("testdev", 0700) == 0);
    CHECK(mkdir("testdev2", 0700) == 0);
    CHECK(mount("devtmpfs", "testdev", "devtmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755") == 0);
    CHECK(mount("devtmpfs", "testdev2", "devtmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755") == 0);
    CHECK(stat("testdev/null", &a) == 0 && S_ISCHR(a.st_mode));
    CHECK(stat("testdev2/null", &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev);
    int fd = open("testdev/zero", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buffer, sizeof(buffer)) == sizeof(buffer));
    for (unsigned i=0; i<sizeof(buffer); i++) CHECK(buffer[i] == 0);
    CHECK(close(fd) == 0);
    for (int i=0; i<8; i++) {
        online(1);
        CHECK(stat("testdev/droidspaces/testnode", &a) == 0 && S_ISCHR(a.st_mode));
        CHECK((a.st_mode & 0777) == 0660 && a.st_uid == 0 && a.st_gid == 0);
        fd = open("testdev2/droidspaces/testnode", O_RDONLY);
        CHECK(fd >= 0);
        online(0);
        CHECK(stat("testdev/droidspaces/testnode", &b) == -1 && errno == ENOENT);
        CHECK(stat("testdev/droidspaces", &b) == -1 && errno == ENOENT);
        CHECK(fstat(fd, &b) == 0 && (b.st_mode & 0777) == 0);
        CHECK(read(fd, buffer, sizeof(buffer)) == 12 && !memcmp(buffer, "devtmpfs-ok\n", 12));
        CHECK(close(fd) == 0);
    }
    CHECK(umount("testdev") == 0);
    CHECK(umount("testdev2") == 0);
    puts("DS_DEVTMPFS_PASS: existing nodes, singleton mounts, device I/O, hotplug, modes, removal and open-FD lifetime");
    return 0;
}
