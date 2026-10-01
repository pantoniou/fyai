/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FSVIEW_H
#define FYAI_FSVIEW_H

#include "fyai_project.h"

struct fyai_fsview {
	const char *project;
	const char *runtime;
	const char *storage;
	const char *arena;
};

/* Called only in a prepared tool child. Parent waits for the PID-namespace init. */
int fyai_fsview_enter(const struct fyai_fsview *view, int status_fd);

/* Ingest a frozen upper through a trusted private overlay mount. Result belongs to gb. */
fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb,
		const struct fyai_fsview *view, char *error, size_t error_size);

#endif
