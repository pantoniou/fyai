/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SANDBOX_BACKEND_H
#define FYAI_SANDBOX_BACKEND_H

#include "fyai_sandbox.h"

/*
 * The interface between the sandbox core (fyai_sandbox.c) and the confinement
 * of a platform. A backend lives in one file and exports one table. The core
 * calls the public functions of fyai_sandbox.h through the table of the
 * backend that this build selects; nothing else names a platform.
 *
 *   Linux   fyai_sandbox_linux.c   Landlock and a seccomp filter
 *   macOS   fyai_sandbox_macos.c   Seatbelt
 *   other   fyai_sandbox_none.c    no confinement
 */
#if defined(__APPLE__)
#define FYAI_SANDBOX_BACKEND_MACOS 1
#elif defined(__linux__) && __has_include(<linux/landlock.h>)
#define FYAI_SANDBOX_BACKEND_LINUX 1
#else
#define FYAI_SANDBOX_BACKEND_NONE 1
#endif

/* Each member has the contract of the public function of the same name. */
struct fyai_sandbox_backend {
	const char *name;
	int (*abi)(void);
	bool (*available)(void);
	bool (*net_restrictable)(int abi);
	bool (*udp_restrictable)(int abi);
	bool (*udp_deniable)(void);
	bool (*path_visible)(const struct fyai_sandbox_spec *spec, const char *dir);
	int (*apply)(const struct fyai_sandbox_spec *spec);
};

#ifdef FYAI_SANDBOX_BACKEND_LINUX
extern const struct fyai_sandbox_backend fyai_sandbox_backend_linux;
#endif
#ifdef FYAI_SANDBOX_BACKEND_MACOS
extern const struct fyai_sandbox_backend fyai_sandbox_backend_macos;
#endif
#ifdef FYAI_SANDBOX_BACKEND_NONE
extern const struct fyai_sandbox_backend fyai_sandbox_backend_none;
#endif

/* True when @path is @root or beneath it. */
bool fyai_sandbox_path_within(const char *path, const char *root);

/*
 * Rewrite PATH to the entries that the backend lets @spec reach. For use in
 * the confined child: it does not allocate.
 */
void fyai_sandbox_filter_path(const struct fyai_sandbox_spec *spec);

#endif
