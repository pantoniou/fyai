/*
 * fyai_host_linux.c - host primitives of Linux
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_host_backend.h"

#ifdef FYAI_HOST_BACKEND_LINUX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <pty.h>

#include "fyai_transport.h"

static int linux_close_range(unsigned int from, unsigned int to)
{
#ifdef SYS_close_range
	return syscall(SYS_close_range, from, to, 0U) ? -1 : 0;
#else
	(void)from;
	(void)to;
	errno = ENOSYS;
	return -1;
#endif
}

/* Check whether Linux reports @pid blocked reading descriptor 0. */
static bool linux_process_reads_stdin(pid_t pid)
{
	char path[64];
	char buf[256];
	long long nr = -1;
	long long fd = -1;
	ssize_t n;
	int f;

	snprintf(path, sizeof(path), "/proc/%ld/syscall", (long)pid);
	f = open(path, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return false;
	n = read(f, buf, sizeof(buf) - 1);
	close(f);
	if (n <= 0)
		return false;
	buf[n] = 0;
	/* "running" for a process on a processor: it waits for nothing. */
	if (sscanf(buf, "%lld %llx", &nr, &fd) != 2)
		return false;
	return (nr == SYS_read || nr == SYS_readv) && fd == 0;
}

/* The children of @pid, into @out. Returns how many were read. */
static size_t linux_process_children(pid_t pid, pid_t *out, size_t max)
{
	char path[80];
	char buf[512];
	size_t count = 0;
	const char *p;
	ssize_t n;
	long v;
	int f;

	snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
		 (long)pid, (long)pid);
	f = open(path, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return 0;
	n = read(f, buf, sizeof(buf) - 1);
	close(f);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	for (p = buf; *p && count < max; ) {
		while (*p == ' ')
			p++;
		v = strtol(p, (char **)&p, 10);
		if (v <= 0)
			break;
		out[count++] = (pid_t)v;
	}
	return count;
}

static int linux_pipe(int fd[2])
{
	return pipe2(fd, O_CLOEXEC);
}

static int linux_seqpacket_pair(int fd[2])
{
	return socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fd);
}

static ssize_t linux_executable(char *path, size_t capacity)
{
	return readlink("/proc/self/exe", path, capacity);
}

static int linux_boot_id(char id[37])
{
	char text[37];
	ssize_t count;
	int fd, saved;

	fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		count = read(fd, text, sizeof(text));
	} while (count < 0 && errno == EINTR);
	saved = errno;
	close(fd);
	errno = saved;
	if (count != 37 || text[36] != '\n') {
		if (count >= 0)
			errno = EIO;
		return -1;
	}
	memcpy(id, text, 36);
	id[36] = '\0';
	return 0;
}

static int linux_exec_self(const char *const argv[])
{
	return execv("/proc/self/exe", (char *const *)argv);
}

/* The main thread stack is the [stack] line of /proc/self/maps. */
static bool linux_stack_bounds(uintptr_t *low, uintptr_t *high)
{
	char line[256];
	unsigned long lo, hi;
	bool found = false;
	FILE *fp;

	fp = fopen("/proc/self/maps", "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		if (!strstr(line, "[stack]"))
			continue;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
			*low = lo;
			*high = hi;
			found = true;
		}
		break;
	}
	fclose(fp);
	return found;
}

static int linux_pidfd_open(pid_t pid)
{
	int fd = syscall(SYS_pidfd_open, pid, 0);

	return fd < 0 ? -errno : fd;
}

/*
 * The PID that a pidfd names in the PID namespace of this process. A process in
 * a descendant namespace has a PID here, so the registrar can be in a namespace
 * of its own. Return 0, -ESRCH when the process has exited or is not visible
 * here, or -EIO when the descriptor says nothing about a process.
 */
static int linux_pidfd_pid(int pidfd, pid_t *pid)
{
	char path[64], line[256];
	FILE *fp;
	long value = 0;
	bool found = false;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", pidfd);
	fp = fopen(path, "re");
	if (!fp)
		return -errno;
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "Pid: %ld", &value) == 1) {
			found = true;
			break;
		}
	}
	fclose(fp);
	if (!found)
		return -EIO;
	if (value <= 0)
		return -ESRCH;
	*pid = (pid_t)value;
	return 0;
}

/* Return 0 when the process is alive, -ESRCH when it has exited. */
static int linux_pidfd_alive(int pidfd)
{
	int rc = syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0);

	return rc < 0 ? -errno : 0;
}

/* Return 1 if @pid is in the subtree @root, 0 if not, or a negative errno. */
static int linux_in_cgroup(const char *root, pid_t pid)
{
	char path[64], line[4096];
	size_t rlen = strlen(root);
	FILE *fp;
	int found = 0;
	size_t len;
	const char *p;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	fp = fopen(path, "re");
	if (!fp)
		return -errno;

	while (fgets(line, sizeof(line), fp)) {
		len = strlen(line);

		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		else
			continue;	/* an over-long line is not a match */
		if (strncmp(line, "0::", 3))
			continue;
		p = line + 3;
		found = !strncmp(p, root, rlen) &&
			(p[rlen] == '\0' || p[rlen] == '/');
		break;
	}
	fclose(fp);
	return found;
}

static const char *const ns_dirs[FYAI_NS_COUNT] = {
	[FYAI_NS_NET] = "net",
	[FYAI_NS_MNT] = "mnt",
	[FYAI_NS_PID] = "pid",
};

/* Store the inode of a namespace of @pid, or of this process for pid 0. */
static int linux_ns_ino(pid_t pid, int ns, uint64_t *ino)
{
	char path[64];
	struct stat st;

	if (pid)
		snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)pid,
			 ns_dirs[ns]);
	else
		snprintf(path, sizeof(path), "/proc/self/ns/%s", ns_dirs[ns]);
	if (stat(path, &st) < 0)
		return -errno;
	*ino = st.st_ino;
	return 0;
}

static int linux_channel_prepare(int channel)
{
	int on = 1;

	if (setsockopt(channel, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0)
		return -errno;
	return 0;
}

static int linux_harden(const char **step)
{
	struct rlimit nocore = { 0, 0 };

	if (prctl(PR_SET_DUMPABLE, 0)) {
		*step = "make the process undumpable";
		return -1;
	}
	if (setrlimit(RLIMIT_CORE, &nocore)) {
		*step = "disable core dumps";
		return -1;
	}
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		*step = "set no-new-privileges";
		return -1;
	}
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() == 1) {
		*step = NULL;
		errno = ESRCH;
		return -1;
	}
	return 0;
}

static int linux_memfd(const char *name, bool cloexec)
{
	return memfd_create(name, cloexec ? MFD_CLOEXEC : 0);
}

static int linux_openpty(int *master, int *slave)
{
	return openpty(master, slave, NULL, NULL, NULL);
}

static bool linux_mount_root(const char *path)
{
	struct statx sx;

	if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, 0, &sx) ||
	    !(sx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT))
		return true;
	return sx.stx_attributes & STATX_ATTR_MOUNT_ROOT;
}

static void linux_sort(void *base, size_t count, size_t size,
		       int (*compare)(const void *a, const void *b, void *arg), void *arg)
{
	qsort_r(base, count, size, compare, arg);
}

const struct fyai_host_backend fyai_host_backend_linux = {
	.name = "linux",
	.close_range = linux_close_range,
	.process_reads_stdin = linux_process_reads_stdin,
	.process_children = linux_process_children,
	.pipe = linux_pipe,
	.exec_self = linux_exec_self,
	.executable = linux_executable,
	.boot_id = linux_boot_id,
	.seqpacket_pair = linux_seqpacket_pair,
	.stack_bounds = linux_stack_bounds,
	.pidfd_open = linux_pidfd_open,
	.pidfd_pid = linux_pidfd_pid,
	.pidfd_alive = linux_pidfd_alive,
	.in_cgroup = linux_in_cgroup,
	.ns_ino = linux_ns_ino,
	.channel_prepare = linux_channel_prepare,
	.harden = linux_harden,
	.memfd = linux_memfd,
	.mount_root = linux_mount_root,
	.openpty = linux_openpty,
	.sort = linux_sort,
	.open_url = "xdg-open",
	.isolation = true,
};

#endif /* FYAI_HOST_BACKEND_LINUX */
