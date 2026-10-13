// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"PID LIFETIME FAIL %d: %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
#ifndef DS_TEST_BPF_ITER
#define DS_TEST_BPF_ITER 1
#endif

static int channel;
static int bpf_call(int cmd, union bpf_attr *attr)
{
    return syscall(SYS_bpf, cmd, attr, sizeof(*attr));
}
static unsigned task_btf_id(void)
{
    FILE *f = fopen("/sys/kernel/btf/vmlinux", "rb");
    CHECK(f != NULL);
    size_t capacity = 16 * 1024 * 1024;
    char *data = malloc(capacity);
    CHECK(data != NULL);
    size_t bytes = fread(data, 1, capacity, f);
    CHECK(feof(f) && bytes > sizeof(struct btf_header));
    fclose(f);
    struct btf_header *h = (void *)data;
    CHECK(h->magic == BTF_MAGIC && h->hdr_len+h->str_off+h->str_len <= bytes);
    char *strings = data+h->hdr_len+h->str_off;
    unsigned id = 1, found = 0;
    size_t offset = h->hdr_len+h->type_off, end = offset+h->type_len;
    while (offset < end) {
        struct btf_type *t = (void *)(data+offset);
        unsigned kind = BTF_INFO_KIND(t->info), count = BTF_INFO_VLEN(t->info);
        CHECK(t->name_off < h->str_len);
        if (kind == BTF_KIND_FUNC && !strcmp(strings+t->name_off,"bpf_iter_task")) { found=id; break; }
        unsigned words = 0;
        switch (kind) {
        case 1: case 14: case 17: words=1; break;
        case 3: words=3; break;
        case 4: case 5: case 15: case 19: words=count*3; break;
        case 6: case 13: words=count*2; break;
        default: CHECK(kind <= 19); break;
        }
        offset += sizeof(*t)+words*4;
        id++;
    }
    free(data);
    CHECK(found != 0);
    return found;
}
static int make_iterator(void)
{
    struct bpf_insn program[] = {
        { .code=BPF_ALU64|BPF_MOV|BPF_K, .dst_reg=BPF_REG_0, .imm=0 },
        { .code=BPF_JMP|BPF_EXIT },
    };
    char log[16384] = {0};
    union bpf_attr attr = { .prog_type=BPF_PROG_TYPE_TRACING, .insn_cnt=2,
        .insns=(uintptr_t)program, .license=(uintptr_t)"GPL",
        .expected_attach_type=BPF_TRACE_ITER, .attach_btf_id=task_btf_id(),
        .log_buf=(uintptr_t)log, .log_size=sizeof(log), .log_level=1 };
    int prog = bpf_call(BPF_PROG_LOAD, &attr);
    if (prog < 0) dprintf(2,"BPF verifier: %s\n",log);
    CHECK(prog >= 0);
    memset(&attr,0,sizeof(attr));
    attr.link_create.prog_fd=prog;
    attr.link_create.attach_type=BPF_TRACE_ITER;
    int link=bpf_call(BPF_LINK_CREATE,&attr);
    CHECK(link >= 0);
    memset(&attr,0,sizeof(attr));
    attr.iter_create.link_fd=link;
    int iter=bpf_call(BPF_ITER_CREATE,&attr);
    CHECK(iter >= 0 && close(link)==0 && close(prog)==0);
    return iter;
}
static void send_fd(int fd)
{
    char data='f', control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov={&data,1};
    struct msghdr msg={ .msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control) };
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS; c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&fd,sizeof(fd));
    CHECK(sendmsg(channel,&msg,0)==1);
}
static int receive_fd(int socket)
{
    char data,control[CMSG_SPACE(sizeof(int))];
    struct iovec iov={&data,1};
    struct msghdr msg={ .msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control) };
    CHECK(recvmsg(socket,&msg,0)==1 && !(msg.msg_flags & MSG_CTRUNC));
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    CHECK(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len==CMSG_LEN(sizeof(int)));
    int fd;
    memcpy(&fd,CMSG_DATA(c),sizeof(fd));
    return fd;
}
static int lifetime_child(void *which)
{
    CHECK(getpid()==1);
    int fd;
    if ((uintptr_t)which == 1) {
        struct perf_event_attr attr={ .type=PERF_TYPE_SOFTWARE,.size=sizeof(attr),
            .config=PERF_COUNT_SW_TASK_CLOCK,.disabled=1,.exclude_kernel=1,.exclude_hv=1,.inherit=1 };
        fd=syscall(SYS_perf_event_open,&attr,0,-1,-1,0);
        CHECK(fd>=0);
        pid_t p=fork(); CHECK(p>=0);
        if (!p) _exit(0);
        int status;
        CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
    } else {
        fd=make_iterator();
    }
    send_fd(fd);
    CHECK(close(fd)==0);
    return 0;
}
static int mount_child(void *unused)
{
    (void)unused;
    CHECK(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)==0);
    const char *root=getenv("DS_TEST_ROOT");
    CHECK(chdir(root ? root : "/")==0);
    CHECK(mkdir("cg-test",0700)==0 && mkdir("fuse-test",0700)==0);
    CHECK(mount("cgroup","cg-test","cgroup",0,"none,name=ds-test")==0);
    FILE *f=fopen("cg-test/cgroup.procs","r");
    CHECK(f!=NULL);
    int pid,found=0;
    while (fscanf(f,"%d",&pid)==1) if (pid==1) found=1;
    CHECK(fclose(f)==0 && found);
    CHECK(umount("cg-test")==0);
    int fd=open("/dev/fuse",O_RDWR);
    CHECK(fd>=0);
    char options[128];
    snprintf(options,sizeof(options),"fd=%d,rootmode=40000,user_id=0,group_id=0",fd);
    CHECK(mount("ds-test","fuse-test","fuse",MS_NOSUID|MS_NODEV,options)==0);
    CHECK(close(fd)==0);
    CHECK(umount2("fuse-test",MNT_DETACH)==0);
    CHECK(rmdir("cg-test")==0 && rmdir("fuse-test")==0);
    return 0;
}
int main(void)
{
    char *stack=malloc(65536);
    CHECK(stack!=NULL);
    for (uintptr_t which=1; which<=(DS_TEST_BPF_ITER ? 2 : 1); which++) {
        int pair[2];
        CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0);
        channel=pair[1];
        pid_t p=clone(lifetime_child,stack+65536,CLONE_NEWPID|SIGCHLD,(void *)which);
        CHECK(p>=0 && close(pair[1])==0);
        int fd=receive_fd(pair[0]);
        int status;
        CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
        /* The namespace's init is gone; the inherited FD must still be usable. */
        uint64_t data;
        CHECK(read(fd,&data,sizeof(data))==(which==1 ? (ssize_t)sizeof(data) : 0));
        CHECK(close(fd)==0 && close(pair[0])==0);
    }
    pid_t p=clone(mount_child,stack+65536,CLONE_NEWPID|CLONE_NEWNS|SIGCHLD,NULL);
    CHECK(p>=0);
    int status;
    CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
    free(stack);
    if (DS_TEST_BPF_ITER)
        puts("DS_BPF_ITER_PASS: retained iterator FD");
    else
        puts("DS_BPF_ITER_SKIP: baseline has no kernel BTF for tracing programs");
    puts("DS_PID_LIFETIMES_PASS: perf inheritance and retained FD, cgroup pidlist, FUSE mount teardown");
    return 0;
}
