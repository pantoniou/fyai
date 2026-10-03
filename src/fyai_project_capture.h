/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROJECT_CAPTURE_H
#define FYAI_PROJECT_CAPTURE_H

#include <sys/types.h>
#include "fyai_project.h"

enum fyai_project_capture_phase {
	FYAI_PROJECT_SCAN,
	FYAI_PROJECT_INGEST,
	FYAI_PROJECT_MATERIALIZE,
	FYAI_PROJECT_VERIFY,
	FYAI_PROJECT_MANIFEST,
	FYAI_PROJECT_DONE,
};

struct fyai_project_capture_stats {
	enum fyai_project_capture_phase phase;
	uint64_t files, directories, symlinks, completed;
	uint64_t logical_bytes, copied_bytes, reflinked_bytes, borrowed_bytes;
	uint64_t copies, reflinks, hardlinks, metacopies;
	uint64_t borrowed_files;
	uint64_t elapsed_ms;
	unsigned int workers;
	int copy_backend;
};

/* Called only on the capture caller; worker counters are sampled atomically. */
typedef void (*fyai_project_capture_progress_fn)(void *arg,
						 const struct fyai_project_capture_stats *stats);

struct fyai_project_capture_opts {
	/* Optional caller-owned result. */
	struct fyai_project_capture_stats *stats;
	fyai_project_capture_progress_fn progress;
	void *progress_arg;
	/* Caller chooses whether to persist the completed capture. */
	bool defer_sync;
	/* Borrow immutable Git object files by host contract. */
	bool borrow_git;
	bool reuse_baseline;
	int previous_baseline_fd; /* Used only when reuse_baseline is set. */
	/* Materialize metadata-only files redirected into data_fd. */
	bool metacopy;
	/* Private snapshot data-only directory; required for metacopy. */
	int data_fd;
	/* Source is a frozen merged overlay with an immutable lower. */
	bool incremental;
	int upper_fd; /* Required only for incremental capture. */
	/* Borrowed baseline manifest; required for incremental capture. */
	fy_generic snapshot;
	int source_fd;
	int objects_fd;
	int baseline_fd; /* -1 records manifests without materializing files. */
	bool verify; /* Independently verify worker output on the caller. */
	unsigned int workers; /* 0 uses the current CPU affinity. */
	bool mapped_owner;
	uid_t host_uid;
	gid_t host_gid;
};

/*
 * Capture a bounded tree into gb. Descriptors are borrowed; failure sets errno.
 */
fy_generic fyai_project_capture(struct fy_generic_builder *gb,
				const struct fyai_project_capture_opts *opts, char *error_path,
				size_t error_size);

#endif
