/*
 * fyai_host.c - the platform-independent part of the host primitives
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

#include "fyai_host_backend.h"

#if defined(FYAI_HOST_BACKEND_LINUX)
#define backend (&fyai_host_backend_linux)
#elif defined(FYAI_HOST_BACKEND_MACOS)
#define backend (&fyai_host_backend_macos)
#else
#define backend (&fyai_host_backend_none)
#endif

int fyai_host_close_range(unsigned int from, unsigned int to)
{
	if (!backend->close_range) {
		errno = ENOSYS;
		return -1;
	}
	return backend->close_range(from, to);
}

bool fyai_host_process_reads_stdin(pid_t pid)
{
	return backend->process_reads_stdin && backend->process_reads_stdin(pid);
}

size_t fyai_host_process_children(pid_t pid, pid_t *out, size_t max)
{
	return backend->process_children ? backend->process_children(pid, out, max) : 0;
}

int fyai_host_pipe(int fd[2])
{
	int rc, saved;

	if (backend->pipe)
		return backend->pipe(fd);
	rc = pipe(fd);
	if (rc)
		return -1;
	rc = fcntl(fd[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(fd[1], F_SETFD, FD_CLOEXEC);
	if (!rc)
		return 0;
	saved = errno;
	close(fd[0]);
	close(fd[1]);
	errno = saved;
	return -1;
}

int fyai_host_seqpacket_pair(int fd[2])
{
	if (!backend->seqpacket_pair) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->seqpacket_pair(fd);
}

bool fyai_host_exec_self_available(void)
{
	return backend->exec_self != NULL;
}

int fyai_host_exec_self(const char *const argv[])
{
	if (!backend->exec_self) {
		errno = ENOSYS;
		return -1;
	}
	return backend->exec_self(argv);
}

ssize_t fyai_host_executable(char *path, size_t capacity)
{
	if (!backend->executable) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->executable(path, capacity);
}

int fyai_host_boot_id(char id[37])
{
	if (!backend->boot_id) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->boot_id(id);
}

bool fyai_host_stack_bounds(uintptr_t *low, uintptr_t *high)
{
	return backend->stack_bounds && backend->stack_bounds(low, high);
}

int fyai_host_pidfd_open(pid_t pid)
{
	return backend->pidfd_open ? backend->pidfd_open(pid) : -ENOSYS;
}

int fyai_host_pidfd_pid(int pidfd, pid_t *pid)
{
	return backend->pidfd_pid ? backend->pidfd_pid(pidfd, pid) : -ENOSYS;
}

int fyai_host_pidfd_alive(int pidfd)
{
	return backend->pidfd_alive ? backend->pidfd_alive(pidfd) : -ENOSYS;
}

int fyai_host_in_cgroup(const char *root, pid_t pid)
{
	return backend->in_cgroup ? backend->in_cgroup(root, pid) : -ENOSYS;
}

int fyai_host_ns_ino(pid_t pid, int ns, uint64_t *ino)
{
	return backend->ns_ino ? backend->ns_ino(pid, ns, ino) : -ENOSYS;
}

int fyai_host_channel_prepare(int channel)
{
	return backend->channel_prepare ? backend->channel_prepare(channel) : -ENOSYS;
}

bool fyai_host_isolation_available(void)
{
	return backend->isolation;
}

int fyai_host_harden(const char **step)
{
	if (!backend->harden) {
		*step = "harden the process";
		errno = ENOTSUP;
		return -1;
	}
	return backend->harden(step);
}

bool fyai_host_memfd_available(void)
{
	return backend->memfd != NULL;
}

int fyai_host_memfd(const char *name, bool cloexec)
{
	if (!backend->memfd) {
		errno = ENOSYS;
		return -1;
	}
	return backend->memfd(name, cloexec);
}

bool fyai_host_mount_root(const char *path)
{
	return backend->mount_root && backend->mount_root(path);
}

void fyai_host_random(void *buf, size_t size)
{
	struct timespec ts;
	uint64_t mix;
	unsigned char *p = buf;
	size_t i;

	if (backend->random) {
		backend->random(buf, size);
		return;
	}
	if (size <= 256 && !getentropy(buf, size))
		return;
	/* No entropy source: mix the clock and the process, enough for jitter. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	mix = (uint64_t)ts.tv_sec << 32 ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16);
	for (i = 0; i < size; i++) {
		mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
		p[i] = (unsigned char)(mix >> 56);
	}
}

/* The portable sort: qsort() with the comparator and argument in a static; one thread. */
static int (*sort_compare)(const void *, const void *, void *);
static void *sort_arg;

static int sort_thunk(const void *a, const void *b)
{
	return sort_compare(a, b, sort_arg);
}

void fyai_host_sort(void *base, size_t count, size_t size,
		    int (*compare)(const void *a, const void *b, void *arg), void *arg)
{
	if (backend->sort) {
		backend->sort(base, count, size, compare, arg);
		return;
	}
	sort_compare = compare;
	sort_arg = arg;
	qsort(base, count, size, sort_thunk);
}

int fyai_host_openpty(int *master, int *slave)
{
	if (!backend->openpty) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->openpty(master, slave);
}

const char *fyai_host_open_url_program(void)
{
	return backend->open_url ? backend->open_url : "xdg-open";
}
