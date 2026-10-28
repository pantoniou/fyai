/*
 * fyai_project_capture_macos.c - file system primitives of the capture on macOS
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_project_capture_backend.h"

#ifdef FYAI_PROJECT_CAPTURE_BACKEND_MACOS
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/xattr.h>

/* Each component is opened without following links or crossing devices. */
static int mac_open_beneath(int root, const char *path, int flags, unsigned int mode)
{
	struct stat base, found;
	char *copy, *part, *next;
	int current, opened = -1, saved;

	if (!path || path[0] == '/') {
		errno = EINVAL;
		return -1;
	}
	if (fstat(root, &base))
		return -1;
	copy = strdup(*path ? path : ".");
	if (!copy)
		return -1;
	current = fcntl(root, F_DUPFD_CLOEXEC, 3);
	if (current < 0)
		goto out;
	for (part = copy; part; part = next) {
		next = strchr(part, '/');
		if (next)
			*next++ = '\0';
		if (!*part || !strcmp(part, "..")) {
			errno = EINVAL;
			goto out;
		}
		opened = openat(current, part, (next ? O_RDONLY | O_DIRECTORY : flags) |
				O_NOFOLLOW | O_CLOEXEC, next ? 0 : mode);
		if (opened < 0)
			goto out;
		if (fstat(opened, &found))
			goto fail_open;
		if (found.st_dev != base.st_dev) {
			errno = EXDEV;
			goto fail_open;
		}
		close(current);
		current = opened;
		opened = -1;
	}
	free(copy);
	return current;
fail_open:
	saved = errno;
	close(opened);
	errno = saved;
out:
	saved = errno;
	if (current >= 0)
		close(current);
	free(copy);
	errno = saved;
	return -1;
}

static int mac_tmpfile(int directory)
{
	char name[64];
	unsigned long long random;
	unsigned int attempt;
	int fd, saved;

	for (attempt = 0; attempt < 16; attempt++) {
		arc4random_buf(&random, sizeof(random));
		snprintf(name, sizeof(name), ".capture-%016llx", random);
		fd = openat(directory, name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			return -1;
		}
		if (!unlinkat(directory, name, 0))
			return fd;
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	errno = EEXIST;
	return -1;
}

/* -2 selects independent copying when the filesystem cannot clone. */
static int mac_clone_file(int source, int root, const char *path, uint64_t size)
{
	struct stat st;
	char *copy, *slash, *name;
	int parent, target, rc, saved;

	copy = strdup(path);
	if (!copy)
		return -1;
	slash = strrchr(copy, '/');
	name = slash ? slash + 1 : copy;
	if (slash)
		*slash = '\0';
	parent = mac_open_beneath(root, slash ? copy : "", O_RDONLY | O_DIRECTORY, 0);
	if (parent < 0) {
		free(copy);
		return -1;
	}
	rc = fclonefileat(source, parent, name, CLONE_NOOWNERCOPY);
	saved = errno;
	if (!rc) {
		target = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		saved = errno;
		if (target >= 0) {
			rc = fstat(target, &st);
			if (rc || (uint64_t)st.st_size != size) {
				saved = rc ? errno : EAGAIN;
				close(target);
				unlinkat(parent, name, 0);
				target = -1;
			}
		}
	} else
		target = saved == EXDEV || saved == ENOTSUP || saved == ENOSYS ? -2 : -1;
	close(parent);
	free(copy);
	errno = saved;
	return target;
}

static ssize_t mac_flistxattr(int fd, char *names, size_t size)
{
	return flistxattr(fd, names, size, 0);
}

static ssize_t mac_fgetxattr(int fd, const char *name, void *value, size_t size)
{
	return fgetxattr(fd, name, value, size, 0, 0);
}

static int mac_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
	return fsetxattr(fd, name, value, size, 0, flags);
}

static bool mac_xattr_ignored(const char *name)
{
	return !strncmp(name, "com.apple.", 10);
}

/* There is no syncfs: each object is published with its file and parent barriers. */
static void mac_sync_policy(struct fyai_cas_copy_state *copy,
			    const struct fyai_project_capture_opts *opts)
{
	copy->defer_file_sync = opts && opts->defer_sync;
	copy->defer_directory_sync = opts && opts->defer_sync;
}

static int mac_cpu_count(void)
{
	return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

static int mac_sync_objects(int fd)
{
	return fsync(fd);
}

const struct fyai_project_capture_backend fyai_project_capture_backend_macos = {
	.name = "macos",
	.open_beneath = mac_open_beneath,
	.tmpfile = mac_tmpfile,
	.clone_file = mac_clone_file,
	.flistxattr = mac_flistxattr,
	.fgetxattr = mac_fgetxattr,
	.fsetxattr = mac_fsetxattr,
	.xattr_ignored = mac_xattr_ignored,
	.sync_policy = mac_sync_policy,
	.cpu_count = mac_cpu_count,
	.sync_objects = mac_sync_objects,
};

#endif /* FYAI_PROJECT_CAPTURE_BACKEND_MACOS */
