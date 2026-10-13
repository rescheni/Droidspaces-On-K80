// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/keyctl.h>
#include <linux/mount.h>
#include <grp.h>
#include <linux/nsfs.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#define CHECK(expr) do { if (!(expr)) { dprintf(2,"FAIL userns-policy %d: %s errno=%d\n",__LINE__,#expr,errno);exit(1); } } while (0)
static uint64_t caps(int permitted)
{
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct d[2];
    CHECK(syscall(SYS_capget,&h,d)==0);
    return permitted ? ((uint64_t)d[1].permitted<<32)|d[0].permitted :
        ((uint64_t)d[1].effective<<32)|d[0].effective;
}
static void wait_ok(pid_t child)
{
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void map_child(pid_t child,const char *name)
{
    char path[100];
    snprintf(path,sizeof(path),"/proc/%d/%s",child,name);
    int fd=open(path,O_WRONLY);
    const char *map="0 1000 2\n";
    CHECK(fd>=0 && write(fd,map,strlen(map))==(ssize_t)strlen(map) && close(fd)==0);
}
static void host_file_denied(void)
{
    CHECK(open("/root-only",O_RDONLY)==-1 && errno==EACCES);
}
static void ptrace_owned_mm(void)
{
    int ready[2],status; char byte=0;
    CHECK(pipe(ready)==0);
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        close(ready[0]);
        CHECK(setresuid(1,1,1)==0);
        CHECK(prctl(PR_SET_DUMPABLE,0)==0);
        CHECK(write(ready[1],&byte,1)==1);
        for (;;) pause();
    }
    close(ready[1]);
    CHECK(read(ready[0],&byte,1)==1);
    close(ready[0]);
    CHECK(ptrace(PTRACE_ATTACH,child,NULL,NULL)==0);
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    CHECK(ptrace(PTRACE_DETACH,child,NULL,NULL)==0);
    CHECK(kill(child,SIGKILL)==0);
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
}
static void after_exec(const char *mode)
{
    int root=strcmp(mode,"--after-drop")!=0;
    int mapped=!strcmp(mode,"--after-mapped");
    CHECK(getuid()==((root && !mapped) ? 0 : 1));
    CHECK(geteuid()==(root ? 0 : 1));
    CHECK(getgid()==0 && getegid()==0);
    host_file_denied();
    if (root) {
        CHECK(caps(0)&(UINT64_C(1)<<CAP_SYS_ADMIN));
        CHECK(unshare(CLONE_NEWUTS)==0);
        CHECK(sethostname("exec-ns",7)==0);
        ptrace_owned_mm();
    } else {
        CHECK(caps(0)==0 && caps(1)==0);
        CHECK(sethostname("forbidden",9)==-1 && errno==EPERM);
        CHECK(setuid(0)==-1 && errno==EPERM);
    }
}
static void *thread_exec(void *unused)
{
    (void)unused;
    execl("/userns-policy-test","userns-policy-test","--after-root",NULL);
    CHECK(0);
    return NULL;
}
static void vfs_policy(void)
{
    struct stat st;
    CHECK(unshare(CLONE_NEWNS)==0);
    CHECK(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)==0);
    CHECK(mkdir("/tmp/vfs-owned",0700)==0 || errno==EEXIST);
    CHECK(mount("tmpfs","/tmp/vfs-owned","tmpfs",0,"mode=0755")==0);
    int fd=open("/tmp/vfs-owned/file",O_CREAT|O_RDWR,0000);
    CHECK(fd>=0);
    CHECK(fchown(fd,1,1)==0);
    CHECK(fstat(fd,&st)==0 && st.st_uid==1 && st.st_gid==1);
    CHECK(access("/tmp/vfs-owned/file",R_OK|W_OK)==0);
    CHECK(chmod("/tmp/vfs-owned/file",0600)==0);
    CHECK(fchown(fd,2,(gid_t)-1)==-1 && errno==EINVAL);
    CHECK(fchown(fd,0,0)==0);
    close(fd);
    CHECK(chown("/root-only",0,0)==-1 && errno==EPERM);
    CHECK(chmod("/root-only",0777)==-1 && errno==EPERM);
    CHECK(mkdir("/tmp/vfs-owned/sys",0755)==0);
    CHECK(unshare(CLONE_NEWNET)==0);
    CHECK(mount("sysfs","/tmp/vfs-owned/sys","sysfs",MS_RDONLY,NULL)==0);
    CHECK(stat("/tmp/vfs-owned/sys/class/net/lo",&st)==0 && st.st_uid==0 && st.st_gid==0);
    CHECK(chroot("/tmp/vfs-owned")==0 && chdir("/")==0);
    CHECK(stat("/file",&st)==0 && st.st_uid==0 && st.st_gid==0);
    CHECK(open("/root-only",O_RDONLY)==-1 && errno==ENOENT);
}
static long namespace_limit(void)
{
    char value[64]={0};
    int fd=open("/proc/sys/user/max_user_namespaces",O_RDONLY);
    CHECK(fd>=0 && read(fd,value,sizeof(value)-1)>0 && close(fd)==0);
    return strtol(value,NULL,10);
}
static void quota_policy(void)
{
    int fd=open("/proc/sys/user/max_user_namespaces",O_WRONLY);
    CHECK(fd>=0 && write(fd,"0\n",2)==2 && close(fd)==0);
    CHECK(namespace_limit()==0);
    CHECK(unshare(CLONE_NEWUSER)==-1 && errno==ENOSPC);
}
static void filecap_policy(void)
{
    const char *path="/test-ext4/userns-policy-fcaps";
    struct vfs_cap_data cap={.magic_etc=htole32(VFS_CAP_REVISION_2|VFS_CAP_FLAGS_EFFECTIVE)};
    struct vfs_ns_cap_data disk={0};
    cap.data[0].permitted=htole32(1U<<CAP_NET_BIND_SERVICE);
    CHECK(setxattr(path,"security.capability",&cap,sizeof(cap),0)==0);
    CHECK(getxattr(path,"security.capability",&disk,sizeof(disk))==sizeof(cap));
    CHECK(le32toh(disk.magic_etc)==(VFS_CAP_REVISION_2|VFS_CAP_FLAGS_EFFECTIVE));
    disk.magic_etc=htole32(VFS_CAP_REVISION_3|VFS_CAP_FLAGS_EFFECTIVE);
    disk.rootid=htole32(2);
    CHECK(setxattr(path,"security.capability",&disk,sizeof(disk),0)==-1 && errno==EINVAL);
    CHECK(setxattr("/root-only","security.capability",&cap,sizeof(cap),0)==-1 && errno==EPERM);
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(setresuid(1,1,1)==0);
        execl(path,"userns-policy-test","--after-fcaps",NULL);
        CHECK(0);
    }
    wait_ok(child);
}
static void proc_identity(int fd, unsigned int id)
{
    char line[512]; unsigned int a,b,c,d; int saw_uid=0,saw_gid=0;
    FILE *file=fdopen(fd,"r");
    CHECK(file!=NULL);
    while (fgets(line,sizeof(line),file)) {
        if (sscanf(line,"Uid: %u %u %u %u",&a,&b,&c,&d)==4) {
            CHECK(a==id && b==id && c==id && d==id); saw_uid=1;
        }
        if (sscanf(line,"Gid: %u %u %u %u",&a,&b,&c,&d)==4) {
            CHECK(a==id && b==id && c==id && d==id); saw_gid=1;
        }
    }
    CHECK(saw_uid && saw_gid && fclose(file)==0);
}
static void seccomp_policy(void)
{
    struct sock_filter instructions[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={.len=sizeof(instructions)/sizeof(instructions[0]),.filter=instructions};
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    CHECK(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0);
    CHECK(syscall(SYS_getppid)==-1 && errno==EPERM);
    CHECK(syscall(SYS_getpid)>0);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,0,0,0,0)==-1 && errno==EINVAL);
}
static void socket_rights(int sockets[2])
{
    int pipefd[2],received[2],zero=0; char byte='r';
    CHECK(pipe2(pipefd,O_CLOEXEC)==0);
    CHECK(setsockopt(sockets[1],SOL_SOCKET,SO_PASSCRED,&zero,sizeof(zero))==0);
    union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control={0};
    struct iovec iov={.iov_base=&byte,.iov_len=1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS; c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&pipefd[0],sizeof(int));
    CHECK(sendmsg(sockets[0],&msg,0)==1);
    for (int i=0;i<2;i++) {
        memset(&control,0,sizeof(control));
        msg.msg_controllen=sizeof(control.data); msg.msg_flags=0;
        CHECK(recvmsg(sockets[1],&msg,MSG_CMSG_CLOEXEC|(i ? 0 : MSG_PEEK))==1);
        CHECK(!(msg.msg_flags&MSG_CTRUNC));
        c=CMSG_FIRSTHDR(&msg);
        CHECK(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS);
        memcpy(&received[i],CMSG_DATA(c),sizeof(int));
        CHECK(fcntl(received[i],F_GETFD)&FD_CLOEXEC);
    }
    CHECK(received[0]!=received[1]);
    close(received[0]); close(pipefd[0]);
    CHECK(write(pipefd[1],"p",1)==1 && read(received[1],&byte,1)==1 && byte=='p');
    c=CMSG_FIRSTHDR(&msg); c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&received[1],sizeof(int));
    msg.msg_controllen=sizeof(control.data); msg.msg_flags=0;
    CHECK(sendmsg(sockets[0],&msg,0)==1);
    msg.msg_control=NULL; msg.msg_controllen=0;
    CHECK(recvmsg(sockets[1],&msg,0)==1 && (msg.msg_flags&MSG_CTRUNC));
    close(received[1]); close(pipefd[1]);
}
static void socket_policy(void)
{
    gid_t groups[2]={0,1};
    CHECK(setgroups(2,groups)==0);
    const int types[]={SOCK_STREAM,SOCK_DGRAM,SOCK_SEQPACKET};
    for (unsigned t=0;t<sizeof(types)/sizeof(types[0]);t++) {
        int type=types[t];
        int sockets[2],one=1; struct ucred peer;
        socklen_t length=sizeof(peer);
        CHECK(socketpair(AF_UNIX,type,0,sockets)==0);
        CHECK(getsockopt(sockets[0],SOL_SOCKET,SO_PEERCRED,&peer,&length)==0);
        dprintf(1,"SOCKET_PEER: pid=%d uid=%u gid=%u expected=%d/0/0\n",peer.pid,peer.uid,peer.gid,getpid());
        CHECK(length==sizeof(peer) && peer.pid==getpid() && peer.uid==0 && peer.gid==0);
        gid_t peers[2]={65534,65534}; length=sizeof(peers);
        CHECK(getsockopt(sockets[0],SOL_SOCKET,SO_PEERGROUPS,peers,&length)==0);
        CHECK(length==sizeof(peers) && peers[0]==0 && peers[1]==1);
        length=sizeof(gid_t);
        CHECK(getsockopt(sockets[0],SOL_SOCKET,SO_PEERGROUPS,peers,&length)==-1 && errno==ERANGE && length==sizeof(peers));
        CHECK(setsockopt(sockets[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
        for (int explicit=0;explicit<2;explicit++) {
            char byte='s';
            union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(struct ucred))]; } control={0};
            struct iovec iov={.iov_base=&byte,.iov_len=1};
            struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1};
            if (explicit) {
                msg.msg_control=control.data; msg.msg_controllen=sizeof(control.data);
                struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
                c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_CREDENTIALS;
                c->cmsg_len=CMSG_LEN(sizeof(struct ucred));
                struct ucred own={.pid=getpid(),.uid=0,.gid=0};
                memcpy(CMSG_DATA(c),&own,sizeof(own));
            }
            CHECK(sendmsg(sockets[0],&msg,0)==1);
            memset(&control,0,sizeof(control));
            msg.msg_control=control.data; msg.msg_controllen=sizeof(control.data);
            CHECK(recvmsg(sockets[1],&msg,0)==1 && !(msg.msg_flags&MSG_CTRUNC));
            struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
            CHECK(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS);
            memcpy(&peer,CMSG_DATA(c),sizeof(peer));
            dprintf(1,"SOCKET_CMSG: type=%d explicit=%d pid=%d uid=%u gid=%u\n",type,explicit,peer.pid,peer.uid,peer.gid);
            CHECK(peer.pid==getpid() && peer.uid==0 && peer.gid==0);
        }
        /* Unmapped claims must be rejected before any message is queued. */
        char byte='f';
        union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(struct ucred))]; } control={0};
        struct iovec iov={.iov_base=&byte,.iov_len=1};
        struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
        c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_CREDENTIALS;
        c->cmsg_len=CMSG_LEN(sizeof(struct ucred));
        struct ucred claim={.pid=getpid(),.uid=2,.gid=0};
        memcpy(CMSG_DATA(c),&claim,sizeof(claim));
        CHECK(sendmsg(sockets[0],&msg,MSG_DONTWAIT)==-1 && errno==EINVAL);
        pid_t unpriv=fork();
        CHECK(unpriv>=0);
        if (!unpriv) {
            CHECK(setresgid(1,1,1)==0 && setresuid(1,1,1)==0);
            claim.pid=getpid(); claim.uid=0;
            memcpy(CMSG_DATA(c),&claim,sizeof(claim));
            CHECK(sendmsg(sockets[0],&msg,MSG_DONTWAIT)==-1 && errno==EPERM);
            _exit(0);
        }
        wait_ok(unpriv);
        CHECK(recv(sockets[1],&byte,1,MSG_DONTWAIT)==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
        socket_rights(sockets);
        close(sockets[0]); close(sockets[1]);
    }
}
static void receive_identity(int fd, uid_t uid, gid_t gid, pid_t pid)
{
    char byte=0; struct ucred peer;
    union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(peer))]; } control={0};
    struct iovec iov={.iov_base=&byte,.iov_len=1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    CHECK(recvmsg(fd,&msg,0)==1 && !(msg.msg_flags&MSG_CTRUNC));
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    CHECK(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS);
    memcpy(&peer,CMSG_DATA(c),sizeof(peer));
    CHECK(peer.uid==uid && peer.gid==gid && peer.pid==pid);
}
static void netlink_policy(void)
{
    int fd[2],one=1;
    struct sockaddr_nl addr[2]={{.nl_family=AF_NETLINK},{.nl_family=AF_NETLINK}};
    for (int i=0;i<2;i++) {
        socklen_t size=sizeof(addr[i]);
        fd[i]=socket(AF_NETLINK,SOCK_DGRAM,NETLINK_USERSOCK);
        CHECK(fd[i]>=0 && bind(fd[i],(struct sockaddr *)&addr[i],sizeof(addr[i]))==0);
        CHECK(getsockname(fd[i],(struct sockaddr *)&addr[i],&size)==0);
    }
    CHECK(setsockopt(fd[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
    CHECK(sendto(fd[0],"n",1,0,(struct sockaddr *)&addr[1],sizeof(addr[1]))==1);
    receive_identity(fd[1],0,0,getpid());
    close(fd[0]);close(fd[1]);
}
static void keyring_parent_policy(void)
{
    struct stat before,after;
    CHECK(stat("/proc/self/ns/user",&before)==0);
    long original=syscall(SYS_keyctl,KEYCTL_JOIN_SESSION_KEYRING,NULL,0,0,0);
    CHECK(original>0);
    int ready[2]; CHECK(pipe(ready)==0);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        close(ready[0]);
        long replacement=syscall(SYS_keyctl,KEYCTL_JOIN_SESSION_KEYRING,NULL,0,0,0);
        CHECK(replacement>0 && replacement!=original);
        CHECK(write(ready[1],&replacement,sizeof(replacement))==sizeof(replacement));
        CHECK(syscall(SYS_keyctl,KEYCTL_SESSION_TO_PARENT,0,0,0,0)==0);
        _exit(0);
    }
    close(ready[1]); long replacement;
    CHECK(read(ready[0],&replacement,sizeof(replacement))==sizeof(replacement));
    close(ready[0]); wait_ok(child);
    CHECK(syscall(SYS_keyctl,KEYCTL_GET_KEYRING_ID,KEY_SPEC_SESSION_KEYRING,0,0,0)==replacement);
    CHECK(stat("/proc/self/ns/user",&after)==0 && before.st_ino==after.st_ino);
    CHECK(getuid()==0 && getgid()==0);
    host_file_denied();
}
static void policy_case(int mode)
{
    int ready[2],go[2]; char byte=0;
    long host_limit=namespace_limit();
    int host_ns=open("/proc/self/ns/user",O_RDONLY);
    CHECK(host_ns>=0);
    CHECK(pipe(ready)==0 && pipe(go)==0);
    int cross[2]={-1,-1}; pid_t host_pid=getpid();
    if (mode==18) {
        int one=1;
        CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,cross)==0);
        CHECK(setsockopt(cross[0],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
        CHECK(setsockopt(cross[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
    }
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        if (mode==18) close(cross[0]);
        close(ready[0]); close(go[1]);
        CHECK(setgid(1000)==0 && setuid(1000)==0);
        CHECK(prctl(PR_SET_DUMPABLE,1)==0);
        int status_fd=open("/proc/self/status",O_RDONLY);
        CHECK(status_fd>=0);
        CHECK(unshare(CLONE_NEWUSER)==0);
        CHECK(write(ready[1],&byte,1)==1 && read(go[0],&byte,1)==1);
        if (mode!=15) { close(ready[1]); close(go[0]); }
        CHECK(getuid()==0 && getgid()==0);
        proc_identity(status_fd,1000);
        proc_identity(open("/proc/self/status",O_RDONLY),0);
        CHECK(setns(host_ns,CLONE_NEWUSER)==-1 && errno==EPERM);
        close(host_ns);
        CHECK(unshare(CLONE_NEWUTS)==0);
        CHECK(caps(0)&(UINT64_C(1)<<CAP_SYS_ADMIN));
        host_file_denied();
        if (mode==21) {
            CHECK(unshare(CLONE_NEWNS|CLONE_NEWCGROUP)==0);
            for (int i=0;i<8;i++) {
                int fd=syscall(SYS_fsopen,"cgroup2",FSOPEN_CLOEXEC);
                CHECK(fd>=0 && close(fd)==0);
            }
        } else if (mode==20) {
            keyring_parent_policy();
        } else if (mode==19) {
            netlink_policy();
        } else if (mode==18) {
            struct ucred peer; socklen_t size=sizeof(peer);
            CHECK(getsockopt(cross[1],SOL_SOCKET,SO_PEERCRED,&peer,&size)==0);
            CHECK(peer.uid==65534 && peer.gid==65534 && peer.pid==host_pid);
            receive_identity(cross[1],65534,65534,host_pid);
            CHECK(send(cross[1],&byte,1,0)==1);
            close(cross[1]);
        } else if (mode==17) {
            socket_policy();
        } else if (mode==16) {
            seccomp_policy();
        } else if (mode==15) {
            CHECK(unshare(CLONE_NEWNET|CLONE_NEWCGROUP|CLONE_NEWTIME)==0);
            CHECK(write(ready[1],&byte,1)==1 && read(go[0],&byte,1)==1);
        } else if (mode==14) {
            filecap_policy();
        } else if (mode==12) {
            vfs_policy();
        } else if (mode==13) {
            quota_policy();
        } else if (mode==1) {
            CHECK(seteuid(1)==0 && caps(0)==0 && caps(1)!=0);
            CHECK(sethostname("forbidden",9)==-1 && errno==EPERM);
            CHECK(seteuid(0)==0 && (caps(0)&(UINT64_C(1)<<CAP_SYS_ADMIN)));
        } else if (mode==2) {
            CHECK(setfsuid(1)==0 && !(caps(0)&(UINT64_C(1)<<CAP_DAC_OVERRIDE)));
            CHECK(setfsuid(0)==1 && (caps(0)&(UINT64_C(1)<<CAP_DAC_OVERRIDE)));
        } else if (mode==10) {
            pthread_t worker;
            CHECK(pthread_create(&worker,NULL,thread_exec,NULL)==0);
            for (;;) pause();
        } else if (mode==3) {
            execl("/userns-policy-test","userns-policy-test","--after-root",NULL);
            CHECK(0);
        } else if (mode==6 || mode==7) {
            execl(mode==6 ? "/userns-policy-suid-host" : "/userns-policy-sgid-host",
                "userns-policy-test","--after-root",NULL);
            CHECK(0);
        } else {
            if (mode==5) CHECK(prctl(PR_SET_KEEPCAPS,1)==0);
            CHECK(setresuid(1,1,1)==0 && getuid()==1 && caps(0)==0);
            CHECK((caps(1)!=0)==(mode==5));
            CHECK(sethostname("forbidden",9)==-1 && errno==EPERM);
            CHECK(setuid(0)==-1 && errno==EPERM);
            if (mode==8 || mode==9) {
                if (mode==9) CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
                execl("/userns-policy-suid-mapped","userns-policy-test",
                    mode==8 ? "--after-mapped" : "--after-drop",NULL);
                CHECK(0);
            }
            if (mode>=4) {
                execl("/userns-policy-test","userns-policy-test","--after-drop",NULL);
                CHECK(0);
            }
        }
        _exit(0);
    }
    if (mode==18) close(cross[1]);
    close(ready[1]); close(go[0]);
    CHECK(read(ready[0],&byte,1)==1);
    map_child(child,"uid_map"); map_child(child,"gid_map");
    if (mode==11) {
        char path[100]; struct stat target;
        snprintf(path,sizeof(path),"/proc/%d/ns/user",child);
        int fd=open(path,O_RDONLY);
        CHECK(fd>=0 && fstat(fd,&target)==0);
        pid_t visitor=fork();
        CHECK(visitor>=0);
        if (!visitor) {
            struct stat joined;
            CHECK(setns(fd,CLONE_NEWUSER)==0);
            CHECK(stat("/proc/self/ns/user",&joined)==0 && joined.st_ino==target.st_ino);
            CHECK(getuid()==65534);
            CHECK(setgid(0)==0 && setuid(0)==0 && getuid()==0);
            host_file_denied();
            CHECK(setns(host_ns,CLONE_NEWUSER)==-1 && errno==EPERM);
            _exit(0);
        }
        wait_ok(visitor);
        close(fd);
    }
    close(host_ns);
    CHECK(write(go[1],&byte,1)==1);
    if (mode==18) {
        CHECK(send(cross[0],&byte,1,0)==1);
        receive_identity(cross[0],1000,1000,child);
        close(cross[0]);
    }
    int retained[3]={-1,-1,-1};
    if (mode==15) {
        const char *names[]={"net","cgroup","time_for_children"};
        CHECK(read(ready[0],&byte,1)==1);
        for (int i=0;i<3;i++) {
            char path[100];
            snprintf(path,sizeof(path),"/proc/%d/ns/%s",child,names[i]);
            retained[i]=open(path,O_RDONLY);
            CHECK(retained[i]>=0);
        }
        CHECK(write(go[1],&byte,1)==1);
    }
    close(ready[0]); close(go[1]);
    wait_ok(child);
    if (mode==15) {
        struct stat previous={0};
        for (int i=0;i<3;i++) {
            uid_t uid=(uid_t)-1; struct stat owner;
            int fd=ioctl(retained[i],NS_GET_USERNS);
            CHECK(fd>=0 && ioctl(fd,NS_GET_OWNER_UID,&uid)==0 && uid==1000);
            CHECK(fstat(fd,&owner)==0);
            CHECK(!i || owner.st_ino==previous.st_ino);
            previous=owner;
            close(fd); close(retained[i]);
        }
    }
    CHECK(namespace_limit()==host_limit);
    if (mode==14) {
        struct vfs_ns_cap_data disk={0};
        CHECK(getxattr("/test-ext4/userns-policy-fcaps","security.capability",&disk,sizeof(disk))==sizeof(disk));
        CHECK(le32toh(disk.magic_etc)==(VFS_CAP_REVISION_3|VFS_CAP_FLAGS_EFFECTIVE));
        CHECK(le32toh(disk.rootid)==1000);
        pid_t host=fork();
        CHECK(host>=0);
        if (!host) {
            CHECK(setgid(1001)==0 && setuid(1001)==0);
            execl("/test-ext4/userns-policy-fcaps","userns-policy-test","--after-host-fcaps",NULL);
            CHECK(0);
        }
        wait_ok(host);
    }
    printf("DS_USERNS_POLICY_CASE=%d passed\n",mode);
}
struct map_writer {
    int fd;
    const char *map;
    ssize_t result;
    int error;
    pthread_barrier_t *start;
};
static void *write_map_racing(void *arg)
{
    struct map_writer *writer=arg;
    pthread_barrier_wait(writer->start);
    writer->result=write(writer->fd,writer->map,strlen(writer->map));
    writer->error=errno;
    return NULL;
}
static void mapping_fd_exit_races(void)
{
    for (int round=0;round<32;round++) {
        int ready[2],go[2]; char byte=0,path[100],map[200];
        CHECK(pipe(ready)==0 && pipe(go)==0);
        pid_t child=fork(); CHECK(child>=0);
        if (!child) {
            close(ready[0]);close(go[1]);
            CHECK(setgid(1000)==0 && setuid(1000)==0);
            CHECK(prctl(PR_SET_DUMPABLE,1)==0 && unshare(CLONE_NEWUSER)==0);
            CHECK(write(ready[1],&byte,1)==1 && read(go[0],&byte,1)==1);
            _exit(0);
        }
        close(ready[1]);close(go[0]);
        CHECK(read(ready[0],&byte,1)==1);close(ready[0]);
        snprintf(path,sizeof(path),"/proc/%d/uid_map",child);
        int fd[2]={open(path,O_WRONLY),open(path,O_WRONLY)};
        int view=open(path,O_RDONLY);
        snprintf(path,sizeof(path),"/proc/%d/gid_map",child);
        int gid=open(path,O_RDWR);
        snprintf(path,sizeof(path),"/proc/%d/ns/user",child);
        int ns=open(path,O_RDONLY);
        CHECK(fd[0]>=0 && fd[1]>=0 && view>=0 && gid>=0 && ns>=0);
        pthread_barrier_t start; pthread_t threads[2];
        CHECK(pthread_barrier_init(&start,NULL,3)==0);
        struct map_writer writers[2]={
            {.fd=fd[0],.map="0 1000 1\n",.start=&start},
            {.fd=fd[1],.map="0 1001 1\n",.start=&start},
        };
        for (int i=0;i<2;i++) CHECK(pthread_create(&threads[i],NULL,write_map_racing,&writers[i])==0);
        pthread_barrier_wait(&start);
        CHECK(write(go[1],&byte,1)==1);close(go[1]);
        wait_ok(child);
        for (int i=0;i<2;i++) CHECK(pthread_join(threads[i],NULL)==0);
        CHECK(pthread_barrier_destroy(&start)==0);
        int winner=writers[0].result>0 ? 0 : 1;
        CHECK(writers[winner].result==(ssize_t)strlen(writers[winner].map));
        CHECK(writers[1-winner].result==-1 && writers[1-winner].error==EPERM);
        ssize_t size=read(view,map,sizeof(map)-1); CHECK(size>0); map[size]=0;
        unsigned int inside,outside,length;
        CHECK(sscanf(map,"%u %u %u",&inside,&outside,&length)==3);
        CHECK(inside==0 && outside==1000u+(unsigned)winner && length==1);
        /* Open mapping files retain the namespace after its last task exits. */
        CHECK(write(gid,"0 1000 1\n",9)==9 && lseek(gid,0,SEEK_SET)==0);
        size=read(gid,map,sizeof(map)-1);CHECK(size>0);map[size]=0;
        CHECK(sscanf(map,"%u %u %u",&inside,&outside,&length)==3);
        CHECK(inside==0 && outside==1000 && length==1);
        uid_t owner; CHECK(ioctl(ns,NS_GET_OWNER_UID,&owner)==0 && owner==1000);
        close(fd[0]);close(fd[1]);close(view);close(gid);close(ns);
    }
    puts("DS_USERNS_MAPPING_RACE_PASS: 32 competing map writes, task exits and retained mapping FDs");
}
int main(int argc,char **argv)
{
    if (argc==2) {
        if (!strcmp(argv[1],"--after-fcaps")) {
            CHECK(getuid()==1 && geteuid()==1);
            CHECK(caps(0)==(UINT64_C(1)<<CAP_NET_BIND_SERVICE));
            CHECK(caps(1)==caps(0));
            host_file_denied();
        } else if (!strcmp(argv[1],"--after-host-fcaps")) {
            CHECK(getuid()==1001 && geteuid()==1001 && caps(0)==0 && caps(1)==0);
        } else after_exec(argv[1]);
        return 0;
    }
    for (int i=0;i<22;i++) policy_case(i);
    for (int i=0;i<8;i++) policy_case(15);
    mapping_fd_exit_races();
    puts("DS_USERNS_POLICY_PASS: mapped privilege drop, effective/fs UID, exec, keepcaps, unmapped/mapped set-ID, no-new-privs, mm ownership, thread exec, setns, VFS, quotas, filecaps and retained namespace owners");
    return 0;
}
