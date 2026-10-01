/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FSVIEW_H
#define FYAI_FSVIEW_H

#include "fyai_project.h"

struct fyai_fsview {
	const char *project;
	const char *runtime;
	const char *storage;
	const char *arena;
	bool terminal;
	bool verify;
};

struct fyai_fsview_mount {
	uint64_t namespace_device;
	uint64_t namespace_inode;
	uint64_t mount_id;
	uint64_t cover_id;
	uint64_t root_inode;
};

/* Read-only inspection mounts persist in the caller's mount namespace. */
int fyai_fsview_mount(const struct fyai_fsview *view, const char *target,
		     struct fyai_fsview_mount *identity);
/* On a busy unmount, identity records any cover already removed. */
int fyai_fsview_unmount(const struct fyai_fsview *view, const char *target,
		       struct fyai_fsview_mount *identity);

/* Called only in a prepared tool child. Parent waits for the PID-namespace init. */
int fyai_fsview_enter(const struct fyai_fsview *view, int status_fd);

/* Ingest a frozen upper through a trusted private overlay mount. Result belongs to gb. */
fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb,
		const struct fyai_fsview *view, char *error, size_t error_size);

#endif
