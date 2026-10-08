/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FSVIEW_H
#define FYAI_FSVIEW_H

#include "fyai_manifest.h"
#include "fyai_project.h"

struct fyai_cfg;

/* Mount point of the backing runtime tree below the scratch directory. */
#define FYAI_FSVIEW_BACKING_NAME ".fyai-view-runtime"

/*
 * Make @fd, a directory of the arena, the arena of an agent runtime in a view:
 * close-on-exec, named /proc/self/fd/@fd, and in a process that is not
 * dumpable, so another process of the view cannot open it through /proc. An
 * arena directory that named the previous descriptor names the new one.
 */
int fyai_fsview_arena_adopt(struct fyai_cfg *cfg, int fd);
/* Close the arena descriptor in a process that runs no agent runtime. */
void fyai_fsview_arena_drop(struct fyai_cfg *cfg);
/* Keep the arena descriptor across the next execution of this program. */
int fyai_fsview_arena_pass(const struct fyai_cfg *cfg);

struct fyai_fsview {
	const char *project;
	/* Absolute directory that the child replaces with a private tmpfs. */
	const char *scratch;
	const char *runtime;
	const char *storage;
	const char *arena;
	/* Reference to the borrowed immutable lower manifest, for result capture. */
	fy_generic baseline;
	bool metacopy;
	bool terminal;
	bool verify;
	bool lazy;
	/*
	 * An agent runtime keeps the arena as a detached mount behind a
	 * descriptor. No projection mounts the arena at a path.
	 */
	bool agent;
};

struct fyai_fsview_mount {
	uint64_t namespace_device;
	uint64_t namespace_inode;
	uint64_t mount_id;
	uint64_t cover_id;
	uint64_t root_inode;
};

/*
 * Resolve the scratch directory: the configured path, else $TMPDIR, else
 * /tmp. The result is absolute and canonical; the caller frees it.
 */
char *fyai_fsview_scratch(const char *configured);

/*
 * A view keeps its baseline and its result as manifest files in the manifests
 * directory of the project storage, and holds a reference to each: a mapping
 * with the version, the root, the name of the file and the number of objects.
 */
#define FYAI_FSVIEW_SNAPSHOT_VERSION 3

/* Build a reference in gb. */
fy_generic fyai_fsview_reference(struct fy_generic_builder *gb,
				 const unsigned char root[FYAI_CAS_HASH_SIZE], uint64_t objects,
				 const char *name);

/*
 * Open the manifest that a reference names and check that it holds the root
 * of the reference. Return 0, or -1 with errno set.
 */
int fyai_fsview_manifest_open(const char *storage, fy_generic reference,
			      struct fyai_manifest *manifest);

/* Publish a manifest and return the name of its file. */
int fyai_fsview_manifest_publish(const char *storage, const struct fyai_manifest *manifest,
				 bool durable, char name[FYAI_MANIFEST_NAME_SIZE]);

/*
 * Reject a project that the scratch tmpfs or the backing tree would hide, and a
 * scratch directory that the project mount would hide.
 */
bool fyai_fsview_project_usable(const char *project, const char *scratch);

/* Probe rootless CAS reads, metadata, and isolated write copy-up. */
int fyai_fsview_metacopy_check(const char *runtime);
/* Whether this process may enter the user, mount and PID namespaces of a view. */
bool fyai_fsview_namespace_usable(void);

/* Read-only inspection mounts persist in the caller's mount namespace. */
int fyai_fsview_mount(const struct fyai_fsview *view, const char *target,
		      struct fyai_fsview_mount *identity);
/* On a busy unmount, identity records any cover already removed. */
int fyai_fsview_unmount(const struct fyai_fsview *view, const char *target,
			struct fyai_fsview_mount *identity);

/* Verify borrowed CAS provenance and bytes; error is a caller-owned buffer. */
int fyai_fsview_verify(const struct fyai_fsview *view, char *error, size_t error_size);

/*
 * Validate the retained lower, CAS bytes, and latest merged snapshot after
 * reboot.
 */
int fyai_fsview_recover(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			fy_generic expected, char *error, size_t error_size);

/*
 * Called only in a prepared tool child. Parent waits for the PID-namespace
 * init. A non-negative @announce_fd is a sequenced-packet socket. The process
 * that runs tool code sends one byte with a pidfd of itself, then waits for the
 * release of the supervisor and returns. The pidfd names the process in every
 * namespace, so a supervisor in an ancestor namespace, and the credential
 * transport, take its PID there. The release can carry one environment variable.
 * Every process closes the descriptor.
 */
int fyai_fsview_enter(struct fyai_cfg *cfg, const struct fyai_fsview *view,
		      int status_fd, int announce_fd);

/*
 * Supervisor side of @announce_fd. Receive the announcement and return the pidfd
 * of the process in @pidfd, which the caller closes.
 */
int fyai_fsview_init_pidfd(int fd, int *pidfd);

/*
 * Release the announced process, with one environment variable for it when @name
 * and @value are given. Not releasing it, and closing @fd, makes it fail.
 */
int fyai_fsview_init_release(int fd, const char *name, const char *value);

/* Ingest a frozen upper through a trusted private overlay mount. Result belongs
 * to gb. */
fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
				char *error, size_t error_size);

#endif
