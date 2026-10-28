/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAS_BACKEND_H
#define FYAI_CAS_BACKEND_H

#include <stdint.h>
#include <sys/types.h>

#include "fyai_cas.h"

/*
 * The interface between the blob store (fyai_cas.c) and the file system
 * primitives of a platform. A backend lives in one file and exports one table.
 *
 *   Linux   fyai_cas_linux.c   copy_file_range, sendfile, FICLONE
 *   other   fyai_cas_none.c    no blob store
 */
#if defined(__linux__)
#define FYAI_CAS_BACKEND_LINUX 1
#else
#define FYAI_CAS_BACKEND_NONE 1
#endif

struct fyai_cas_backend {
	const char *name;
	/* Make the data of @fd durable. Required. */
	int (*sync_file)(int fd);
	/*
	 * Move at most @count bytes at the current offsets, in the kernel. Returns the
	 * count moved, or -1 with errno set. NULL: unsupported.
	 */
	ssize_t (*copy_range)(int source_fd, int target_fd, size_t count);
	/* As copy_range. NULL: the core reads and writes through a buffer. */
	ssize_t (*sendfile)(int source_fd, int target_fd, size_t count);
	/*
	 * Clone the whole file @source_fd of @size bytes into the empty file
	 * @target_fd. Returns 0 when cloned, 1 when the pair of file systems cannot
	 * clone, and -1 with errno set. NULL: always 1.
	 */
	int (*reflink)(int source_fd, int target_fd, uint64_t size);
};

#ifdef FYAI_CAS_BACKEND_LINUX
extern const struct fyai_cas_backend fyai_cas_backend_linux;
#endif
#ifdef FYAI_CAS_BACKEND_NONE
extern const struct fyai_cas_backend fyai_cas_backend_none;
#endif

#endif
