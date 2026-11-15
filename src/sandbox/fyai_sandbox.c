/*
 * fyai_sandbox.c - the platform-independent part of tool confinement
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Environment sanitizing, the mode names, and the dispatch of the public
 * interface to the backend of the platform (fyai_sandbox_backend.h).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_sandbox_backend.h"


/*
 * Environment variables preserved for a tool sub-execution. Everything else -
 * notably any <PROVIDER>_API_KEY and other secrets fyai reads - is dropped so
 * it cannot leak into a tool or the processes it spawns.
 */
static const char *const fyai_env_keep[] = {
	"PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TZ", "TMPDIR",
	"LANG", "LANGUAGE",
	"DEVELOPER_DIR", "SDKROOT", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
	"LIBRARY_PATH", "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH",
};

/* True if @name of @len is one of the variables a tool may keep. */
static bool env_keep_name(const char *name, size_t len,
			  const char *const *keep)
{
	size_t i;

	for (; keep && *keep; keep++)
		if (!strncmp(name, *keep, len) && (*keep)[len] == '\0')
			return true;

	for (i = 0; i < sizeof(fyai_env_keep) / sizeof(*fyai_env_keep); i++)
		if (!strncmp(name, fyai_env_keep[i], len) &&
		    fyai_env_keep[i][len] == '\0')
			return true;
	/* Keep locale overrides (LC_*) too. */
	if (len >= 3 && !strncmp(name, "LC_", 3))
		return true;
	return false;
}

int fyai_env_sanitize(const char *const *keep)
{
	extern char **environ;
	char name[FYAI_ENV_NAME_MAX];
	const char *eq;
	size_t i, len;
	bool removed;
	int rc = 0;

	/* Avoid allocation in the child; restart after unsetenv() moves entries. */
	do {
		removed = false;
		for (i = 0; environ && environ[i]; i++) {
			eq = strchr(environ[i], '=');
			len = eq ? (size_t)(eq - environ[i]) :
				   strlen(environ[i]);
			if (env_keep_name(environ[i], len, keep))
				continue;
			if (len >= sizeof(name)) {
				/* Too long to name, so it cannot be removed. */
				rc = -1;
				continue;
			}
			memcpy(name, environ[i], len);
			name[len] = '\0';
			if (unsetenv(name) < 0) {
				rc = -1;
				continue;
			}
			removed = true;
			break;
		}
	} while (removed);

	return rc;
}

int fyai_sandbox_mode_parse(const char *name, enum fyai_sandbox_mode *modep)
{
	if (!name || !modep)
		return -1;
	if (!strcmp(name, "rw"))
		*modep = FYAI_SB_RW;
	else if (!strcmp(name, "ro"))
		*modep = FYAI_SB_RO;
	else if (!strcmp(name, "edit"))
		*modep = FYAI_SB_EDIT;
	else if (!strcmp(name, "append"))
		*modep = FYAI_SB_APPEND;
	else
		return -1;
	return 0;
}

/* True when @path is @root or beneath it. */
bool fyai_sandbox_path_within(const char *path, const char *root)
{
	size_t n = strlen(root);

	while (n > 1 && root[n - 1] == '/')
		n--;
	if (n == 1 && root[0] == '/')
		return true;
	return !strncmp(path, root, n) && (path[n] == '/' || path[n] == '\0');
}

/*
 * Rewrite PATH to the entries that the grants reach. The child owns its
 * environment, and the buffer is on the stack: no allocation after the fork.
 * Nothing left selects the system directories.
 */
void fyai_sandbox_filter_path(const struct fyai_sandbox_spec *spec)
{
	static const char fallback[] = "/usr/local/bin:/usr/bin:/bin";
	char out[8192], dir[4096];
	const char *path = getenv("PATH"), *end;
	size_t len, used = 0;

	if (!path)
		return;
	out[0] = '\0';
	while (*path) {
		end = strchr(path, ':');
		len = end ? (size_t)(end - path) : strlen(path);
		if (len && len < sizeof(dir)) {
			memcpy(dir, path, len);
			dir[len] = '\0';
			if (fyai_sandbox_path_visible(spec, dir) &&
			    used + len + 2 <= sizeof(out)) {
				if (used)
					out[used++] = ':';
				memcpy(out + used, dir, len + 1);
				used += len;
			}
		}
		path = end ? end + 1 : path + len;
	}
	setenv("PATH", used ? out : fallback, 1);
}

#if defined(FYAI_SANDBOX_BACKEND_LINUX)
#define backend (&fyai_sandbox_backend_linux)
#elif defined(FYAI_SANDBOX_BACKEND_MACOS)
#define backend (&fyai_sandbox_backend_macos)
#else
#define backend (&fyai_sandbox_backend_none)
#endif

int fyai_sandbox_abi(void)
{
	return backend->abi();
}

bool fyai_sandbox_available(void)
{
	return backend->available();
}

bool fyai_sandbox_net_restrictable(int abi)
{
	return backend->net_restrictable(abi);
}

bool fyai_sandbox_udp_restrictable(int abi)
{
	return backend->udp_restrictable(abi);
}

bool fyai_sandbox_udp_deniable(void)
{
	return backend->udp_deniable();
}

bool fyai_sandbox_path_visible(const struct fyai_sandbox_spec *spec, const char *dir)
{
	return backend->path_visible(spec, dir);
}

int fyai_sandbox_apply(const struct fyai_sandbox_spec *spec)
{
	return backend->apply(spec);
}
