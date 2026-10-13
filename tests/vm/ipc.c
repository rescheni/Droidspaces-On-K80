// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "IPC FAIL %d: %s errno=%d\n", __LINE__, #x, errno); _exit(1); } } while (0)
struct message { long type; char text[6000]; };
static int semaphore;
static int proc_has_id(const char *path, int id)
{
	FILE *file = fopen(path, "r");
	char line[512];
	int found = 0, row_id;
	CHECK(file != NULL);
	while (fgets(line, sizeof(line), file))
		if (sscanf(line, "%*d %d", &row_id) == 1 && row_id == id)
			found = 1;
	CHECK(fclose(file) == 0);
	return found;
}
static void write_limit(const char *path, const char *value)
{
	FILE *file = fopen(path, "w");
	CHECK(file && fputs(value, file) >= 0 && fclose(file) == 0);
}
static void reap(pid_t child)
{
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void undo_decrement(void)
{
	struct sembuf op = { 0, -1, SEM_UNDO };
	CHECK(semop(semaphore, &op, 1) == 0);
}
static int shared_undo(void *arg)
{
	(void)arg;
	undo_decrement();
	return 0;
}

int main(void)
{
	const key_t key = 0x44535043;
	struct message msg = { .type = 7 }, received;
	struct msqid_ds msgstat;
	struct shmid_ds shmstat;
	int queue, segment;
	char *memory;
	pid_t child;
	CHECK(unshare(CLONE_NEWIPC) == 0);
#if DS_TEST_SHM_HUGETLB_REJECT
	CHECK(shmget(IPC_PRIVATE, 2 * 1024 * 1024,
		     IPC_CREAT | 0600 | SHM_HUGETLB) == -1 && errno == EOPNOTSUPP);
#endif
	int own_ns = open("/proc/self/ns/ipc", O_RDONLY | O_CLOEXEC);
	CHECK(own_ns >= 0);
	queue = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
	semaphore = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
	segment = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
	CHECK(queue >= 0 && semaphore >= 0 && segment >= 0);
	CHECK(proc_has_id("/proc/sysvipc/msg", queue));
	CHECK(proc_has_id("/proc/sysvipc/sem", semaphore));
	CHECK(proc_has_id("/proc/sysvipc/shm", segment));
	CHECK(semctl(semaphore, 0, SETVAL, 5) == 0);
	memory = shmat(segment, NULL, 0);
	CHECK(memory != (void *)-1);
	strcpy(memory, "parent memory");
	memset(msg.text, 'X', sizeof(msg.text));
	CHECK(msgsnd(queue, &msg, sizeof(msg.text), 0) == 0);
	CHECK(msgctl(queue, IPC_STAT, &msgstat) == 0 && msgstat.msg_qnum == 1);
	CHECK(msgrcv(queue, &received, 10, 7, IPC_NOWAIT) == -1 && errno == E2BIG);
	CHECK(msgrcv(queue, &received, sizeof(received.text), 7, 0) == sizeof(msg.text));
	CHECK(received.type == 7 && !memcmp(msg.text, received.text, sizeof(msg.text)));
	CHECK(msgrcv(queue, &received, sizeof(received.text), 0, IPC_NOWAIT) == -1 && errno == ENOMSG);
	CHECK(msgsnd(queue, &msg, 16, 0) == 0);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(!strcmp(memory, "parent memory"));
		CHECK(unshare(CLONE_NEWIPC) == 0);
		CHECK(msgget(key, 0) == -1 && errno == ENOENT);
		CHECK(semget(key, 1, 0) == -1 && errno == ENOENT);
		CHECK(shmget(key, 4096, 0) == -1 && errno == ENOENT);
		int q = msgget(key, IPC_CREAT | 0600);
		int s = semget(key, 1, IPC_CREAT | 0600);
		int h = shmget(key, 4096, IPC_CREAT | 0600);
		CHECK(q >= 0 && s >= 0 && h >= 0);
		CHECK(semctl(s, 0, SETVAL, 9) == 0);
		char *m = shmat(h, NULL, 0);
		CHECK(m != (void *)-1 && m[0] == 0);
		strcpy(m, "isolated memory");
		CHECK(shmdt(m) == 0);
		/* Leave resources allocated: namespace destruction must remove them. */
		CHECK(setns(own_ns, CLONE_NEWIPC) == 0);
		CHECK(msgget(key, 0) == queue);
		CHECK(msgrcv(queue, &received, sizeof(received.text), 0, 0) == 16);
		_exit(0);
	}
	reap(child);
	CHECK(!strcmp(memory, "parent memory"));
	CHECK(semctl(semaphore, 0, GETVAL) == 5);
	/* A sender exceeding the queue capacity must unblock as the reader drains. */
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		for (int i = 0; i < 16; i++) {
			msg.text[0] = (char)i;
			CHECK(msgsnd(queue, &msg, sizeof(msg.text), 0) == 0);
		}
		_exit(0);
	}
	for (int i = 0; i < 16; i++) {
		CHECK(msgrcv(queue, &received, sizeof(received.text), 7, 0) == sizeof(msg.text));
		CHECK(received.text[0] == (char)i);
	}
	reap(child);
	child = fork();
	CHECK(child >= 0);
	if (!child) { undo_decrement(); _exit(0); }
	reap(child);
	CHECK(semctl(semaphore, 0, GETVAL) == 5);
	undo_decrement();
	child = fork();
	CHECK(child >= 0);
	if (!child) _exit(0);
	reap(child);
	CHECK(semctl(semaphore, 0, GETVAL) == 4);
	CHECK(unshare(CLONE_SYSVSEM) == 0);
	CHECK(semctl(semaphore, 0, GETVAL) == 5);
	char *stack = malloc(65536);
	CHECK(stack != NULL);
	child = clone(shared_undo, stack + 65536, CLONE_SYSVSEM | SIGCHLD, NULL);
	CHECK(child >= 0);
	reap(child);
	CHECK(semctl(semaphore, 0, GETVAL) == 4);
	CHECK(clone(shared_undo, stack + 65536, CLONE_SYSVSEM | CLONE_NEWIPC | SIGCHLD, NULL) == -1 && errno == EINVAL);
	CHECK(unshare(CLONE_SYSVSEM) == 0);
	CHECK(semctl(semaphore, 0, GETVAL) == 5);
	free(stack);
	/* Unsharing IPC must apply the old namespace's pending undo adjustments. */
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		undo_decrement();
		CHECK(unshare(CLONE_NEWIPC) == 0);
		CHECK(setns(own_ns, CLONE_NEWIPC) == 0);
		CHECK(semctl(semaphore, 0, GETVAL) == 5);
		_exit(0);
	}
	reap(child);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(shmget(key + 1, 4096, IPC_CREAT | 0600) >= 0);
		_exit(0);
	}
	reap(child);
	int orphan = shmget(key + 1, 4096, 0);
	CHECK(orphan >= 0);
	write_limit("/proc/sys/kernel/shm_rmid_forced", "1\n");
	CHECK(shmctl(orphan, IPC_STAT, &shmstat) == -1 && errno == EINVAL);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(setuid(65534) == 0);
		CHECK(msgsnd(queue, &msg, 1, IPC_NOWAIT) == -1 && errno == EACCES);
		CHECK(semctl(semaphore, 0, GETVAL) == -1 && errno == EACCES);
		CHECK(shmat(segment, NULL, 0) == (void *)-1 && errno == EACCES);
		_exit(0);
	}
	reap(child);
	CHECK(shmctl(segment, IPC_RMID, NULL) == 0);
	CHECK(!strcmp(memory, "parent memory"));
	CHECK(shmdt(memory) == 0);
	CHECK(shmctl(segment, IPC_STAT, &shmstat) == -1);
	struct sembuf blocked_op = { 0, -6, 0 };
	struct timespec immediate = { 0 };
	CHECK(semtimedop(semaphore, &blocked_op, 1, &immediate) == -1 && errno == EAGAIN);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(semop(semaphore, &blocked_op, 1) == -1 && errno == EIDRM);
		_exit(0);
	}
	int waiting = 0;
	for (int i = 0; i < 200; i++) {
		if (semctl(semaphore, 0, GETNCNT) == 1) { waiting = 1; break; }
		usleep(10000);
	}
	CHECK(waiting && semctl(semaphore, 0, IPC_RMID) == 0);
	reap(child);
	int ready[2];
	CHECK(pipe(ready) == 0);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(write(ready[1], "r", 1) == 1);
		CHECK(msgrcv(queue, &received, sizeof(received.text), 0, 0) == -1 && errno == EIDRM);
		_exit(0);
	}
	char mark;
	CHECK(read(ready[0], &mark, 1) == 1);
	char path[80], line[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", child);
	waiting = 0;
	for (int i = 0; i < 200; i++) {
		FILE *file = fopen(path, "r");
		CHECK(file && fgets(line, sizeof(line), file) && fclose(file) == 0);
		char *state = strrchr(line, ')');
		if (state && state[1] == ' ' && state[2] == 'S') { waiting = 1; break; }
		usleep(10000);
	}
	CHECK(waiting && msgctl(queue, IPC_RMID, NULL) == 0);
	reap(child);
	CHECK(close(ready[0]) == 0 && close(ready[1]) == 0);
	CHECK(!proc_has_id("/proc/sysvipc/msg", queue));
	CHECK(!proc_has_id("/proc/sysvipc/sem", semaphore));
	CHECK(!proc_has_id("/proc/sysvipc/shm", segment));
	CHECK(close(own_ns) == 0);
	puts("DS_SYSVIPC_PASS: messages, semaphores, shared memory, isolation, SEM_UNDO, clone sharing, permissions, blocked removal, procfs, forced cleanup");
	return 0;
}
