/*
 * fyai_cas_linux.c - file system primitives of the blob store on Linux
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_cas_backend.h"

#ifdef FYAI_CAS_BACKEND_LINUX
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <linux/fs.h>

static int linux_sync_file(int fd)
{
	return fsync(fd);
}

static ssize_t linux_copy_range(int source_fd, int target_fd, size_t count)
{
	return copy_file_range(source_fd, NULL, target_fd, NULL, count, 0);
}

static ssize_t linux_sendfile(int source_fd, int target_fd, size_t count)
{
	return sendfile(target_fd, source_fd, NULL, count);
}

static int linux_reflink(int source_fd, int target_fd, uint64_t size)
{
	struct stat st;
	int rc;

	rc = fstat(source_fd, &st);
	if (rc)
		return -1;
	if ((uint64_t)st.st_size != size || lseek(source_fd, 0, SEEK_CUR) != 0 ||
	    lseek(target_fd, 0, SEEK_CUR) != 0)
		return 1;
	do {
		rc = ioctl(target_fd, FICLONE, source_fd);
	} while (rc && errno == EINTR);
	if (rc)
		return (errno == EXDEV || errno == EOPNOTSUPP || errno == ENOSYS ||
		    errno == EINVAL || errno == ENOTTY) ? 1 : -1;
	rc = lseek(source_fd, size, SEEK_SET) < 0 || lseek(target_fd, size, SEEK_SET) < 0;
	return rc ? -1 : 0;
}

const struct fyai_cas_backend fyai_cas_backend_linux = {
	.name = "linux",
	.sync_file = linux_sync_file,
	.copy_range = linux_copy_range,
	.sendfile = linux_sendfile,
	.reflink = linux_reflink,
};

#endif /* FYAI_CAS_BACKEND_LINUX */
