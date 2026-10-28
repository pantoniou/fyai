/* SPDX-License-Identifier: MIT */
#include "fyai_fsview_backend.h"

#ifdef FYAI_FSVIEW_BACKEND_MACOS
#include <copyfile.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fyai.h"
#include "fyai_project_capture.h"
#include "fyai_tools.h"

static bool native_path(const char *value, const char *logical)
{
	size_t length = strlen(logical);

	return !strncmp(value, logical, length) && (!value[length] || value[length] == '/');
}

static char *native_map(const char *value, const char *logical, const char *physical)
{
	char *mapped;

	if (!native_path(value, logical))
		return strdup(value);
	if (asprintf(&mapped, "%s%s", physical, value + strlen(logical)) < 0)
		return NULL;
	return mapped;
}

static bool native_path_list(const char *name, size_t length)
{
	static const char *const names[] = {
		"PATH", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
		"LIBRARY_PATH", "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH", NULL,
	};
	const char *const *p;

	for (p = names; *p; p++)
		if (strlen(*p) == length && !strncmp(name, *p, length))
			return true;
	return false;
}

/* The child owns its environment; setenv may replace its backing array. */
static int native_environment(const char *logical, const char *physical, const char *scratch)
{
	extern char **environ;
	char **names, *name, *value, *copy, *entry, *next, *mapped, *output;
	const char *equal;
	size_t count = 0, i, length, used;
	bool list;
	int rc, result = -1;

	while (environ && environ[count])
		count++;
	names = calloc(count + 1, sizeof(*names));
	if (!names)
		return -1;
	for (i = 0; i < count; i++) {
		equal = strchr(environ[i], '=');
		if (!equal)
			continue;
		length = equal - environ[i];
		list = native_path_list(environ[i], length);
		if (!list && !native_path(equal + 1, logical))
			continue;
		names[i] = strndup(environ[i], length);
		if (!names[i])
			goto out;
	}
	for (i = 0; i < count; i++) {
		name = names[i];
		if (!name)
			continue;
		value = getenv(name);
		list = native_path_list(name, strlen(name));
		if (!list) {
			mapped = native_map(value, logical, physical);
			if (!mapped)
				goto out;
			rc = setenv(name, mapped, 1);
			free(mapped);
			if (rc)
				goto out;
			continue;
		}
		copy = strdup(value);
		output = NULL;
		used = 0;
		if (!copy)
			goto out;
		for (entry = copy; entry; entry = next) {
			next = strchr(entry, ':');
			if (next)
				*next++ = '\0';
			mapped = native_map(entry, logical, physical);
			if (!mapped) {
				free(output);
				free(copy);
				goto out;
			}
			length = strlen(mapped);
			value = realloc(output, used + length + 2);
			if (!value) {
				free(mapped);
				free(output);
				free(copy);
				goto out;
			}
			output = value;
			memcpy(output + used, mapped, length);
			used += length;
			if (next)
				output[used++] = ':';
			output[used] = '\0';
			free(mapped);
		}
		rc = setenv(name, output, 1);
		free(output);
		free(copy);
		if (rc)
			goto out;
	}
	rc = setenv("PWD", physical, 1);
	if (!rc)
		rc = setenv("TMPDIR", scratch, 1);
	if (!rc)
		rc = unsetenv("OLDPWD");
	result = rc;
out:
	for (i = 0; i < count; i++)
		free(names[i]);
	free(names);
	return result;
}

static int mac_enter(struct fyai_cfg *cfg, const struct fyai_fsview *view,
		      int status_fd, int announce_fd)
{
	char *scratch, *slash;
	int rc;

	(void)cfg;
	(void)status_fd;
	if (view->agent || announce_fd >= 0 || !view->execution) {
		errno = ENOTSUP;
		return -1;
	}
	scratch = strdup(view->execution);
	if (!scratch)
		return -1;
	slash = strrchr(scratch, '/');
	if (!slash || strcmp(slash + 1, "project")) {
		free(scratch);
		errno = EINVAL;
		return -1;
	}
	strcpy(slash + 1, "tmp");
	rc = native_environment(view->project, view->execution, scratch);
	if (!rc)
		rc = chdir(view->execution);
	free(scratch);
	return rc;
}

static fy_generic mac_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			       char *error, size_t error_size)
{
	struct fyai_project_capture_opts opts = {
		.source_fd = -1, .objects_fd = -1, .baseline_fd = -1, .upper_fd = -1,
		.verify = view->verify, .defer_sync = view->lazy,
	};
	struct fyai_manifest manifest = { 0 };
	char name[FYAI_MANIFEST_NAME_SIZE];
	fy_generic result = fy_invalid;
	int rc, saved;

	if (!view->execution) {
		errno = ENOTSUP;
		return fy_invalid;
	}
	opts.source_fd = open(view->execution, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	opts.objects_fd = open(fy_sprintfa("%s/objects/blake3", view->storage),
				 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (opts.source_fd < 0 || opts.objects_fd < 0)
		goto out;
	rc = fyai_project_capture_manifest(&opts, &manifest, error, error_size);
	if (!rc)
		rc = fyai_fsview_manifest_publish(view->storage, &manifest, !view->lazy, name);
	if (!rc)
		result = fyai_fsview_reference(gb, manifest.root, manifest.object_count, name);
out:
	saved = errno;
	fyai_manifest_close(&manifest);
	if (opts.source_fd >= 0)
		close(opts.source_fd);
	if (opts.objects_fd >= 0)
		close(opts.objects_fd);
	errno = saved;
	return result;
}

static int mac_recover(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			fy_generic expected, char *error, size_t error_size)
{
	struct fyai_fsview check = *view;
	fy_generic found;

	if (!view->execution)
		return fyai_fsview_verify(view, error, error_size);
	check.lazy = true;
	found = mac_snapshot(gb, &check, error, error_size);
	if (!fy_is_valid(found))
		return -1;
	if (!fy_equal(fy_get(found, "root", fy_invalid), fy_get(expected, "root", fy_invalid))) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int mac_sync_tree(int fd, unsigned int depth)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int child, rc = 0, saved;

	if (depth > FYAI_PROJECT_MAX_DEPTH + 16) {
		errno = E2BIG;
		return -1;
	}
	child = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (child < 0)
		return -1;
	directory = fdopendir(child);
	if (!directory) {
		saved = errno;
		close(child);
		errno = saved;
		return -1;
	}
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			rc = errno ? -1 : 0;
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		rc = fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW);
		if (rc)
			break;
		if (S_ISLNK(st.st_mode))
			continue;
		if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
			errno = ENOTSUP;
			rc = -1;
			break;
		}
		child = openat(fd, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
			       (S_ISDIR(st.st_mode) ? O_DIRECTORY : O_NONBLOCK));
		if (child < 0) {
			rc = -1;
			break;
		}
		rc = S_ISDIR(st.st_mode) ? mac_sync_tree(child, depth + 1) :
					  fcntl(child, F_FULLFSYNC);
		saved = errno;
		close(child);
		errno = saved;
		if (rc)
			break;
	}
	if (!rc)
		rc = fsync(fd);
	saved = errno;
	closedir(directory);
	errno = saved;
	return rc;
}

static int mac_sync_backing(const char *runtime, const char *storage)
{
	/* There is no syncfs(): flush the runtime of the view and its storage tree by tree. */
	const char *const trees[] = { runtime, storage };
	size_t i;
	int fd, rc, saved;

	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		fd = open(trees[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return -1;
		rc = mac_sync_tree(fd, 0);
		saved = errno;
		close(fd);
		errno = saved;
		if (rc)
			return -1;
	}
	return 0;
}

static int mac_clone(struct fyai_ctx *ctx, struct fyai_fsview *spec, fy_generic *view,
		     char **execution)
{
	fy_generic recorded = fy_get(*view, "execution", fy_invalid);
	char *parent = NULL, *target = NULL, *resolved, *slash;
	const char *relative;
	size_t i;
	const char *source;
	struct stat st;
	int rc = -1, saved, fd;

	if (fy_is_valid(recorded)) {
		if (!fy_is_string(recorded)) {
			errno = EINVAL;
			return -1;
		}
		*execution = strdup(fy_castp(&recorded, ""));
		if (!*execution)
			return -1;
		resolved = realpath(*execution, NULL);
		if (!resolved && errno == ENOENT) {
			/* The system removed the clone from the scratch directory: make it again. */
			free(*execution);
			*execution = NULL;
			goto create;
		}
		if (!resolved)
			return -1;
		rc = strcmp(resolved, *execution);
		free(resolved);
		if (rc || !fyai_path_beneath(*execution, spec->scratch) ||
		    strncmp(*execution + strlen(spec->scratch), "/fyai-view-", 11) ||
		    strcmp(strrchr(*execution, '/'), "/project")) {
			errno = EINVAL;
			return -1;
		}
		relative = *execution + strlen(spec->scratch);
		if (strlen(relative) != 25) {
			errno = EINVAL;
			return -1;
		}
		for (i = 11; i < 17; i++) {
			if (!isalnum((unsigned char)relative[i])) {
				errno = EINVAL;
				return -1;
			}
		}
		parent = strdup(*execution);
		if (!parent)
			return -1;
		slash = strrchr(parent, '/');
		*slash = '\0';
		rc = lstat(parent, &st);
		free(parent);
		if (rc || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
			errno = EACCES;
			return -1;
		}
		rc = stat(*execution, &st);
		if (rc || !S_ISDIR(st.st_mode))
			return -1;
		spec->execution = *execution;
		return 0;
	}
create:
	if (fy_is_valid(fy_get(*view, "result", fy_invalid))) {
		errno = ENOTSUP;
		return -1;
	}
	if (asprintf(&parent, "%s/fyai-view-XXXXXX", spec->scratch) < 0)
		return -1;
	if (!mkdtemp(parent))
		goto out;
	if (asprintf(&target, "%s/project", parent) < 0)
		goto out;
	source = fy_sprintfa("%s/baseline", spec->runtime);
	rc = copyfile(source, target, NULL, COPYFILE_RECURSIVE | COPYFILE_CLONE);
	if (rc)
		goto out;
	rc = mkdir(fy_sprintfa("%s/tmp", parent), 0700);
	if (rc)
		goto out;
	*execution = target;
	target = NULL;
	spec->execution = *execution;
	*view = fy_assoc(ctx->gb, *view, "execution", fy_value(ctx->gb, *execution));
	rc = fy_is_mapping(*view) ? 0 : -1;
out:
	saved = errno;
	if (rc && parent) {
		fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd >= 0) {
			fyai_fsview_runtime_clear(fd);
			close(fd);
			rmdir(parent);
		}
	}
	free(target);
	free(parent);
	errno = saved;
	return rc;
}

static int mac_prepare(struct fyai_ctx *ctx, struct fyai_fsview *spec, fy_generic *view,
		       char **execution, char *error, size_t error_size)
{
	bool created;
	int rc;

	created = !fy_is_valid(fy_get(*view, "execution", fy_invalid));
	rc = fyai_fsview_verify(spec, error, error_size);
	if (rc)
		return -1;
	rc = mac_clone(ctx, spec, view, execution);
	if (rc)
		return -1;
	if (created)
		return mac_recover(ctx->gb, spec, spec->baseline, error, error_size);
	return 0;
}


const struct fyai_fsview_backend fyai_fsview_backend_macos = {
	.name = "macos",
	.enter = mac_enter,
	.recover = mac_recover,
	.snapshot = mac_snapshot,
	.sync_backing = mac_sync_backing,
	.prepare = mac_prepare,
	.confine = fyai_tools_confine_native_view,
};

#endif /* FYAI_FSVIEW_BACKEND_MACOS */
