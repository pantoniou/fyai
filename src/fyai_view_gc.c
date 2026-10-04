/*
 * fyai_view_gc.c - collect the project storage of views and project states
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The project storage (<project>/.fyai) holds manifests, blobs and the runtime
 * trees of views. Their references are in the arena: the project state of the
 * ref-log entries, and the stored views of each branch. A file that no
 * reference reaches is garbage.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "fyai.h"
#include "fyai_view.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fyai_branch.h"
#include "fyai_cas.h"
#include "fyai_manifest.h"
#include "fyai_storage.h"

#define GC_LOCK_NAME "gc.lock"
#define GC_ENTRY_MAX 4096

struct strv {
	char **items;
	size_t count, capacity;
	bool sorted;
};

struct storage_roots {
	char *path;
	struct strv manifests;
	struct strv runtimes;
};

struct gc {
	struct fyai_ctx *ctx;
	struct storage_roots *storages;
	size_t count, capacity;
	time_t cutoff;
	struct fyai_view_gc_stats *stats;
};

static int strv_add(struct strv *v, const char *text)
{
	char **items;

	if (v->count == v->capacity) {
		v->capacity = v->capacity ? v->capacity * 2 : 16;
		items = realloc(v->items, v->capacity * sizeof(*items));
		if (!items)
			return -1;
		v->items = items;
	}
	v->items[v->count] = strdup(text);
	if (!v->items[v->count])
		return -1;
	v->count++;
	v->sorted = false;
	return 0;
}

static int strv_order(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool strv_has(struct strv *v, const char *text)
{
	if (!v->sorted) {
		qsort(v->items, v->count, sizeof(*v->items), strv_order);
		v->sorted = true;
	}
	return v->count && bsearch(&text, v->items, v->count, sizeof(*v->items), strv_order);
}

static void strv_free(struct strv *v)
{
	size_t i;

	for (i = 0; i < v->count; i++)
		free(v->items[i]);
	free(v->items);
	memset(v, 0, sizeof(*v));
}

static bool name_is_digest(const char *name)
{
	size_t i;

	if (strlen(name) != FYAI_CAS_DIGEST_SIZE - 1)
		return false;
	for (i = 0; name[i]; i++)
		if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
			return false;
	return true;
}

/* Whether the arena at path is the one of this run. */
static bool fyai_view_same_arena(struct fyai_ctx *ctx, const char *path)
{
	char *mine = realpath(ctx->cfg->arena_dir, NULL), *theirs = realpath(path, NULL);
	bool same = mine && theirs && !strcmp(mine, theirs);

	free(mine);
	free(theirs);
	return same;
}

int fyai_view_storage_lock_shared(const char *storage)
{
	const char *path = fy_sprintfa("%s/" GC_LOCK_NAME, storage);
	int fd, rc;

	fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	do {
		rc = flock(fd, LOCK_SH);
	} while (rc && errno == EINTR);
	if (rc) {
		close(fd);
		return -1;
	}
	return fd;
}

static struct storage_roots *gc_storage(struct gc *gc, const char *path)
{
	struct storage_roots *roots;
	size_t i;

	for (i = 0; i < gc->count; i++)
		if (!strcmp(gc->storages[i].path, path))
			return &gc->storages[i];
	if (gc->count == gc->capacity) {
		gc->capacity = gc->capacity ? gc->capacity * 2 : 4;
		roots = realloc(gc->storages, gc->capacity * sizeof(*roots));
		if (!roots)
			return NULL;
		gc->storages = roots;
	}
	roots = &gc->storages[gc->count];
	memset(roots, 0, sizeof(*roots));
	roots->path = strdup(path);
	if (!roots->path)
		return NULL;
	gc->count++;
	return roots;
}

static int gc_root_manifest(struct gc *gc, const char *storage, fy_generic reference)
{
	struct storage_roots *roots;
	const char *name = fy_get(reference, "manifest", "");

	if (fy_str_empty(storage) || !name_is_digest(name))
		return 0;
	roots = gc_storage(gc, storage);
	return roots && !strv_add(&roots->manifests, name) ? 0 : -1;
}

/* The views that a branch stores, and the project state of each entry of its ref log. */
static int gc_collect_branch(struct gc *gc, fy_generic entry)
{
	struct fyai_branch b;
	struct storage_roots *roots;
	fy_generic views, view;
	const char *name, *storage, *runtime;
	unsigned int n;
	int rc = 0;

	for (n = 0; n < GC_ENTRY_MAX && fy_is_valid(entry) && !rc; n++) {
		if (!fyai_branch_decode(entry, &b))
			break;
		if (fy_is_mapping(b.project))
			rc = gc_root_manifest(gc, fy_get(b.project, "storage", ""), b.project);
		/* The views of the newest entry are the views of the branch. */
		if (!n && !rc) {
			views = fy_get(b.store, "views", fy_invalid);
			fy_foreach_key_value(name, view, views) {
				storage = fy_get(view, "storage", "");
				runtime = fy_get(view, "runtime", "");
				rc = gc_root_manifest(gc, storage, fy_get(view, "baseline", fy_invalid));
				if (!rc)
					rc = gc_root_manifest(gc, storage,
							      fy_get(view, "result", fy_invalid));
				if (rc || fy_str_empty(storage) || fy_str_empty(runtime))
					continue;
				roots = gc_storage(gc, storage);
				rc = roots && !strv_add(&roots->runtimes, runtime) ? 0 : -1;
			}
		}
		if (!fyai_branch_entry_contained(gc->ctx->durable_allocator, b.prev, 1))
			break;
		entry = b.prev;
	}
	return rc;
}

static bool gc_old(struct gc *gc, const struct stat *st)
{
	time_t newest = st->st_mtime > st->st_ctime ? st->st_mtime : st->st_ctime;

	return newest <= gc->cutoff;
}

static void gc_count(struct gc *gc, const struct stat *st, size_t *counter)
{
	(*counter)++;
	/* A blob with another link keeps its bytes. */
	if (S_ISREG(st->st_mode) && st->st_nlink == 1)
		gc->stats->bytes += (uint64_t)st->st_size;
}

/*
 * The blobs of the live manifests, and the manifests that a delta needs for its
 * base. A manifest that cannot be read is kept with what it names, and the
 * sweep of blobs is skipped, because the blobs it names are unknown.
 */
static int gc_live(struct gc *gc, struct storage_roots *roots, int manifests, struct strv *blobs,
		   bool *complete)
{
	struct fyai_manifest manifest;
	struct fyai_mobject object;
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	char hex[FYAI_CAS_DIGEST_SIZE], name[FYAI_CAS_DIGEST_SIZE + 16];
	size_t i, position;

	*complete = true;
	for (i = 0; i < roots->manifests.count; i++) {
		memset(&manifest, 0, sizeof(manifest));
		if (fyai_manifest_open_at(&manifest, manifests, roots->manifests.items[i])) {
			fyai_warning(gc->ctx, "gc: cannot read the manifest %s of %s: %s; its objects "
					      "are kept",
				     roots->manifests.items[i], roots->path, strerror(errno));
			*complete = false;
			continue;
		}
		if (manifest.delta) {
			fyai_cas_hex(hex, manifest.base_digest, FYAI_CAS_HASH_SIZE);
			if (!strv_has(&roots->manifests, hex) && strv_add(&roots->manifests, hex)) {
				fyai_manifest_close(&manifest);
				return -1;
			}
		}
		for (position = 0; position < manifest.object_count; position++) {
			if (!fyai_manifest_object_at(&manifest, position, digest, &object) ||
			    object.kind != FYAI_PROJECT_FILE)
				continue;
			if (fyai_cas_name(name, sizeof(name), &object.blob) ||
			    strv_add(blobs, name)) {
				fyai_manifest_close(&manifest);
				return -1;
			}
		}
		fyai_manifest_close(&manifest);
	}
	return 0;
}

static void gc_sweep_directory(struct gc *gc, int directory, const char *prefix,
			       struct strv *live, size_t *counter)
{
	struct dirent *entry;
	struct stat st;
	char name[PATH_MAX];
	DIR *stream;
	int fd;

	fd = dup(directory);
	if (fd < 0)
		return;
	stream = fdopendir(fd);
	if (!stream) {
		close(fd);
		return;
	}
	while ((entry = readdir(stream))) {
		if (!name_is_digest(entry->d_name))
			continue;
		snprintf(name, sizeof(name), "%s%s", prefix, entry->d_name);
		if (strv_has(live, name) || fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW))
			continue;
		if (!S_ISREG(st.st_mode) || !gc_old(gc, &st))
			continue;
		if (unlinkat(directory, entry->d_name, 0))
			continue;
		gc_count(gc, &st, counter);
	}
	closedir(stream);
}

static void gc_sweep_runtimes(struct gc *gc, struct storage_roots *roots)
{
	struct dirent *entry;
	struct stat st;
	char *path, *lock;
	const char *views = fy_sprintfa("%s/views", roots->path);
	DIR *stream = opendir(views);
	int fd;

	if (!stream)
		return;
	while ((entry = readdir(stream))) {
		if (strncmp(entry->d_name, "view-", 5) && strncmp(entry->d_name, "diff-", 5))
			continue;
		if (asprintf(&path, "%s/%s", views, entry->d_name) < 0)
			continue;
		if (strv_has(&roots->runtimes, path) || lstat(path, &st) || !S_ISDIR(st.st_mode) ||
		    !gc_old(gc, &st)) {
			free(path);
			continue;
		}
		/* A runtime that a command holds is in use: its lock is taken. */
		fd = -1;
		if (!strncmp(entry->d_name, "view-", 5)) {
			if (asprintf(&lock, "%s/lock", path) < 0) {
				free(path);
				continue;
			}
			fd = open(lock, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
			free(lock);
			if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) {
				close(fd);
				free(path);
				continue;
			}
		}
		if (!fyai_view_runtime_remove(path))
			gc->stats->runtimes++;
		else
			fyai_warning(gc->ctx, "gc: cannot remove %s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		free(path);
	}
	closedir(stream);
}

static void gc_sweep_storage(struct gc *gc, struct storage_roots *roots)
{
	struct strv blobs = { 0 };
	const char *path;
	bool complete;
	int lock, manifests = -1, objects = -1, borrowed = -1, rc;

	/* A storage that has an arena of its own may hold the references of that arena. */
	path = fy_sprintfa("%s/arena", roots->path);
	if (!access(path, F_OK) && !fyai_view_same_arena(gc->ctx, path)) {
		fyai_warning(gc->ctx, "gc: %s belongs to another arena; it was not collected",
			     roots->path);
		return;
	}
	path = fy_sprintfa("%s/" GC_LOCK_NAME, roots->path);
	lock = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0)
		return;
	rc = flock(lock, LOCK_EX | LOCK_NB);
	if (rc) {
		fyai_warning(gc->ctx, "gc: %s is in use; it was not collected", roots->path);
		close(lock);
		return;
	}
	/* The runtimes need no manifests, so they go first. */
	gc_sweep_runtimes(gc, roots);
	manifests = open(fy_sprintfa("%s/manifests", roots->path),
			 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (manifests < 0)
		goto out;
	if (gc_live(gc, roots, manifests, &blobs, &complete))
		goto out;
	gc_sweep_directory(gc, manifests, "", &roots->manifests, &gc->stats->manifests);
	if (!complete)
		goto out;
	objects = open(fy_sprintfa("%s/objects/blake3", roots->path),
		       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (objects < 0)
		goto out;
	gc_sweep_directory(gc, objects, "", &blobs, &gc->stats->objects);
	borrowed = openat(objects, "borrowed", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (borrowed >= 0)
		gc_sweep_directory(gc, borrowed, "borrowed/", &blobs, &gc->stats->objects);
out:
	if (manifests >= 0)
		close(manifests);
	if (objects >= 0)
		close(objects);
	if (borrowed >= 0)
		close(borrowed);
	strv_free(&blobs);
	close(lock);
}

int fyai_view_gc(struct fyai_ctx *ctx, unsigned int grace, struct fyai_view_gc_stats *stats)
{
	struct gc gc = { .ctx = ctx, .stats = stats };
	fy_generic entry;
	struct stat st;
	const char *key;
	char *project;
	size_t i;
	int rc = 0;

	memset(stats, 0, sizeof(*stats));
	gc.cutoff = time(NULL) - (time_t)grace;
	if (fy_is_mapping(ctx->arena_branches)) {
		fy_foreach_key_value(key, entry, ctx->arena_branches) {
			rc = gc_collect_branch(&gc, entry);
			if (rc)
				break;
		}
	}
	/* The storage next to the arena is collected even when nothing refers to it. */
	project = fyai_view_project_root(ctx);
	if (!rc && project) {
		key = fy_sprintfa("%s/.fyai", project);
		if (!stat(key, &st) && !gc_storage(&gc, key))
			rc = -1;
	}
	free(project);
	for (i = 0; i < gc.count && !rc; i++)
		gc_sweep_storage(&gc, &gc.storages[i]);
	for (i = 0; i < gc.count; i++) {
		free(gc.storages[i].path);
		strv_free(&gc.storages[i].manifests);
		strv_free(&gc.storages[i].runtimes);
	}
	free(gc.storages);
	if (rc)
		fyai_error(ctx, "gc: out of memory while collecting the project storage");
	return rc ? -1 : 0;
}
#else
int fyai_view_storage_lock_shared(const char *storage)
{
	(void)storage;
	return -1;
}

int fyai_view_gc(struct fyai_ctx *ctx, unsigned int grace, struct fyai_view_gc_stats *stats)
{
	(void)ctx;
	(void)grace;
	memset(stats, 0, sizeof(*stats));
	return 0;
}
#endif
