// SPDX-License-Identifier: GPL-2.0-only
/* A small real FUSE daemon. Tests protocol identities, not just mount(2). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fuse.h>
#include <linux/posix_acl_xattr.h>
#include <linux/posix_acl.h>
#include <sys/xattr.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while (0)
#define DS_FUSE_PASSTHROUGH (1ULL << 37)
#define DS_FUSE_ALLOW_IDMAP (1ULL << 40)
struct counts { unsigned requests, bad_ids, reads, writes, creates, setattr; };
static struct counts *counts;
static void map_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    CHECK(!close(fd));
}
static void token(int fd) { CHECK(write(fd,"!",1) == 1); }
static void wait_token(int fd) { char c; CHECK(read(fd,&c,1) == 1); }
static void send_fd(int sock, int fd)
{
    char c = '!'; struct iovec iov = {&c,1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct msghdr msg = {.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control)};
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level=SOL_SOCKET; cm->cmsg_type=SCM_RIGHTS; cm->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm),&fd,sizeof(fd));
    CHECK(sendmsg(sock,&msg,0)==1);
}
static int recv_fd(int sock)
{
    char c; struct iovec iov={&c,1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(sock,&msg,0)==1);
    struct cmsghdr *cm=CMSG_FIRSTHDR(&msg); int fd;
    CHECK(cm && cm->cmsg_level==SOL_SOCKET && cm->cmsg_type==SCM_RIGHTS && !(msg.msg_flags&MSG_CTRUNC));
    memcpy(&fd,CMSG_DATA(cm),sizeof(fd)); return fd;
}
static void reply(int fd, const struct fuse_in_header *in, int error, const void *data, size_t len)
{
    struct fuse_out_header out={.len=sizeof(out)+len,.error=-error,.unique=in->unique};
    struct iovec iov[]={{&out,sizeof(out)},{(void *)data,len}};
    CHECK(writev(fd,iov,len ? 2 : 1)==(ssize_t)(sizeof(out)+len));
}
static struct fuse_attr attr(uint64_t ino, unsigned uid, unsigned gid, size_t size, unsigned mode)
{
    return (struct fuse_attr){.ino=ino,.mode=mode,.nlink=ino==1?2:1,.uid=uid,.gid=gid,.size=size,.blksize=4096};
}
static void daemon_loop(int fd)
{
    char buffer[131072] __attribute__((aligned(8)));
    char content[4096]="namespace-fuse";
    size_t size=strlen(content); unsigned uid=0,gid=0,mode=S_IFREG|0600;
    int created=0;
    char acl_value[1024]; size_t acl_size=0;
    for (;;) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        if (n<0 && errno==EINTR) continue;
        if (n<0 && errno==ENODEV) break;
        CHECK(n>=(ssize_t)sizeof(struct fuse_in_header));
        const struct fuse_in_header *in=(void *)buffer;
        const void *body=in+1;
        CHECK(in->len==(unsigned)n);
        counts->requests++;
        if (in->opcode!=FUSE_INIT && in->opcode!=FUSE_FORGET && in->opcode!=FUSE_BATCH_FORGET && in->opcode!=FUSE_DESTROY) {
            if (in->uid>3 || in->gid>3) {
                fprintf(stderr,"unmapped request: opcode=%u uid=%u gid=%u\n",in->opcode,in->uid,in->gid);
                counts->bad_ids++;
            }
        }
        switch(in->opcode) {
        case FUSE_INIT: {
            const struct fuse_init_in *init=body;
            uint64_t offered=init->flags | (uint64_t)init->flags2<<32;
            CHECK(!(offered & (DS_FUSE_ALLOW_IDMAP|DS_FUSE_PASSTHROUGH)));
            struct fuse_init_out out={.major=7,.minor=31,.max_readahead=131072,
                .flags=FUSE_BIG_WRITES|FUSE_POSIX_ACL|FUSE_INIT_EXT,
                .flags2=(DS_FUSE_ALLOW_IDMAP|DS_FUSE_PASSTHROUGH)>>32,.max_write=65536,.time_gran=1};
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_LOOKUP: {
            const char *name=body; uint64_t node;
            if (!strcmp(name,"bpf-denied")) {
                struct {struct fuse_entry_out entry; uint64_t extra[4];} payload={
                    .entry={.nodeid=4,.attr=attr(4,0,0,0,S_IFREG|0600)},.extra={1,UINT64_MAX,0,0}};
                struct fuse_out_header out={.len=sizeof(out)+sizeof(payload),.unique=in->unique};
                struct iovec iov[]={{&out,sizeof(out)},{&payload,sizeof(payload)}};
                CHECK(writev(fd,iov,2)==-1 && errno==EINVAL);
                break;
            }
            if (!strcmp(name,"file")) node=2;
            else if (!strcmp(name,"created") && created) node=3;
            else { reply(fd,in,ENOENT,NULL,0); break; }
            struct fuse_entry_out out={.nodeid=node,.generation=1,
                .attr=attr(node,uid,gid,size,mode)};
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_GETATTR: {
            struct fuse_attr_out out={.attr=in->nodeid==1?attr(1,0,0,0,S_IFDIR|0777):attr(in->nodeid,uid,gid,size,mode)};
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_SETATTR: {
            const struct fuse_setattr_in *a=body;
            if (a->valid&FATTR_UID) { CHECK(a->uid<=3); uid=a->uid; }
            if (a->valid&FATTR_GID) { CHECK(a->gid<=3); gid=a->gid; }
            if (a->valid&FATTR_MODE) mode=(mode&S_IFMT)|(a->mode&07777);
            if (a->valid&FATTR_SIZE) { CHECK(a->size<=sizeof(content)); size=a->size; }
            counts->setattr++;
            struct fuse_attr_out out={.attr=attr(in->nodeid,uid,gid,size,mode)};
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_CREATE: {
            const struct fuse_create_in *a=body;
            CHECK(!strcmp((const char *)(a+1),"created")); created=1; size=0;
            uid=in->uid; gid=in->gid; mode=a->mode; counts->creates++;
            struct {struct fuse_entry_out e; struct fuse_open_out o;} out={
                .e={.nodeid=3,.generation=1,.attr=attr(3,uid,gid,size,mode)},
                .o={.fh=3,.open_flags=FOPEN_DIRECT_IO}};
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_OPEN: {
            struct fuse_open_out out={.fh=in->nodeid,.open_flags=FOPEN_DIRECT_IO};
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_READ: {
            const struct fuse_read_in *a=body;
            size_t len=a->offset>=size?0:size-a->offset;
            if (len>a->size) len=a->size;
            counts->reads++; reply(fd,in,0,content+(len?a->offset:0),len); break;
        }
        case FUSE_WRITE: {
            const struct fuse_write_in *a=body;
            CHECK(a->offset+a->size<=sizeof(content));
            memcpy(content+a->offset,a+1,a->size);
            if (a->offset+a->size>size) size=a->offset+a->size;
            struct fuse_write_out out={.size=a->size}; counts->writes++;
            reply(fd,in,0,&out,sizeof(out)); break;
        }
        case FUSE_UNLINK: created=0; reply(fd,in,0,NULL,0); break;
        case FUSE_SETXATTR: {
            const struct fuse_setxattr_in *a=body;
            const char *name=(const char *)body+FUSE_COMPAT_SETXATTR_IN_SIZE;
            CHECK(!strcmp(name,"system.posix_acl_access") && a->size<=sizeof(acl_value));
            memcpy(acl_value,name+strlen(name)+1,a->size); acl_size=a->size;
            const struct posix_acl_xattr_entry *e=(void *)(acl_value+sizeof(struct posix_acl_xattr_header));
            size_t count=(acl_size-sizeof(struct posix_acl_xattr_header))/sizeof(*e);
            for (size_t i=0;i<count;i++) {
                if (e[i].e_tag==ACL_USER) CHECK(e[i].e_id==1);
                if (e[i].e_tag==ACL_GROUP) CHECK(e[i].e_id==2);
                if (e[i].e_tag==ACL_MASK) mode=(mode&~0070)|(e[i].e_perm<<3);
            }
            reply(fd,in,0,NULL,0); break;
        }
        case FUSE_GETXATTR: {
            const struct fuse_getxattr_in *a=body; const char *name=(const char *)(a+1);
            if (in->nodeid!=2 || strcmp(name,"system.posix_acl_access") || !acl_size) { reply(fd,in,ENODATA,NULL,0); break; }
            if (!a->size) { struct fuse_getxattr_out out={.size=acl_size}; reply(fd,in,0,&out,sizeof(out)); }
            else if (a->size<acl_size) reply(fd,in,ERANGE,NULL,0);
            else reply(fd,in,0,acl_value,acl_size);
            break;
        }
        case FUSE_FLUSH: case FUSE_RELEASE: case FUSE_FSYNC: case FUSE_ACCESS:
            reply(fd,in,0,NULL,0); break;
        case FUSE_FORGET: case FUSE_BATCH_FORGET: break;
        case FUSE_DESTROY: reply(fd,in,0,NULL,0); break;
        default: reply(fd,in,ENOSYS,NULL,0); break;
        }
    }
    CHECK(!close(fd));
}
static void reap(pid_t child)
{
    int status; CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void deny_reader(int dir, int sibling)
{
    CHECK(!setgroups(0,NULL)); CHECK(!setresgid(1000,1000,1000)); CHECK(!setresuid(1000,1000,1000));
    if (sibling) {
        CHECK(!prctl(PR_SET_DUMPABLE,1));
        CHECK(!unshare(CLONE_NEWUSER));
        map_file("/proc/self/uid_map","0 1000 1\n");
        map_file("/proc/self/setgroups","deny\n");
        map_file("/proc/self/gid_map","0 1000 1\n");
    }
    errno=0; CHECK(openat(dir,"file",O_RDONLY)==-1 && errno==EACCES);
}
static void worker(int sock, const char *mountpoint, int mapped)
{
    int inside=mapped==-1;
    char options[160];
    if (!inside) {
        int ancestor=open("/dev/fuse",O_RDWR|O_CLOEXEC); CHECK(ancestor>=0);
        CHECK(!setgroups(0,NULL)); CHECK(!setresgid(1000,1000,1000)); CHECK(!setresuid(1000,1000,1000));
        CHECK(!prctl(PR_SET_DUMPABLE,1));
        CHECK(!unshare(CLONE_NEWUSER));
        token(sock); wait_token(sock);
        if (!mapped) {
            map_file("/proc/self/uid_map","0 1000 1\n");
            map_file("/proc/self/setgroups","deny\n");
            map_file("/proc/self/gid_map","0 1000 1\n");
        }
        CHECK(!unshare(CLONE_NEWNS));
        CHECK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
        snprintf(options,sizeof(options),"fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other,default_permissions",ancestor);
        errno=0; CHECK(mount("ds-fuse","/tmp/ds-fuse-bad","fuse",MS_NOSUID|MS_NODEV,options)==-1 && errno==EINVAL);
        CHECK(!close(ancestor));
    }
    int fd=open("/dev/fuse",O_RDWR|O_CLOEXEC); CHECK(fd>=0);
    snprintf(options,sizeof(options),"fd=%d,rootmode=40000,user_id=99,group_id=0",fd);
    errno=0; CHECK(mount("ds-fuse",mountpoint,"fuse",MS_NOSUID|MS_NODEV,options)==-1 && errno==EINVAL);
    snprintf(options,sizeof(options),"fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other,default_permissions",fd);
    CHECK(!mount("ds-fuse",mountpoint,"fuse",MS_NOSUID|MS_NODEV,options));
    pid_t client=getpid();
    pid_t daemon=fork(); CHECK(daemon>=0);
    if (!daemon) { CHECK(!prctl(PR_SET_PDEATHSIG,SIGKILL)); CHECK(getppid()==client); if (sock>=0) close(sock); daemon_loop(fd); _exit(0); }
    CHECK(!close(fd));
    int dir=open(mountpoint,O_PATH|O_DIRECTORY); CHECK(dir>=0);
    if (!inside) { send_fd(sock,dir); wait_token(sock); }
    struct stat st;
    errno=0; CHECK(fstatat(dir,"bpf-denied",&st,0)==-1 && errno==EIO);
    CHECK(!fstatat(dir,"file",&st,0)); CHECK(st.st_uid==0 && st.st_gid==0);
    int file=openat(dir,"file",O_RDWR); CHECK(file>=0);
    char data[64]={0}; CHECK(read(file,data,sizeof(data))==14); CHECK(!memcmp(data,"namespace-fuse",14));
    CHECK(pwrite(file,"roundtrip",9,0)==9); memset(data,0,sizeof(data));
    CHECK(pread(file,data,9,0)==9 && !memcmp(data,"roundtrip",9));
    if (mapped>0) {
        CHECK(!fchown(file,1,2)); CHECK(!fstat(file,&st)); CHECK(st.st_uid==1 && st.st_gid==2);
        CHECK(!fchown(file,0,0));
    } else { errno=0; CHECK(fchown(file,1,1)==-1 && errno==EINVAL); }
    if (mapped>0) {
        struct {struct posix_acl_xattr_header h; struct posix_acl_xattr_entry e[6];} acl={
            .h={.a_version=POSIX_ACL_XATTR_VERSION},.e={
                {ACL_USER_OBJ,6,ACL_UNDEFINED_ID},{ACL_USER,4,1},
                {ACL_GROUP_OBJ,0,ACL_UNDEFINED_ID},{ACL_GROUP,4,2},
                {ACL_MASK,4,ACL_UNDEFINED_ID},{ACL_OTHER,0,ACL_UNDEFINED_ID}}};
        CHECK(!fsetxattr(file,"system.posix_acl_access",&acl,sizeof(acl),0));
        char got[sizeof(acl)];
        CHECK(fgetxattr(file,"system.posix_acl_access",got,sizeof(got))==sizeof(acl));
        CHECK(!memcmp(&acl,got,sizeof(acl)));
        for (int actor=0;actor<3;actor++) {
            pid_t reader=fork(); CHECK(reader>=0);
            if (!reader) {
                CHECK(!setresgid(actor==1?2:3,actor==1?2:3,actor==1?2:3));
                CHECK(!setresuid(actor==0?1:3,actor==0?1:3,actor==0?1:3));
                int r=openat(dir,"file",O_RDONLY);
                if (actor==2) CHECK(r==-1 && errno==EACCES);
                else { CHECK(r>=0); char c; CHECK(read(r,&c,1)==1); close(r); }
                _exit(0);
            }
            reap(reader);
        }
        puts("DS_FUSE_ACL_PASS: mapped named user/group, roundtrip and access checks");
    }
    CHECK(!ftruncate(file,4)); CHECK(!fstat(file,&st) && st.st_size==4);
    CHECK(!close(file));
    file=openat(dir,"created",O_CREAT|O_EXCL|O_RDWR,0600); CHECK(file>=0);
    CHECK(write(file,"created-data",12)==12);
    CHECK(!fstat(file,&st) && st.st_uid==0 && st.st_gid==0);
    CHECK(!close(file)); CHECK(!unlinkat(dir,"created",0));
    CHECK(!close(dir));
    /* The daemon observes ENODEV and exits; connection keeps user_ns alive
     * until all device references and the RCU release have completed. */
    CHECK(!umount2(mountpoint,0)); reap(daemon);
    CHECK(counts->requests>10 && !counts->bad_ids && counts->reads>=2 && counts->writes>=2 && counts->creates==1 && counts->setattr);
    printf("DS_FUSE_IO_PASS: %s requests=%u uid/gid, cross-userns fd, ancestor/sibling isolation, create/read/write/chown/unmount\n",inside?"bubblewrap":mapped?"mapped-range":"unprivileged",counts->requests);
}
static unsigned live_namespaces(void)
{
    unsigned value; FILE *f=fopen("/sys/module/droidspaces_ns/parameters/userns_live","r");
    CHECK(f && fscanf(f,"%u",&value)==1); CHECK(!fclose(f)); return value;
}
static void wait_live(unsigned expected)
{
    for (unsigned n=0;n<100;n++) {
        if (live_namespaces()==expected) return;
        usleep(50000);
    }
    CHECK(live_namespaces()==expected);
}
static void retained_tests(const char *mountpoint)
{
    wait_live(0);
    for (unsigned round=0;round<8;round++) {
        int sock[2]; CHECK(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sock));
        pid_t child=fork(); CHECK(child>=0);
        if (!child) {
            close(sock[0]);
            CHECK(!setgroups(0,NULL)); CHECK(!setresgid(1000,1000,1000)); CHECK(!setresuid(1000,1000,1000));
            CHECK(!prctl(PR_SET_DUMPABLE,1)); CHECK(!unshare(CLONE_NEWUSER));
            map_file("/proc/self/uid_map","0 1000 1\n");
            map_file("/proc/self/setgroups","deny\n");
            map_file("/proc/self/gid_map","0 1000 1\n");
            CHECK(!unshare(CLONE_NEWNS)); CHECK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
            int fd=open("/dev/fuse",O_RDWR|O_CLOEXEC); CHECK(fd>=0);
            char options[128];
            snprintf(options,sizeof(options),"fd=%d,rootmode=40000,user_id=0,group_id=0",fd);
            CHECK(!mount("ds-fuse-retained",mountpoint,"fuse",MS_NODEV|MS_NOSUID,options));
            int dir=open(mountpoint,O_PATH|O_DIRECTORY); CHECK(dir>=0);
            send_fd(sock[1],fd); send_fd(sock[1],dir);
            _exit(0);
        }
        close(sock[1]); int device=recv_fd(sock[0]), dir=recv_fd(sock[0]);
        reap(child); close(sock[0]);
        wait_live(1);
        CHECK(!close(device)); wait_live(1);
        CHECK(!close(dir)); wait_live(0);
    }
    puts("DS_FUSE_RETAINED_PASS: 8 creator-exit rounds, device and mount FD ownership, RCU cleanup");
}
int main(int argc, char **argv)
{
    int mapped=argc==2 && !strcmp(argv[1],"--mapped");
    int retained=argc==2 && !strcmp(argv[1],"--retained");
    int inside=argc==2 && !strcmp(argv[1],"--inside");
    setbuf(stdout,NULL); alarm(45);
    CHECK(!unshare(CLONE_NEWNS)); CHECK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
    if (!inside) CHECK(!chmod("/dev/fuse",0666));
    char mountpoint[]="/tmp/ds-fuse-XXXXXX"; CHECK(mkdtemp(mountpoint));
    if (!inside) CHECK(!chown(mountpoint,1000,1000));
    CHECK(!chmod(mountpoint,0700));
    if (retained) { retained_tests(mountpoint); CHECK(!rmdir(mountpoint)); return 0; }
    CHECK(!mkdir("/tmp/ds-fuse-bad",0777));
    counts=mmap(NULL,sizeof(*counts),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0); CHECK(counts!=MAP_FAILED);
    if (inside) {
        worker(-1,mountpoint,-1);
        CHECK(!rmdir(mountpoint)); CHECK(!rmdir("/tmp/ds-fuse-bad"));
        puts("DS_FUSE_BUBBLEWRAP_PASS"); return 0;
    }
    int sock[2]; CHECK(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sock));
    pid_t child=fork(); CHECK(child>=0);
    if (!child) { close(sock[0]); worker(sock[1],mountpoint,mapped); _exit(0); }
    close(sock[1]); wait_token(sock[0]);
    if (mapped>0) {
        char path[128]; snprintf(path,sizeof(path),"/proc/%d/uid_map",child); map_file(path,"0 1000 4\n");
        snprintf(path,sizeof(path),"/proc/%d/setgroups",child); map_file(path,"deny\n");
        snprintf(path,sizeof(path),"/proc/%d/gid_map",child); map_file(path,"0 1000 4\n");
    }
    token(sock[0]); int dir=recv_fd(sock[0]);
    for (int sibling=0;sibling<2;sibling++) {
        pid_t reader=fork(); CHECK(reader>=0);
        if (!reader) { deny_reader(dir,sibling); _exit(0); }
        reap(reader);
    }
    close(dir); token(sock[0]); reap(child); close(sock[0]);
    CHECK(!rmdir(mountpoint)); CHECK(!rmdir("/tmp/ds-fuse-bad"));
    munmap(counts,sizeof(*counts));
    return 0;
}
