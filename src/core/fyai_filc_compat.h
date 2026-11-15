/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FILC_COMPAT_H
#define FYAI_FILC_COMPAT_H

/* The Fil-C libc has no struct statx and no statx(). */
#ifdef __FILC__

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/stat.h>

static inline int statx(int dirfd, const char *path, int flags,
			unsigned int mask, struct statx *sx)
{
	return (int)syscall(SYS_statx, dirfd, path, flags, mask, sx);
}

#endif

#endif
