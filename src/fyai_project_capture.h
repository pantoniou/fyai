/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROJECT_CAPTURE_H
#define FYAI_PROJECT_CAPTURE_H

#include <sys/types.h>
#include "fyai_project.h"

struct fyai_project_capture_opts {
	bool incremental; /* Source is a frozen merged overlay with an immutable lower. */
	int upper_fd; /* Required only for incremental capture. */
	fy_generic snapshot; /* Borrowed baseline manifest; required for incremental capture. */
	int source_fd;
	int objects_fd;
	int baseline_fd; /* -1 records manifests without materializing files. */
	bool verify; /* Independently verify worker output on the caller. */
	unsigned int workers; /* 0 uses the current CPU affinity. */
	bool mapped_owner;
	uid_t host_uid;
	gid_t host_gid;
};

/* Capture a bounded tree into gb. Descriptors are borrowed; failure sets errno. */
fy_generic fyai_project_capture(struct fy_generic_builder *gb,
		const struct fyai_project_capture_opts *opts,
		char *error_path, size_t error_size);

#endif
