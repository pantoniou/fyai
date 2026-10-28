/*
 * fyai_fsview.c - views of a captured project: the platform-independent part
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_fsview_backend.h"
#include "fyai_project_capture.h"
#include "fyai_scratch.h"
#include "fyai_wire.h"
#include "fyai_sandbox.h"

/* The manifest files of a view live next to its objects, in the project storage. */
#define VIEW_MANIFESTS "manifests"

bool fyai_fsview_manifest_name_valid(const char *name)
{
	size_t i;

	if (strlen(name) != FYAI_CAS_DIGEST_SIZE - 1)
		return false;
	for (i = 0; name[i]; i++)
		if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
			return false;
	return true;
}

static int view_manifest_directory(const char *storage, bool create)
{
	const char *path = fy_sprintfa("%s/" VIEW_MANIFESTS, storage);

	if (create && mkdir(path, 0700) && errno != EEXIST)
		return -1;
	return open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

fy_generic fyai_fsview_reference(struct fy_generic_builder *gb,
				 const unsigned char root[FYAI_CAS_HASH_SIZE], uint64_t objects,
				 const char *name)
{
	char hex[FYAI_CAS_DIGEST_SIZE];

	fyai_cas_hex(hex, root, FYAI_CAS_HASH_SIZE);
	return fy_mapping(gb, "version", (long long)FYAI_FSVIEW_SNAPSHOT_VERSION, "root",
			  fy_value(gb, hex), "manifest", fy_value(gb, name), "objects",
			  (long long)objects);
}

int fyai_fsview_manifest_open(const char *storage, fy_generic reference,
			      struct fyai_manifest *manifest)
{
	const char *name = fy_get(reference, "manifest", "");
	char hex[FYAI_CAS_DIGEST_SIZE];
	int directory, rc, saved;

	if (!fy_equal(fy_get(reference, "version", fy_invalid),
		      (long long)FYAI_FSVIEW_SNAPSHOT_VERSION) ||
	    !fyai_fsview_manifest_name_valid(name)) {
		errno = ENOTSUP;
		return -1;
	}
	directory = view_manifest_directory(storage, false);
	if (directory < 0)
		return -1;
	rc = fyai_manifest_open_at(manifest, directory, name);
	saved = errno;
	close(directory);
	if (rc) {
		errno = saved;
		return -1;
	}
	/* The reference names the root that the file must hold. */
	fyai_cas_hex(hex, manifest->root, FYAI_CAS_HASH_SIZE);
	if (strcmp(hex, fy_get(reference, "root", ""))) {
		fyai_manifest_close(manifest);
		errno = EBADMSG;
		return -1;
	}
	return 0;
}

int fyai_fsview_manifest_publish(const char *storage, const struct fyai_manifest *manifest,
				 bool durable, char name[FYAI_MANIFEST_NAME_SIZE])
{
	int directory, rc, saved;

	directory = view_manifest_directory(storage, true);
	if (directory < 0)
		return -1;
	rc = fyai_manifest_write(manifest, directory, durable, name);
	saved = errno;
	close(directory);
	errno = saved;
	return rc;
}

int fyai_fsview_verify(const struct fyai_fsview *view, char *error, size_t size)
{
	struct fyai_manifest manifest;
	const char *path;
	int fd, rc, saved;

	if (fyai_fsview_manifest_open(view->storage, view->baseline, &manifest))
		return -1;
	path = fy_sprintfa("%s/objects/blake3", view->storage);
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		saved = errno;
		fyai_manifest_close(&manifest);
		errno = saved;
		return -1;
	}
	rc = fyai_manifest_check_borrowed(&manifest, fd, view->verify, error, size);
	saved = errno;
	close(fd);
	fyai_manifest_close(&manifest);
	errno = saved;
	return rc;
}

char *fyai_fsview_scratch(const char *configured)
{
	const char *dir = configured;

	if (!dir || !*dir)
		dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/tmp";
	if (*dir != '/') {
		errno = EINVAL;
		return NULL;
	}
	return realpath(dir, NULL);
}

bool fyai_fsview_project_usable(const char *project, const char *scratch)
{
	return strcmp(project, "/") && strcmp(project, scratch) &&
	       !fyai_path_beneath(scratch, project) &&
	       !fyai_path_beneath(project, fy_sprintfa("%s/" FYAI_FSVIEW_BACKING_NAME, scratch));
}


/*
 * Remove only private runtime entries; never change shared blob file metadata.
 */
static int runtime_clear(int fd, unsigned int depth)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int child, rc = 0, saved;

	if (depth > 128) {
		errno = ELOOP;
		return -1;
	}
	rc = fchmod(fd, 0700);
	if (rc)
		return -1;
	child = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
		if (S_ISDIR(st.st_mode)) {
			/*
			 * OverlayFS leaves a work directory with no mode, which not even its
			 * owner can open. It is a directory, not a link: open it after the mode.
			 */
			rc = fchmodat(fd, entry->d_name, 0700, 0);
			if (rc && errno != EPERM)
				break;
			child = openat(fd, entry->d_name,
				       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (child < 0) {
				rc = -1;
				break;
			}
			rc = runtime_clear(child, depth + 1);
			saved = errno;
			close(child);
			errno = saved;
			if (rc)
				break;
		}
		rc = unlinkat(fd, entry->d_name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0);
		if (rc)
			break;
	}
	saved = errno;
	closedir(directory);
	errno = saved;
	return rc;
}

#if defined(FYAI_FSVIEW_BACKEND_LINUX)
#define backend (&fyai_fsview_backend_linux)
#else
#define backend (&fyai_fsview_backend_none)
#endif

int fyai_fsview_arena_adopt(struct fyai_cfg *cfg, int fd)
{
	if (!backend->arena_adopt) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->arena_adopt(cfg, fd);
}

void fyai_fsview_arena_drop(struct fyai_cfg *cfg)
{
	if (backend->arena_drop)
		backend->arena_drop(cfg);
}

int fyai_fsview_arena_pass(const struct fyai_cfg *cfg)
{
	return backend->arena_pass ? backend->arena_pass(cfg) : 0;
}

int fyai_fsview_metacopy_check(const char *runtime)
{
	if (!backend->metacopy_check) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->metacopy_check(runtime);
}

bool fyai_fsview_namespace_usable(void)
{
	return backend->namespace_usable && backend->namespace_usable();
}

bool fyai_fsview_hide_usable(void)
{
	return backend->hide_usable && backend->hide_usable();
}

int fyai_fsview_hide(const char *const *paths, size_t n,
		     const struct fyai_sandbox_mount *mount, size_t mount_n)
{
	if (!backend->hide) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->hide(paths, n, mount, mount_n);
}

int fyai_fsview_mounts(const struct fyai_sandbox_mount *mount, size_t n, bool late)
{
	if (!backend->mounts) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->mounts(mount, n, late);
}

int fyai_fsview_holder(const char *const *paths, size_t n, int hold,
		       int owner, pid_t *waiter, pid_t *init)
{
	if (!backend->holder) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->holder(paths, n, hold, owner, waiter, init);
}

void fyai_fsview_on_parent_exit(void (*fn)(void *arg), void *arg)
{
	if (backend->on_parent_exit)
		backend->on_parent_exit(fn, arg);
}

int fyai_fsview_outer(void)
{
	return backend->outer ? backend->outer() : 0;
}

void fyai_fsview_set_grace(long ms)
{
	if (backend->set_grace)
		backend->set_grace(ms);
}

int fyai_fsview_drop_capabilities(void)
{
	if (!backend->drop_capabilities) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->drop_capabilities();
}

int fyai_fsview_mount(const struct fyai_fsview *view, const char *target,
		      struct fyai_fsview_mount *identity)
{
	if (!backend->mount) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->mount(view, target, identity);
}

int fyai_fsview_unmount(const struct fyai_fsview *view, const char *target,
			struct fyai_fsview_mount *identity)
{
	if (!backend->unmount) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->unmount(view, target, identity);
}

int fyai_fsview_recover(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			fy_generic expected, char *error, size_t error_size)
{
	if (!backend->recover) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->recover(gb, view, expected, error, error_size);
}

int fyai_fsview_enter(struct fyai_cfg *cfg, const struct fyai_fsview *view,
		      int status_fd, int announce_fd)
{
	if (!backend->enter) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->enter(cfg, view, status_fd, announce_fd);
}

int fyai_fsview_init_pidfd(int fd, int *pidfd)
{
	if (!backend->init_pidfd) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->init_pidfd(fd, pidfd);
}

int fyai_fsview_init_release(int fd, const char *name, const char *value)
{
	if (!backend->init_release) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->init_release(fd, name, value);
}

fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
				char *error, size_t error_size)
{
	if (!backend->snapshot) {
		errno = ENOTSUP;
		return fy_invalid;
	}
	return backend->snapshot(gb, view, error, error_size);
}

int fyai_fsview_runtime_clear(int fd)
{
	return runtime_clear(fd, 0);
}

bool fyai_fsview_sessions_supported(void)
{
	return backend->sessions;
}

int fyai_fsview_sync_backing(const char *runtime, const char *storage)
{
	if (!backend->sync_backing) {
		errno = ENOTSUP;
		return -1;
	}
	return backend->sync_backing(runtime, storage);
}

int fyai_fsview_prepare(struct fyai_ctx *ctx, struct fyai_fsview *spec, fy_generic *view,
			char **execution, char *error, size_t error_size)
{
	return backend->prepare ?
	       backend->prepare(ctx, spec, view, execution, error, error_size) : 0;
}

int fyai_fsview_confine(struct fyai_ctx *ctx, const struct fyai_fsview *view)
{
	return backend->confine ? backend->confine(ctx, view) : 0;
}
