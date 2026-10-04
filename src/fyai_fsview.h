/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FSVIEW_H
#define FYAI_FSVIEW_H

#include "fyai_manifest.h"
#include "fyai_project.h"

/* Mount point of the backing runtime tree below the scratch directory. */
#define FYAI_FSVIEW_BACKING_NAME ".fyai-view-runtime"

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
	 * Agent runtime projection: the arena stays writable at its own path
	 * and only the project storage is denied. A tool projection covers
	 * both.
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

/* Reject a project that the scratch tmpfs or the backing tree would hide. */
bool fyai_fsview_project_usable(const char *project, const char *scratch);

/* Probe rootless CAS reads, metadata, and isolated write copy-up. */
int fyai_fsview_metacopy_check(const char *runtime);

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
 * Make the mount points that the agent projection needs below the cover of
 * the protected directory. Call it in the supervisor before the child enters.
 */
int fyai_fsview_agent_prepare(const struct fyai_fsview *view);

/*
 * Called only in a prepared tool child. Parent waits for the PID-namespace
 * init.
 */
int fyai_fsview_enter(const struct fyai_fsview *view, int status_fd);

/* Ingest a frozen upper through a trusted private overlay mount. Result belongs
 * to gb. */
fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
				char *error, size_t error_size);

#endif
