/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROJECT_CAPTURE_H
#define FYAI_PROJECT_CAPTURE_H

#include <sys/types.h>
#include "fyai_cas.h"
#include "fyai_ignore.h"
#include "fyai_manifest.h"
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
	enum fyai_cas_method copy_backend;
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
	/*
	 * The baseline, required for an incremental capture. A manifest is used as it
	 * is; a snapshot generic is converted for the capture. The manifest wins.
	 */
	const struct fyai_manifest *baseline_manifest;
	fy_generic snapshot;
	/*
	 * What the scan leaves out, besides the reserved directories: the paths that the
	 * rules ignore, in the project and in a view that continues it. NULL ignores
	 * nothing. Only the paths that a rule ignores are changed: the capture of a
	 * result must use what the capture of its baseline used.
	 */
	const struct fyai_ignore_spec *ignore;
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
 * Capture a bounded tree into a manifest, which the caller closes. Descriptors
 * are borrowed. Return 0, or -1 with errno set and the path in error_path.
 */
int fyai_project_capture_manifest(const struct fyai_project_capture_opts *opts,
				  struct fyai_manifest *manifest, char *error_path,
				  size_t error_size);

/*
 * Capture an incremental tree as a delta over opts->baseline_manifest, which must
 * be a full manifest whose file is named base_name. The tree is not built whole:
 * the delta holds the changed objects and the exact list of removed ones.
 */
int fyai_project_capture_delta(const struct fyai_project_capture_opts *opts, const char *base_name,
			       struct fyai_manifest *delta, char *error_path, size_t error_size);

/*
 * Capture a bounded tree into gb as a snapshot generic. Descriptors are
 * borrowed; failure sets errno.
 */
fy_generic fyai_project_capture(struct fy_generic_builder *gb,
				const struct fyai_project_capture_opts *opts, char *error_path,
				size_t error_size);

#endif
