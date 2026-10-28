/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FSVIEW_BACKEND_H
#define FYAI_FSVIEW_BACKEND_H

#include "fyai_fsview.h"

/*
 * The interface between the view core (fyai_fsview.c) and the view machinery
 * of a platform. A backend lives in one file and exports one table. The core
 * calls the platform functions of fyai_fsview.h through the table of the
 * backend that this build selects.
 *
 *   Linux   fyai_fsview_linux.c   OverlayFS, namespaces and CAS-backed mounts
 *   macOS   fyai_fsview_macos.c   a cloned project directory
 *   other   fyai_fsview_none.c    no views
 *
 * A member that is NULL is not supported by the backend. The core then fails
 * with ENOTSUP, or does nothing where the function cannot fail. A member that
 * says otherwise below has another default.
 */
#if defined(__linux__)
#define FYAI_FSVIEW_BACKEND_LINUX 1
#elif defined(__APPLE__)
#define FYAI_FSVIEW_BACKEND_MACOS 1
#else
#define FYAI_FSVIEW_BACKEND_NONE 1
#endif

/* Each member has the contract of the function of fyai_fsview.h with its name. */
struct fyai_fsview_backend {
	const char *name;
	int (*arena_adopt)(struct fyai_cfg *cfg, int fd);
	void (*arena_drop)(struct fyai_cfg *cfg);
	int (*arena_pass)(const struct fyai_cfg *cfg);
	int (*metacopy_check)(const char *runtime);
	bool (*namespace_usable)(void);
	bool (*hide_usable)(void);
	int (*hide)(const char *const *paths, size_t n,
		    const struct fyai_sandbox_mount *mount, size_t mount_n);
	int (*mounts)(const struct fyai_sandbox_mount *mount, size_t n, bool late);
	int (*holder)(const char *const *paths, size_t n, int hold, int owner,
		      pid_t *waiter, pid_t *init);
	void (*on_parent_exit)(void (*fn)(void *arg), void *arg);
	int (*outer)(void);
	void (*set_grace)(long ms);
	int (*drop_capabilities)(void);
	int (*mount)(const struct fyai_fsview *view, const char *target,
		     struct fyai_fsview_mount *identity);
	int (*unmount)(const struct fyai_fsview *view, const char *target,
		       struct fyai_fsview_mount *identity);
	int (*recover)(struct fy_generic_builder *gb, const struct fyai_fsview *view,
		       fy_generic expected, char *error, size_t error_size);
	int (*enter)(struct fyai_cfg *cfg, const struct fyai_fsview *view,
		     int status_fd, int announce_fd);
	int (*init_pidfd)(int fd, int *pidfd);
	int (*init_release)(int fd, const char *name, const char *value);
	fy_generic (*snapshot)(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			       char *error, size_t error_size);
	/* Runs terminal sessions, agents, the transport and profiles in a view. */
	bool sessions;
	int (*sync_backing)(const char *runtime, const char *storage);
	/* NULL: nothing to prepare. */
	int (*prepare)(struct fyai_ctx *ctx, struct fyai_fsview *spec, fy_generic *view,
		       char **execution, char *error, size_t error_size);
	/* NULL: nothing to confine. */
	int (*confine)(struct fyai_ctx *ctx, const struct fyai_fsview *view);
};

#ifdef FYAI_FSVIEW_BACKEND_LINUX
extern const struct fyai_fsview_backend fyai_fsview_backend_linux;
#endif
#ifdef FYAI_FSVIEW_BACKEND_MACOS
extern const struct fyai_fsview_backend fyai_fsview_backend_macos;
#endif
#ifdef FYAI_FSVIEW_BACKEND_NONE
extern const struct fyai_fsview_backend fyai_fsview_backend_none;
#endif

/* True when @name is the name of a manifest file: lower-case hexadecimal of the digest size. */
bool fyai_fsview_manifest_name_valid(const char *name);

#endif
