// SPDX-License-Identifier: GPL-2.0-only
/*
 * Device probe for the PID / IPC namespace module.
 *
 * Freestanding AArch64 static binary: it uses raw syscalls only, so the test
 * exercises exactly the entries the module replaces (SysV IPC, POSIX mqueue,
 * unshare) without depending on any libc or userspace IPC tooling.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -nostdlib -static -O2 -o nsprobe nsprobe.c
 */
typedef unsigned long u64;
typedef long s64;

#define SYS_write 64
#define SYS_exit_group 94
#define SYS_clone 220
#define SYS_wait4 260
#define SYS_getpid 172
#define SYS_getppid 173
#define SYS_unshare 97
#define SYS_msgget 186
#define SYS_msgctl 187
#define SYS_msgsnd 189
#define SYS_msgrcv 188
#define SYS_semget 190
#define SYS_semctl 191
#define SYS_shmget 194
#define SYS_shmctl 195
#define SYS_mq_open 180
#define SYS_mq_unlink 181
#define SYS_mq_timedsend 182
#define SYS_mq_timedreceive 183
#define SYS_mq_getsetattr 185

#define CLONE_NEWIPC 0x08000000
#define CLONE_NEWPID 0x20000000
#define SIGCHLD 17

#define IPC_PRIVATE 0
#define IPC_CREAT 01000
#define IPC_RMID 0
#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100

#define ENOENT 2

struct mq_attr {
	s64 flags, maxmsg, msgsize, curmsgs;
};

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	register long x8 __asm__("x8") = n;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
			 : "memory");
	return x0;
}

static u64 slen(const char *s)
{
	u64 n = 0;

	while (s[n])
		n++;
	return n;
}

static void out(const char *s)
{
	sc(SYS_write, 1, (long)s, slen(s), 0, 0, 0);
}

static void out_pair(const char *label, long value)
{
	char buf[24];
	int i = 24;

	out(label);
	if (value < 0) {
		out("-");
		value = -value;
	}
	do {
		buf[--i] = '0' + value % 10;
		value /= 10;
	} while (value);
	sc(SYS_write, 1, (long)(buf + i), 24 - i, 0, 0, 0);
	out("\n");
}

static void report(const char *name, int ok)
{
	out(ok ? "PASS " : "FAIL ");
	out(name);
	out("\n");
}

static long fork_child(void)
{
	return sc(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
}

static int wait_child(void)
{
	long status = 0;

	if (sc(SYS_wait4, -1, (long)&status, 0, 0, 0, 0) < 0)
		return -1;
	return (status >> 8) & 0xff;
}

/* A private IPC key; SysV ids are per namespace, so the key is what the
 * child re-uses to prove the object is unreachable from a new IPC namespace.
 */
#define PROBE_KEY 0x44533231

static int test_sysv(void)
{
	char msg[8] = "probe";
	char rcv[8];
	long msgid, semid, shmid, sent, got;
	long pid, code;

	msgid = sc(SYS_msgget, PROBE_KEY, 0666 | IPC_CREAT, 0, 0, 0, 0);
	semid = sc(SYS_semget, PROBE_KEY, 1, 0666 | IPC_CREAT, 0, 0, 0);
	shmid = sc(SYS_shmget, PROBE_KEY, 4096, 0666 | IPC_CREAT, 0, 0, 0);
	report("sysv-create", msgid >= 0 && semid >= 0 && shmid >= 0);

	sent = sc(SYS_msgsnd, msgid, (long)msg, 8, 0, 0, 0);
	got = sc(SYS_msgrcv, msgid, (long)rcv, 8, 0, 0, 0);
	report("sysv-message-roundtrip", sent == 0 && got == 8 && rcv[0] == 'p');

	pid = fork_child();
	if (pid == 0) {
		sc(SYS_unshare, CLONE_NEWIPC, 0, 0, 0, 0, 0);
		/* The parent's queue must not exist in the new namespace. */
		code = sc(SYS_msgget, PROBE_KEY, 0, 0, 0, 0, 0);
		sc(SYS_exit_group, code == -ENOENT ? 0 : 1, 0, 0, 0, 0, 0);
	}
	report("sysv-new-ns-isolated", pid > 0 && wait_child() == 0);

	sc(SYS_msgctl, msgid, IPC_RMID, 0, 0, 0, 0);
	sc(SYS_semctl, semid, 0, IPC_RMID, 0, 0, 0);
	sc(SYS_shmctl, shmid, IPC_RMID, 0, 0, 0, 0);
	return 0;
}

/* POSIX names start with '/', but the raw syscall takes the name without
 * it: libc strips the prefix, and lookup_one_common() rejects '/' with
 * EACCES.
 */
#define PROBE_QUEUE "dsprobe"

static int test_mqueue(void)
{
	struct mq_attr attr = { 0, 4, 64, 0 };
	char msg[8] = "queue";
	/* mq_receive() requires a buffer of at least mq_msgsize. */
	char rcv[64];
	long fd, sent, got, pid, code;

	fd = sc(SYS_mq_open, (long)PROBE_QUEUE, O_RDWR | O_CREAT, 0644,
		(long)&attr, 0, 0);
	if (fd < 0) {
		out_pair("mqueue-open rc: ", fd);
		return 0;
	}
	report("mqueue-open", 1);

	sent = sc(SYS_mq_timedsend, fd, (long)msg, 8, 0, 0, 0);
	got = sc(SYS_mq_timedreceive, fd, (long)rcv, 64, 0, 0, 0);
	report("mqueue-roundtrip", sent == 0 && got == 8 && rcv[0] == 'q');
	if (sent != 0 || got != 8) {
		out_pair("mqueue-send rc: ", sent);
		out_pair("mqueue-recv rc: ", got);
	}

	pid = fork_child();
	if (pid == 0) {
		sc(SYS_unshare, CLONE_NEWIPC, 0, 0, 0, 0, 0);
		/* Queues live in the IPC namespace's mqueuefs, so a fresh
		 * namespace must not resolve the name. */
		code = sc(SYS_mq_open, (long)PROBE_QUEUE, O_RDWR, 0, 0, 0, 0);
		sc(SYS_exit_group, code == -ENOENT ? 0 : 1, 0, 0, 0, 0, 0);
	}
	report("mqueue-new-ns-isolated", pid > 0 && wait_child() == 0);
	return 0;
}

static int test_pidns(void)
{
	long pid, code;

	pid = fork_child();
	if (pid == 0) {
		/* CLONE_NEWPID applies to children, so the pid namespace
		 * init is the next fork. */
		if (sc(SYS_unshare, CLONE_NEWPID, 0, 0, 0, 0, 0) < 0) {
			out_pair("unshare-pid errno", 0);
			sc(SYS_exit_group, 1, 0, 0, 0, 0, 0);
		}
		if (fork_child() == 0) {
			long self = sc(SYS_getpid, 0, 0, 0, 0, 0, 0);
			long parent = sc(SYS_getppid, 0, 0, 0, 0, 0, 0);

			out_pair("init-ns-pid", self);
			out_pair("init-ns-ppid", parent);
			sc(SYS_exit_group, self == 1 && parent == 0 ? 0 : 1, 0, 0, 0, 0, 0);
		}
		code = wait_child();
		sc(SYS_exit_group, code, 0, 0, 0, 0, 0);
	}
	report("pidns-init-is-pid1", pid > 0 && wait_child() == 0);
	return 0;
}

void _start(void)
{
	out_pair("host-pid", sc(SYS_getpid, 0, 0, 0, 0, 0, 0));
	test_sysv();
	test_mqueue();
	test_pidns();
	sc(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}
