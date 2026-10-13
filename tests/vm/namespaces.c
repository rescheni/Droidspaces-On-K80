// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
#include <unistd.h>

#ifndef DS_TEST_PIDFD_NS_IOCTL
#define DS_TEST_PIDFD_NS_IOCTL 1
#endif

#define CHECK(expr) do { if (!(expr)) { \
	dprintf(2, "FAIL line %d: %s (errno=%d)\n", __LINE__, #expr, errno); \
	_exit(1); } } while (0)

static void reap(pid_t child)
{
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void leave_orphan(void)
{
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		for (;;)
			pause();
	}
}

static void container_init(void)
{
	char self[32];
	pid_t child;
	ssize_t len;

	CHECK(getpid() == 1);
	CHECK(getppid() == 0);
	CHECK(unshare(CLONE_NEWNS) == 0);
	CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
	CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
	len = readlink("/proc/self", self, sizeof(self) - 1);
	CHECK(len > 0);
	self[len] = 0;
	CHECK(strcmp(self, "1") == 0);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(getpid() == 2);
		CHECK(unshare(CLONE_NEWPID | CLONE_NEWIPC) == 0);
		pid_t nested = fork();
		CHECK(nested >= 0);
		if (!nested) {
			CHECK(getpid() == 1);
			CHECK(getppid() == 0);
			leave_orphan();
			_exit(0);
		}
		reap(nested);
		_exit(0);
	}
	reap(child);
	leave_orphan();
	_exit(0);
}

static int clone_init(void *unused)
{
	(void)unused;
	container_init();
	return 1;
}

static int reboot_init(void *command)
{
	CHECK(getpid() == 1);
	CHECK(syscall(SYS_reboot, 0, 0, 0, NULL) == -1 && errno == EINVAL);
	CHECK(syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_CAD_ON, NULL) == -1 && errno == EINVAL);
	leave_orphan();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		(unsigned long)command, NULL);
	_exit(1);
}

int main(void)
{
	struct stat initial, isolated, restored;
	int ipc_fd = open("/proc/self/ns/ipc", O_RDONLY | O_CLOEXEC);
	int pid_fd = open("/proc/self/ns/pid", O_RDONLY | O_CLOEXEC);
	CHECK(ipc_fd >= 0 && pid_fd >= 0);
	CHECK(fstat(ipc_fd, &initial) == 0);
	for (int i = 0; i < 8; i++) {
		CHECK(unshare(CLONE_NEWIPC | CLONE_NEWPID) == 0);
		CHECK(stat("/proc/self/ns/ipc", &isolated) == 0);
		CHECK(isolated.st_ino != initial.st_ino);
		pid_t child = fork();
		CHECK(child >= 0);
		if (!child)
			container_init();
		reap(child);
		CHECK(setns(ipc_fd, CLONE_NEWIPC) == 0);
		CHECK(setns(pid_fd, CLONE_NEWPID) == 0);
		CHECK(stat("/proc/self/ns/ipc", &restored) == 0);
		CHECK(restored.st_ino == initial.st_ino);
	}
	void *stack = malloc(65536);
	CHECK(stack != NULL);
	pid_t child = clone(clone_init, (char *)stack + 65536,
			    CLONE_NEWPID | CLONE_NEWIPC | SIGCHLD, NULL);
	CHECK(child >= 0);
	reap(child);
#if DS_TEST_PIDFD_NS_IOCTL
	/* pidfd ioctl obtains real nsfs references, including the host IPC fallback. */
	int self_pidfd = syscall(SYS_pidfd_open, getpid(), 0);
	CHECK(self_pidfd >= 0);
	for (unsigned nr = 2; nr <= 6; nr++) {
		if (nr == 3 || nr == 4) continue;
		int nsfd = ioctl(self_pidfd, _IO(0xff, nr), 0);
		CHECK(nsfd >= 0);
		CHECK(fstat(nsfd, &restored) == 0);
		CHECK(close(nsfd) == 0);
	}
	CHECK(close(self_pidfd) == 0);
#else
	puts("DS_NAMESPACES_SKIP: pidfd namespace ioctl is not provided by this baseline");
#endif
	unsigned commands[] = { LINUX_REBOOT_CMD_RESTART, LINUX_REBOOT_CMD_RESTART2,
		LINUX_REBOOT_CMD_HALT, LINUX_REBOOT_CMD_POWER_OFF };
	for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		child = clone(reboot_init, (char *)stack + 65536, CLONE_NEWPID | SIGCHLD,
			(void *)(unsigned long)commands[i]);
		CHECK(child >= 0);
		int status;
		CHECK(waitpid(child, &status, 0) == child);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == (i < 2 ? SIGHUP : SIGINT));
	}
	free(stack);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(setuid(65534) == 0);
		errno = 0;
		CHECK(setns(ipc_fd, CLONE_NEWIPC) == -1 && errno == EPERM);
		errno = 0;
		CHECK(unshare(CLONE_NEWPID) == -1 && errno == EPERM);
		_exit(0);
	}
	reap(child);
	CHECK(close(ipc_fd) == 0);
	CHECK(close(pid_fd) == 0);
	puts("DS_NAMESPACES_PASS: PID 1, nested PID, procfs, orphan reaping, IPC identity, setns, clone, permissions");
	return 0;
}
