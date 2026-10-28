/*
 * fyai_project_capture_linux.c - file system primitives of the capture on Linux
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_project_capture_backend.h"

#ifdef FYAI_PROJECT_CAPTURE_BACKEND_LINUX
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <linux/openat2.h>

static int linux_open_beneath(int root, const char *path, int flags, unsigned int mode)
{
	struct open_how how = { .flags = flags | O_NOFOLLOW | O_CLOEXEC,
				.mode = mode,
				.resolve =
					RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV };

	return syscall(SYS_openat2, root, *path ? path : ".", &how, sizeof(how));
}


static int linux_tmpfile(int directory)
{
	return openat(directory, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
}

static ssize_t linux_flistxattr(int fd, char *names, size_t size)
{
	return flistxattr(fd, names, size);
}

static ssize_t linux_fgetxattr(int fd, const char *name, void *value, size_t size)
{
	return fgetxattr(fd, name, value, size);
}

static int linux_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
	return fsetxattr(fd, name, value, size, flags);
}

static int linux_cpu_count(void)
{
	cpu_set_t affinity;

	return sched_getaffinity(0, sizeof(affinity), &affinity) ? 1 : CPU_COUNT(&affinity);
}

static int linux_sync_objects(int fd)
{
	return syncfs(fd);
}

const struct fyai_project_capture_backend fyai_project_capture_backend_linux = {
	.name = "linux",
	.overlay = true,
	.open_beneath = linux_open_beneath,
	.tmpfile = linux_tmpfile,
	.flistxattr = linux_flistxattr,
	.fgetxattr = linux_fgetxattr,
	.fsetxattr = linux_fsetxattr,
	.cpu_count = linux_cpu_count,
	.sync_objects = linux_sync_objects,
};

#endif /* FYAI_PROJECT_CAPTURE_BACKEND_LINUX */
