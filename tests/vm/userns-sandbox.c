// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    int disabled=argc>1 && !strcmp(argv[1],"--disable-userns");
    struct stat net,cgroup;
    char identity[32];
    if (stat("/proc/self/ns/net",&net) || stat("/proc/self/ns/cgroup",&cgroup)) return 1;
    snprintf(identity,sizeof(identity),"%lu",(unsigned long)net.st_ino);
    setenv("DS_HOST_NET_INODE",identity,1);
    snprintf(identity,sizeof(identity),"%lu",(unsigned long)cgroup.st_ino);
    setenv("DS_HOST_CGROUP_INODE",identity,1);
    setenv("DS_SANDBOX_PROFILE",disabled ? "disabled" : (argc>1 ? "all" : "basic"),1);
    if (setgroups(0,NULL) || setgid(1000) || setuid(1000)) {
        perror("sandbox drop host privileges");
        return 1;
    }
    if (argc>1 && !strcmp(argv[1],"--fuse")) {
        execl("/usr/bin/bwrap","bwrap","--unshare-all","--uid","0","--gid","0",
            "--cap-add","CAP_SYS_ADMIN","--ro-bind","/bin","/bin",
            "--ro-bind","/userns-fuse-io-test","/fuse-io-test",
            "--proc","/proc","--dev","/dev","--dev-bind","/dev/fuse","/dev/fuse",
            "--tmpfs","/tmp","--chdir","/","--","/fuse-io-test","--inside",NULL);
        perror("exec bubblewrap FUSE"); return 1;
    }
    execl("/usr/bin/bwrap","bwrap",
        argc>1 ? "--unshare-all" : "--unshare-user", "--unshare-user",
        disabled ? "--disable-userns" : "--unshare-user",
        disabled ? "--assert-userns-disabled" : "--unshare-user","--uid","0","--gid","0",
        "--unshare-pid","--unshare-ipc","--unshare-uts",
        "--ro-bind","/bin","/bin","--proc","/proc","--dev","/dev",
        "--tmpfs","/tmp","--chdir","/",
        "--","/bin/sh","-ec",
        "test \"$(id -u)\" = 0; test \"$(id -g)\" = 0; "
        "test -r /proc/1/status; test ! -e /host-should-be-hidden; "
        "test ! -e /root-only; echo private > /tmp/private; "
        "test \"$(cat /tmp/private)\" = private; "
        "test -c /dev/null; "
        "if [ \"$DS_SANDBOX_PROFILE\" != basic ]; then "
        "test \"$(stat -L -c %i /proc/self/ns/net)\" != \"$DS_HOST_NET_INODE\"; "
        "test \"$(stat -L -c %i /proc/self/ns/cgroup)\" != \"$DS_HOST_CGROUP_INODE\"; "
        "ifconfig lo | grep -q UP; "
        "if [ \"$DS_SANDBOX_PROFILE\" = disabled ]; then "
        "if unshare -U true; then exit 1; fi; "
        "echo DS_USERNS_SANDBOX_DISABLED_PASS: nested user namespaces rejected; "
        "else echo DS_USERNS_SANDBOX_ALL_PASS: network and cgroup; fi; "
        "else echo DS_USERNS_SANDBOX_PASS: Bubblewrap workload; fi",
        NULL);
    perror("exec bubblewrap");
    return 1;
}
