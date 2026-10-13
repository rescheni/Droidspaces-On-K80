// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2, "MQ FAIL %d: %s errno=%d\n", __LINE__, #x, errno); _exit(1); } } while (0)
static long read_limit(const char *path)
{
	FILE *file = fopen(path, "r");
	long value;
	CHECK(file && fscanf(file, "%ld", &value) == 1 && fclose(file) == 0);
	return value;
}
int main(void)
{
	const char *mountpoint = getenv("DS_TEST_MQUEUE_PATH");
	char queue_path[4096];
	if (!mountpoint) mountpoint = "/mqueue";
	CHECK(snprintf(queue_path, sizeof(queue_path), "%s/ds-test", mountpoint) < (int)sizeof(queue_path));
	char buf[128];
	unsigned priority;
	struct mq_attr attr = { .mq_maxmsg = 4, .mq_msgsize = sizeof(buf) }, actual;
	struct stat st;
	CHECK(unshare(CLONE_NEWIPC | CLONE_NEWNS) == 0);
	CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
	CHECK(mkdir(mountpoint, 0700) == 0);
	CHECK(mount("mqueue", mountpoint, "mqueue", 0, NULL) == 0);
	long parent_limit = read_limit("/proc/sys/fs/mqueue/msg_max");
	CHECK(parent_limit > 0);
	CHECK(read_limit("/proc/sys/kernel/msgmax") >= 6000);
	mqd_t mq = mq_open("/ds-test", O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);
	CHECK(mq != (mqd_t)-1);
	CHECK(stat(queue_path, &st) == 0);
	CHECK(mq_send(mq, "low", 4, 1) == 0);
	CHECK(mq_send(mq, "high", 5, 7) == 0);
	CHECK(mq_getattr(mq, &actual) == 0 && actual.mq_curmsgs == 2);
	CHECK(mq_receive(mq, buf, sizeof(buf), &priority) == 5 && priority == 7 && !strcmp(buf, "high"));
	CHECK(mq_receive(mq, buf, sizeof(buf), &priority) == 4 && priority == 1 && !strcmp(buf, "low"));
	CHECK(mq_receive(mq, buf, sizeof(buf), &priority) == -1 && errno == EAGAIN);
	attr.mq_flags = 0;
	CHECK(mq_setattr(mq, &attr, NULL) == 0);
	struct timespec now;
	CHECK(clock_gettime(CLOCK_REALTIME, &now) == 0);
	CHECK(mq_timedreceive(mq, buf, sizeof(buf), NULL, &now) == -1 && errno == ETIMEDOUT);
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGUSR1);
	CHECK(sigprocmask(SIG_BLOCK, &signals, NULL) == 0);
	struct sigevent notify = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1, .sigev_value.sival_int = 42 };
	CHECK(mq_notify(mq, &notify) == 0);
	CHECK(mq_send(mq, "notify", 7, 1) == 0);
	siginfo_t info;
	struct timespec timeout = { .tv_sec = 2 };
	CHECK(sigtimedwait(&signals, &info, &timeout) == SIGUSR1 && info.si_value.sival_int == 42);
	CHECK(mq_receive(mq, buf, sizeof(buf), NULL) == 7);
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(unshare(CLONE_NEWIPC) == 0);
		CHECK(mq_open("/ds-test", O_RDONLY) == (mqd_t)-1 && errno == ENOENT);
		FILE *file = fopen("/proc/sys/fs/mqueue/msg_max", "w");
		CHECK(file && fprintf(file, "6\n") > 0 && fclose(file) == 0);
		CHECK(read_limit("/proc/sys/fs/mqueue/msg_max") == 6);
		mqd_t own = mq_open("/ds-test", O_CREAT | O_RDWR, 0600, &attr);
		CHECK(own != (mqd_t)-1 && mq_close(own) == 0);
		/* Keep the named queue: namespace teardown must dispose of it. */
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(read_limit("/proc/sys/fs/mqueue/msg_max") == parent_limit);
	CHECK(mq_unlink("/ds-test") == 0);
	CHECK(stat(queue_path, &st) == -1 && errno == ENOENT);
	CHECK(mq_send(mq, "open fd", 8, 0) == 0);
	CHECK(mq_receive(mq, buf, sizeof(buf), NULL) == 8);
	CHECK(mq_close(mq) == 0);
	CHECK(umount(mountpoint) == 0);
	CHECK(rmdir(mountpoint) == 0);
	puts("DS_MQUEUE_PASS: mount, priority, timeout, notification, namespace isolation, sysctl, unlink lifetime");
	return 0;
}
