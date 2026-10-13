// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#define MFD_EXEC 0x0010U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif
#define CHECK(x) do { if (!(x)) { dprintf(2,"PID POLICY FAIL %d: %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
static const char *policy="/proc/sys/vm/memfd_noexec";
static int get_scope(void)
{
    FILE *f=fopen(policy,"r");
    int value;
    CHECK(f && fscanf(f,"%d",&value)==1 && fclose(f)==0);
    return value;
}
static void set_scope(int value, int error)
{
    int fd=open(policy,O_WRONLY);
    CHECK(fd>=0);
    char data='0'+value;
    ssize_t n=write(fd,&data,1);
    if (error) CHECK(n==-1 && errno==error);
    else CHECK(n==1);
    CHECK(close(fd)==0);
}
static void check_memfd(unsigned flags, int executable, int error)
{
    int fd=syscall(SYS_memfd_create,"ds-policy",flags);
    if (error) { CHECK(fd==-1 && errno==error); return; }
    struct stat st;
    CHECK(fd>=0 && fstat(fd,&st)==0);
    CHECK(!!(st.st_mode&0111)==executable);
    if (!executable) CHECK(fcntl(fd,F_GET_SEALS)&F_SEAL_EXEC);
    CHECK(close(fd)==0);
}
static int nested(void *unused)
{
    (void)unused;
    CHECK(get_scope()==2);
    set_scope(1,EINVAL);
    check_memfd(MFD_EXEC,0,EACCES);
    check_memfd(0,0,0);
    return 0;
}
static void run(int (*child)(void *))
{
    char *stack=malloc(65536);
    CHECK(stack);
    pid_t p=clone(child,stack+65536,CLONE_NEWPID|SIGCHLD,NULL);
    CHECK(p>0);
    int status;
    CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
    free(stack);
}
static int child(void *unused)
{
    (void)unused;
    CHECK(get_scope()==0);
    check_memfd(0,1,0);
    set_scope(1,0);
    check_memfd(0,0,0);
    check_memfd(MFD_EXEC,1,0);
    set_scope(2,0);
    check_memfd(MFD_EXEC,0,EACCES);
    check_memfd(0,0,0);
    check_memfd(MFD_EXEC|MFD_NOEXEC_SEAL,0,EINVAL);
    run(nested);
    return 0;
}
int main(void)
{
    CHECK(get_scope()==0);
    run(child);
    CHECK(get_scope()==0);
    check_memfd(0,1,0);
    puts("DS_PID_POLICY_PASS: memfd executable policy, seal, enforcement, parent floor, host isolation");
    return 0;
}
