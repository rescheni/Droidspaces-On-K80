// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <grp.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(expr) do { if (!(expr)) { dprintf(2,"FAIL userns-live %d: %s errno=%d\n",__LINE__,#expr,errno);exit(1); } } while (0)
static void mapping(const char *name, const char *value, int error)
{
    char path[100];
    snprintf(path,sizeof(path),"/proc/self/%s",name);
    int fd=open(path,O_WRONLY);
    CHECK(fd>=0);
    ssize_t ret=write(fd,value,strlen(value));
    if (error) CHECK(ret==-1 && errno==error);
    else CHECK(ret==(ssize_t)strlen(value));
    CHECK(close(fd)==0);
}
/* Version 1 arm64 mount-query ABI (headers in the test VM predate it). */
struct ds_mnt_req { uint32_t size, fd; uint64_t id, param, ns_id; };
static void mount_queries(void)
{
    struct ds_mnt_req req = { .size=32, .id=UINT64_MAX };
    uint64_t ids[32], result[64];
    long count=syscall(458, &req, ids, 32, 0); /* listmount */
    CHECK(count>0);
    req.id=ids[0];
    req.param=2; /* STATMOUNT_MNT_BASIC */
    CHECK(syscall(457, &req, result, sizeof(result), 0)==0); /* statmount */
    CHECK((result[1]&2)!=0);
    CHECK(syscall(457, &req, NULL, sizeof(result), 0)==-1 && errno==EFAULT);
}
static int clone_user(void *unused)
{
    (void)unused;
    CHECK(getuid()==65534);
    mapping("uid_map","0 1000 1\n",0);
    CHECK(getuid()==0);
    return 0;
}
int main(void)
{
    struct stat parent, child;
    CHECK(stat("/proc/self/ns/user",&parent)==0);
    CHECK(mkdir("/mount-source",0777)==0 && mkdir("/mount-target",0777)==0);
    CHECK(chown("/mount-source",1000,1000)==0);
    mount_queries();
    int fd=open("/root-only",O_WRONLY|O_CREAT|O_TRUNC,0600);
    CHECK(fd>=0 && write(fd,"secret",6)==6 && close(fd)==0);
    pid_t pid=fork();
    CHECK(pid>=0);
    if (!pid) {
        gid_t initial_groups[2]={1000,1001};
        CHECK(setgroups(2,initial_groups)==0);
        CHECK(setgid(1000)==0 && setuid(1000)==0);
        CHECK(prctl(PR_SET_DUMPABLE,1)==0);
        char *stack=malloc(1<<20);
        CHECK(stack!=NULL);
        pid_t cloned=clone(clone_user,stack+(1<<20),CLONE_NEWUSER|SIGCHLD,NULL);
        CHECK(cloned>0);
        int clone_status;
        CHECK(waitpid(cloned,&clone_status,0)==cloned && WIFEXITED(clone_status) && WEXITSTATUS(clone_status)==0);
        free(stack);
        CHECK(unshare(CLONE_NEWUSER)==0);
        CHECK(stat("/proc/self/ns/user",&child)==0 && child.st_ino!=parent.st_ino);
        CHECK(getuid()==65534 && getgid()==65534);
        mapping("uid_map","0 0 1\n",EPERM);
        mapping("uid_map","0 1000 1\n",0);
        mapping("gid_map","0 1000 1\n",EPERM);
        mapping("setgroups","deny\n",0);
        mapping("gid_map","0 1000 1\n",0);
        CHECK(getuid()==0 && geteuid()==0 && getgid()==0 && getegid()==0);
        uid_t ru,eu,su;
        gid_t rg,eg,sg,groups[2];
        CHECK(getresuid(&ru,&eu,&su)==0 && ru==0 && eu==0 && su==0);
        CHECK(getresgid(&rg,&eg,&sg)==0 && rg==0 && eg==0 && sg==0);
        CHECK(getgroups(2,groups)==2 && groups[0]==0 && groups[1]==65534);
        CHECK(setgroups(0,NULL)==-1 && errno==EPERM);
        CHECK(setresuid(0,0,0)==0 && setresgid(0,0,0)==0);
        CHECK(setreuid(0,0)==0 && setregid(0,0)==0);
        CHECK(setuid(0)==0 && setgid(0)==0);
        CHECK(setfsuid(0)==0 && setfsgid(0)==0);
        CHECK(setuid(1)==-1 && errno==EINVAL);
        CHECK(setgid(1)==-1 && errno==EINVAL);
        CHECK(stat("/root-only",&child)==0 && child.st_uid==65534);
        CHECK(stat("/mount-source",&child)==0 && child.st_uid==0 && child.st_gid==0);
        struct statx sx;
        CHECK(statx(AT_FDCWD,"/mount-source",0,STATX_UID|STATX_GID,&sx)==0 && sx.stx_uid==0 && sx.stx_gid==0);
        fd=open("/root-only",O_RDONLY);
        CHECK(fd==-1 && errno==EACCES);
        CHECK(unshare(CLONE_NEWNS)==0);
        CHECK(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)==0);
        CHECK(mount("/mount-source","/mount-target",NULL,MS_BIND,NULL)==0);
        mount_queries();
        CHECK(umount("/mount-target")==0);
        CHECK(mount("sandbox","/mount-target","tmpfs",MS_NOSUID|MS_NODEV,"mode=0700")==0);
        fd=open("/mount-target/private",O_WRONLY|O_CREAT|O_EXCL,0600);
        CHECK(fd>=0 && write(fd,"ok",2)==2 && close(fd)==0);
        CHECK(umount("/mount-target")==0);
        pid_t proc_test=fork();
        CHECK(proc_test>=0);
        if (!proc_test) {
            CHECK(unshare(CLONE_NEWPID)==0);
            pid_t proc_child=fork();
            CHECK(proc_child>=0);
            if (!proc_child) {
                CHECK(getpid()==1);
                CHECK(mount("proc","/mount-target","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)==0);
                CHECK(access("/mount-target/1/status",R_OK)==0);
                CHECK(umount("/mount-target")==0);
                _exit(0);
            }
            int proc_status;
            CHECK(waitpid(proc_child,&proc_status,0)==proc_child && WIFEXITED(proc_status) && WEXITSTATUS(proc_status)==0);
            _exit(0);
        }
        int proc_test_status;
        CHECK(waitpid(proc_test,&proc_test_status,0)==proc_test && WIFEXITED(proc_test_status) && WEXITSTATUS(proc_test_status)==0);
        CHECK(unshare(CLONE_NEWUTS)==0);
        CHECK(sethostname("sandbox-test",12)==0);
        char name[64];
        CHECK(gethostname(name,sizeof(name))==0 && !strcmp(name,"sandbox-test"));
        pid_t nested=fork();
        CHECK(nested>=0);
        if (!nested) {
            CHECK(getuid()==0);
            CHECK(unshare(CLONE_NEWUSER)==0);
            mapping("uid_map","0 0 1\n",0);
            CHECK(getuid()==0);
            _exit(0);
        }
        int status;
        CHECK(waitpid(nested,&status,0)==nested && WIFEXITED(status) && WEXITSTATUS(status)==0);
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    char name[64];
    CHECK(gethostname(name,sizeof(name))==0 && strcmp(name,"sandbox-test"));
    CHECK(getuid()==0 && stat("/proc/self/ns/user",&child)==0 && child.st_ino==parent.st_ino);
    puts("DS_USERNS_LIVE_PASS: actual clone/unshare, mount ownership/queries, tmpfs and PID-owned procfs, proc maps, UID/GID changes and stat mappings, host file rejection, UTS ownership, nested fork/unshare and exit");
    return 0;
}
