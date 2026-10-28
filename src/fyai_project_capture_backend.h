/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROJECT_CAPTURE_BACKEND_H
#define FYAI_PROJECT_CAPTURE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "fyai_cas.h"
#include "fyai_project_capture.h"

/*
 * The interface between the project capture (fyai_project_capture.c) and the
 * file system primitives of a platform. A backend lives in one file and exports
 * one table.
 *
 *   Linux   fyai_project_capture_linux.c   openat2, O_TMPFILE, OverlayFS xattrs
 *   other   fyai_project_capture_none.c    no capture
 */
#if defined(__linux__)
#define FYAI_PROJECT_CAPTURE_BACKEND_LINUX 1
#else
#define FYAI_PROJECT_CAPTURE_BACKEND_NONE 1
#endif

struct fyai_project_capture_backend {
	const char *name;
	/* The backend can capture metacopy files and an incremental delta (OverlayFS). */
	bool overlay;
	/*
	 * Open @path, relative to the directory @root, with no link followed and no
	 * device crossed. Returns a descriptor, or -1 with errno set.
	 */
	int (*open_beneath)(int root, const char *path, int flags, unsigned int mode);
	/* Make an anonymous file in @directory. Returns a descriptor, or -1. */
	int (*tmpfile)(int directory);
	/*
	 * Clone the open file @source to @path below @root, without a copy of the
	 * data. Returns the descriptor of the clone, -2 when the file system cannot
	 * clone and the caller copies, or -1 with errno set. NULL: always -2.
	 */
	int (*clone_file)(int source, int root, const char *path, uint64_t size);
	/* The extended attribute calls with the signature of Linux. */
	ssize_t (*flistxattr)(int fd, char *names, size_t size);
	ssize_t (*fgetxattr)(int fd, const char *name, void *value, size_t size);
	int (*fsetxattr)(int fd, const char *name, const void *value, size_t size, int flags);
	/* True for an attribute that is not project content. NULL: none is. */
	bool (*xattr_ignored)(const char *name);
	/* Set when the capture defers the barriers of the objects. NULL: it defers both. */
	void (*sync_policy)(struct fyai_cas_copy_state *copy,
			    const struct fyai_project_capture_opts *opts);
	/* The number of processors that the capture may use, at least 1. */
	int (*cpu_count)(void);
	/* Make the objects of the store durable. Returns 0, or -1 with errno set. */
	int (*sync_objects)(int fd);
};

#ifdef FYAI_PROJECT_CAPTURE_BACKEND_LINUX
extern const struct fyai_project_capture_backend fyai_project_capture_backend_linux;
#endif
#ifdef FYAI_PROJECT_CAPTURE_BACKEND_NONE
extern const struct fyai_project_capture_backend fyai_project_capture_backend_none;
#endif

#endif
