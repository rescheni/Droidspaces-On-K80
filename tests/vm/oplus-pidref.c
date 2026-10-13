// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int start_pipe[2], ready_pipe[2], stop_pipe[2];
static atomic_int stop_worker;
static void *worker(void *unused)
{
	(void)unused;
	while (!atomic_load(&stop_worker)) usleep(1000);
	return NULL;
}
static int child(void *unused)
{
	pthread_t thread;
	char byte;
	(void)unused;
	if (read(start_pipe[0], &byte, 1) != 1 || pthread_create(&thread, NULL, worker, NULL)) return 2;
	if (write(ready_pipe[1], "r", 1) != 1 || read(stop_pipe[0], &byte, 1) != 1) return 3;
	atomic_store(&stop_worker, 1);
	return pthread_join(thread, NULL) ? 4 : 0;
}
static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t size;
	if (fd < 0) return -1;
	size = write(fd, text, strlen(text));
	close(fd);
	return size == (ssize_t)strlen(text) ? 0 : -1;
}
static long counter(const char *module, const char *name)
{
	char path[256];
	long value = -1;
	FILE *file;
	snprintf(path, sizeof(path), "/sys/module/%s/parameters/%s", module, name);
	file = fopen(path, "r");
	if (file) { if (fscanf(file, "%ld", &value) != 1) value = -1; fclose(file); }
	return value;
}
static int exercise_one(void)
{
	char path[256], text[32], byte;
	char *stack = malloc(1024*1024);
	DIR *dir;
	struct dirent *entry;
	int pid = -1, tid = -1, status, result = -1;
	if (!stack || pipe(start_pipe) || pipe(ready_pipe) || pipe(stop_pipe)) return -1;
	atomic_store(&stop_worker, 0);
	pid = clone(child, stack+1024*1024, CLONE_NEWUSER|CLONE_NEWPID|SIGCHLD, NULL);
	if (pid < 0) goto out;
	snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
	if (write_text(path, "0 0 1\n")) goto out;
	snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
	if (write_text(path, "deny\n")) goto out;
	snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
	if (write_text(path, "0 0 1\n")) goto out;
	if (write(start_pipe[1], "s", 1) != 1 || read(ready_pipe[0], &byte, 1) != 1) goto out;
	snprintf(path, sizeof(path), "/proc/%d/task", pid);
	dir = opendir(path);
	if (!dir) goto out;
	while ((entry = readdir(dir))) {
		int candidate = atoi(entry->d_name);
		if (candidate > 0 && candidate != pid) { tid = candidate; break; }
	}
	closedir(dir);
	if (tid < 0) goto out;
	snprintf(text, sizeof(text), "%d\n", tid);
	for (int i = 0; i < 8; i++)
		if (write_text("/sys/module/oplus_sys_stability_helper/parameters/exercise", text)) goto out;
	if (write(stop_pipe[1], "s", 1) != 1 || waitpid(pid, &status, 0) != pid) goto out;
	pid = -1;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) result = 0;
out:
	if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
	for (int i = 0; i < 2; i++) { close(start_pipe[i]); close(ready_pipe[i]); close(stop_pipe[i]); }
	free(stack);
	return result;
}
int main(int argc, char **argv)
{
	int negative = argc == 2 && !strcmp(argv[1], "negative");
	long pid_before = counter("droidspaces_ns", "pid_live");
	long user_before = counter("droidspaces_ns", "userns_live");
	long balanced_before = counter("droidspaces_compat", "balanced");
	int rounds = negative ? 1 : 8;
	alarm(60);
	if (pid_before < 0 || user_before < 0 || (!negative && balanced_before < 0)) return 1;
	for (int i = 0; i < rounds; i++) if (exercise_one()) { perror("exercise"); return 1; }
	if (negative) {
		sleep(3);
		if (counter("droidspaces_ns", "pid_live") != pid_before+1 ||
		    counter("droidspaces_ns", "userns_live") != user_before+1) return 1;
		puts("DS_OPLUS_BASELINE_LEAK_PASS");
		return 0;
	}
	for (int i = 0; i < 300; i++) {
		if (counter("droidspaces_ns", "pid_live") == pid_before &&
		    counter("droidspaces_ns", "userns_live") == user_before) break;
		usleep(100000);
	}
	if (counter("droidspaces_ns", "pid_live") != pid_before ||
	    counter("droidspaces_ns", "userns_live") != user_before ||
	    counter("droidspaces_compat", "balanced") != balanced_before+8*rounds) return 1;
	puts("DS_OPLUS_PIDREF_PASS");
	return 0;
}
