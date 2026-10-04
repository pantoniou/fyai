/*
 * fyai_view_apply.c - apply the result of a view to the host project
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libfyaml/libfyaml-blake3.h>

#include "fyai_view_apply.h"

/* The state of a path on the host, as far as a comparison needs it. */
struct host_state {
	bool present;
	/* The parent exists but is not a directory, or cannot be walked. */
	bool blocked;
	struct stat st;
};

struct apply {
	struct fy_generic_builder *gb;
	int project, objects;
	const struct fyai_manifest *baseline, *result;
	struct fy_blake3_hasher *hasher;
	fy_generic rows;
	struct fyai_apply_summary *summary;
	bool dry_run;
	unsigned int serial;
};

enum apply_action { APPLIED, SATISFIED, CONFLICT, SKIPPED };

static const char *const action_names[] = { "applied", "satisfied", "conflict", "skipped" };

static int apply_report(struct apply *apply, const char *path, const char *status,
			enum apply_action action, const char *reason)
{
	fy_generic row;

	switch (action) {
	case APPLIED:
		apply->summary->applied++;
		break;
	case SATISFIED:
		apply->summary->satisfied++;
		break;
	case CONFLICT:
		apply->summary->conflicts++;
		break;
	case SKIPPED:
		apply->summary->skipped++;
		break;
	}
	row = fy_mapping(apply->gb, "path", fy_value(apply->gb, path), "status", status, "action",
			 action_names[action], "reason", fy_value(apply->gb, reason ? reason : ""));
	apply->rows = fy_append(apply->gb, apply->rows, row);
	if (!fy_is_valid(apply->rows)) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

/* A path of a result is a relative path of plain names, outside .git and .fyai. */
static bool apply_path_safe(const char *path)
{
	const char *part = path, *end;
	size_t length;

	if (!*path || *path == '/')
		return false;
	for (; part; part = *end ? end + 1 : NULL) {
		end = strchr(part, '/');
		if (!end)
			end = part + strlen(part);
		length = (size_t)(end - part);
		if (!length || (length == 1 && *part == '.') ||
		    (length == 2 && !memcmp(part, "..", 2)))
			return false;
		if (part == path && ((length == 4 && !memcmp(part, ".git", 4)) ||
				     (length == 5 && !memcmp(part, ".fyai", 5))))
			return false;
	}
	return true;
}

static bool apply_selected(const char *path, const char *const *paths, size_t count)
{
	size_t i, length;

	if (!count)
		return true;
	for (i = 0; i < count; i++) {
		length = strlen(paths[i]);
		while (length > 1 && paths[i][length - 1] == '/')
			length--;
		if (!strncmp(path, paths[i], length) && (!path[length] || path[length] == '/'))
			return true;
	}
	return false;
}

/*
 * Open the directory that holds the path, one component at a time and with no
 * symlink followed. On return leaf names the last component. A missing
 * ancestor leaves *parent at -1 and sets *missing.
 */
static int apply_parent(int project, const char *path, int *parent, const char **leaf,
			bool *missing, bool *blocked)
{
	const char *part = path, *slash;
	char name[NAME_MAX + 1];
	size_t length;
	int directory, child;

	*missing = *blocked = false;
	directory = dup(project);
	if (directory < 0)
		return -1;
	while ((slash = strchr(part, '/'))) {
		length = (size_t)(slash - part);
		if (length > NAME_MAX) {
			close(directory);
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(name, part, length);
		name[length] = '\0';
		child = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(directory);
		if (child < 0) {
			if (errno == ENOENT)
				*missing = true;
			else if (errno == ENOTDIR || errno == ELOOP)
				*blocked = true;
			else
				return -1;
			*parent = -1;
			*leaf = slash + 1;
			return 0;
		}
		directory = child;
		part = slash + 1;
	}
	*parent = directory;
	*leaf = part;
	return 0;
}

static int apply_host(struct apply *apply, const char *path, struct host_state *host,
		      int *parent, const char **leaf)
{
	bool missing, blocked;
	int rc;

	memset(host, 0, sizeof(*host));
	rc = apply_parent(apply->project, path, parent, leaf, &missing, &blocked);
	if (rc)
		return -1;
	if (missing || blocked) {
		host->blocked = blocked;
		return 0;
	}
	rc = fstatat(*parent, *leaf, &host->st, AT_SYMLINK_NOFOLLOW);
	if (rc) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	host->present = true;
	return 0;
}

/* Hash a regular file with reads: the host can write it while this runs. */
static int apply_hash(struct apply *apply, int directory, const char *name,
		      unsigned char digest[FYAI_CAS_HASH_SIZE], uint64_t *size)
{
	unsigned char buffer[64 * 1024];
	const uint8_t *hash;
	ssize_t count;
	uint64_t total = 0;
	int fd, saved;

	fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	fy_blake3_hasher_reset(apply->hasher);
	for (;;) {
		count = read(fd, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		fy_blake3_hasher_update(apply->hasher, buffer, (size_t)count);
		total += (uint64_t)count;
	}
	saved = errno;
	close(fd);
	if (count < 0) {
		errno = saved;
		return -1;
	}
	hash = fy_blake3_hasher_finalize(apply->hasher);
	if (!hash) {
		errno = EIO;
		return -1;
	}
	memcpy(digest, hash, FYAI_CAS_HASH_SIZE);
	*size = total;
	return 0;
}

/*
 * Whether the host path is the object: the same kind and bytes, and for a file
 * the same execute bits. Time and ownership are not compared. A negative
 * result with errno set means the host could not be read.
 */
static int apply_equals(struct apply *apply, int parent, const char *leaf,
			const struct host_state *host, const struct fyai_mobject *object)
{
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	char target[PATH_MAX];
	uint64_t size;
	ssize_t length;

	if (!object)
		return !host->present;
	if (!host->present)
		return 0;
	switch (object->kind) {
	case FYAI_PROJECT_DIRECTORY:
		return S_ISDIR(host->st.st_mode);
	case FYAI_PROJECT_SYMLINK:
		if (!S_ISLNK(host->st.st_mode))
			return 0;
		length = readlinkat(parent, leaf, target, sizeof(target));
		if (length < 0)
			return -1;
		return (size_t)length == object->target_length &&
		       !memcmp(target, object->target, object->target_length);
	case FYAI_PROJECT_FILE:
		if (!S_ISREG(host->st.st_mode) || (uint64_t)host->st.st_size != object->blob.size ||
		    (host->st.st_mode & 0111 ? 1 : 0) != (object->meta.mode & 0111 ? 1 : 0))
			return 0;
		if (apply_hash(apply, parent, leaf, digest, &size))
			return -1;
		return size == object->blob.size &&
		       !memcmp(digest, object->blob.digest, sizeof(digest));
	}
	return 0;
}

static void apply_temp_name(struct apply *apply, char name[64])
{
	snprintf(name, 64, ".fyai-apply-%ld-%u", (long)getpid(), apply->serial++);
}

/* Write the object next to its destination, and rename it into place. */
static int apply_write(struct apply *apply, int parent, const char *leaf,
		       const struct fyai_mobject *object)
{
	char name[64], *target;
	int source = -1, file = -1, rc = -1, saved;

	apply_temp_name(apply, name);
	switch (object->kind) {
	case FYAI_PROJECT_DIRECTORY:
		return mkdirat(parent, leaf, object->meta.mode & 0777);
	case FYAI_PROJECT_SYMLINK:
		target = strndup((const char *)object->target, object->target_length);
		if (!target)
			return -1;
		rc = symlinkat(target, parent, name);
		free(target);
		break;
	case FYAI_PROJECT_FILE:
		source = fyai_cas_open(apply->objects, &object->blob);
		if (source < 0)
			return -1;
		file = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			      0600);
		if (file < 0)
			goto out;
		rc = fyai_cas_clone(source, file, object->blob.size);
		if (!rc)
			rc = fchmod(file, object->meta.mode & 07777);
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if (!rc)
		rc = renameat(parent, name, parent, leaf);
out:
	saved = errno;
	if (source >= 0)
		close(source);
	if (file >= 0)
		close(file);
	if (rc && object->kind != FYAI_PROJECT_DIRECTORY)
		unlinkat(parent, name, 0);
	errno = saved;
	return rc;
}

/* The parents that the result adds, made before the file that needs them. */
static int apply_parents(struct apply *apply, const char *path)
{
	struct fyai_mobject object, before;
	char *copy = strdup(path), *slash;
	int parent, rc = 0;
	const char *leaf;
	struct host_state host;

	if (!copy)
		return -1;
	for (slash = strchr(copy, '/'); slash && !rc; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		rc = apply_host(apply, copy, &host, &parent, &leaf);
		if (rc)
			break;
		if (!host.present && parent >= 0) {
			if (!fyai_manifest_lookup(apply->result, (const unsigned char *)copy,
						  strlen(copy), &object, NULL) ||
			    object.kind != FYAI_PROJECT_DIRECTORY ||
			    fyai_manifest_lookup(apply->baseline, (const unsigned char *)copy,
						 strlen(copy), &before, NULL)) {
				errno = ENOENT;
				rc = -1;
			} else if (!apply->dry_run) {
				rc = apply_write(apply, parent, leaf, &object);
			}
		} else if (host.present && !S_ISDIR(host.st.st_mode)) {
			errno = ENOTDIR;
			rc = -1;
		} else if (parent < 0 && !apply->dry_run) {
			errno = ENOENT;
			rc = -1;
		}
		if (parent >= 0)
			close(parent);
		*slash = '/';
	}
	free(copy);
	return rc;
}

static int apply_one(struct apply *apply, const char *path, const char *status, bool remove)
{
	struct fyai_mobject before, after;
	const struct fyai_mobject *old = NULL, *new = NULL;
	struct host_state host;
	const char *leaf;
	const char *reason = NULL;
	int parent = -1, equal_old, equal_new, rc, saved;

	if (fyai_manifest_lookup(apply->baseline, (const unsigned char *)path, strlen(path),
				 &before, NULL))
		old = &before;
	if (fyai_manifest_lookup(apply->result, (const unsigned char *)path, strlen(path), &after,
				 NULL))
		new = &after;
	/* Only the creation and the removal of a directory carry a change. */
	if (old && new && old->kind == FYAI_PROJECT_DIRECTORY &&
	    new->kind == FYAI_PROJECT_DIRECTORY)
		return apply_report(apply, path, status, SATISFIED,
				    "directory attributes are not applied");
	/* A directory that becomes a file goes after what is beneath it: the removal pass. */
	if (old && new && old->kind == FYAI_PROJECT_DIRECTORY && !remove)
		return 0;
	rc = apply_host(apply, path, &host, &parent, &leaf);
	if (rc) {
		saved = errno;
		rc = apply_report(apply, path, status, SKIPPED, strerror(saved));
		goto out;
	}
	if (host.blocked) {
		rc = apply_report(apply, path, status, CONFLICT, "a parent is not a directory");
		goto out;
	}
	/* Remove: only the removal pass reaches here with remove set. */
	if (remove) {
		if (parent < 0) {
			rc = apply_report(apply, path, status, SATISFIED, "already absent");
			goto out;
		}
		equal_old = apply_equals(apply, parent, leaf, &host, old);
		if (!host.present) {
			rc = apply_report(apply, path, status, SATISFIED, "already absent");
			goto out;
		}
		if (equal_old <= 0) {
			rc = apply_report(apply, path, status, CONFLICT,
					  equal_old < 0 ? strerror(errno) : "changed in the project");
			goto out;
		}
		if (!apply->dry_run &&
		    unlinkat(parent, leaf, old->kind == FYAI_PROJECT_DIRECTORY ? AT_REMOVEDIR : 0)) {
			saved = errno;
			/* A directory that is to become a file is a conflict, not a leftover. */
			rc = apply_report(apply, path, status,
					  saved == ENOTEMPTY && !new ? SKIPPED : CONFLICT,
					  saved == ENOTEMPTY ? "the directory holds other files" :
							       strerror(saved));
			goto out;
		}
		if (!apply->dry_run && new && apply_write(apply, parent, leaf, new)) {
			saved = errno;
			rc = apply_report(apply, path, status, SKIPPED, strerror(saved));
			goto out;
		}
		rc = apply_report(apply, path, status, APPLIED,
				  apply->dry_run ? (new ? "would replace" : "would remove") : "");
		goto out;
	}
	equal_new = parent < 0 ? 0 : apply_equals(apply, parent, leaf, &host, new);
	equal_old = parent < 0 ? !old : apply_equals(apply, parent, leaf, &host, old);
	if (equal_new < 0 || equal_old < 0) {
		saved = errno;
		rc = apply_report(apply, path, status, SKIPPED, strerror(saved));
		goto out;
	}
	if (parent >= 0 && equal_new) {
		rc = apply_report(apply, path, status, SATISFIED, "already in the project");
		goto out;
	}
	if (!equal_old) {
		reason = host.present ? "changed in the project" : "removed from the project";
		if (!old)
			reason = "exists in the project";
		rc = apply_report(apply, path, status, CONFLICT, reason);
		goto out;
	}
	if (!apply->dry_run) {
		if (parent < 0) {
			rc = apply_parents(apply, path);
			if (rc)
				goto fail;
			rc = apply_host(apply, path, &host, &parent, &leaf);
			if (rc)
				goto fail;
		}
		/* A file or a symlink that becomes a directory makes room first. */
		if (host.present && !S_ISDIR(host.st.st_mode) &&
		    new->kind == FYAI_PROJECT_DIRECTORY && unlinkat(parent, leaf, 0)) {
			rc = -1;
			goto fail;
		}
		rc = apply_write(apply, parent, leaf, new);
		if (rc)
			goto fail;
	}
	rc = apply_report(apply, path, status, APPLIED, apply->dry_run ? "would write" : "");
	goto out;
fail:
	saved = errno;
	rc = apply_report(apply, path, status, SKIPPED, strerror(saved));
out:
	saved = errno;
	if (parent >= 0)
		close(parent);
	errno = saved;
	return rc;
}

/*
 * A file that the result has at another path, with the same bytes, is a rename.
 * The project follows it with a rename when the old path still has the
 * baseline file and the new path is free. Return 1 when the rename is done,
 * or reported, and 0 when the paths are to be handled one at a time.
 */
static int apply_rename(struct apply *apply, const char *from, const char *to)
{
	struct fyai_mobject old, new;
	struct host_state source, target;
	const char *from_leaf, *to_leaf;
	char reason[PATH_MAX + 16];
	int from_parent = -1, to_parent = -1, ok = 0, rc = 0, saved;

	if (!fyai_manifest_lookup(apply->baseline, (const unsigned char *)from, strlen(from), &old,
				  NULL) ||
	    !fyai_manifest_lookup(apply->result, (const unsigned char *)to, strlen(to), &new, NULL))
		return 0;
	if (apply_host(apply, from, &source, &from_parent, &from_leaf))
		return 0;
	if (apply_host(apply, to, &target, &to_parent, &to_leaf))
		goto out;
	if (source.blocked || target.blocked || !source.present || target.present ||
	    from_parent < 0 || apply_equals(apply, from_parent, from_leaf, &source, &old) != 1)
		goto out;
	if (!apply->dry_run) {
		if (to_parent < 0) {
			if (apply_parents(apply, to))
				goto out;
			if (apply_host(apply, to, &target, &to_parent, &to_leaf) || to_parent < 0)
				goto out;
		}
		if (renameat(from_parent, from_leaf, to_parent, to_leaf))
			goto out;
		if ((old.meta.mode ^ new.meta.mode) & 0111 &&
		    fchmodat(to_parent, to_leaf, new.meta.mode & 07777, 0))
			goto out;
	}
	snprintf(reason, sizeof(reason), "%s from %s", apply->dry_run ? "would rename" : "renamed",
		 from);
	rc = apply_report(apply, to, "renamed", APPLIED, reason);
	ok = rc ? -1 : 1;
out:
	saved = errno;
	if (from_parent >= 0)
		close(from_parent);
	if (to_parent >= 0)
		close(to_parent);
	errno = saved;
	return ok;
}

/* One changed path, with the objects of both sides and the rename it belongs to. */
struct change {
	const char *path;
	const char *status;
	struct fyai_mobject before, after;
	bool has_before, has_after;
	/* The index of the other path of a rename, or -1. */
	long pair;
	/* The removed path of a rename that was done: nothing more to do. */
	bool done;
};

static int change_digest_order(const void *a, const void *b, void *arg)
{
	const struct change *list = arg;
	const struct change *x = &list[*(const size_t *)a], *y = &list[*(const size_t *)b];
	int order = memcmp(x->before.blob.digest, y->before.blob.digest, FYAI_CAS_HASH_SIZE);

	if (order)
		return order;
	return *(const size_t *)a < *(const size_t *)b ? -1 : *(const size_t *)a > *(const size_t *)b;
}

static const char *change_base(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

/*
 * Pair each added file with a deleted file of the same bytes. A file pairs only
 * with a file, and an empty file with none. Among several candidates the one with
 * the same name wins, else the first in path order, as section 10.1 says.
 */
static void changes_pair(struct change *list, size_t count)
{
	size_t *deleted, i, n = 0, low, high, mid, k, pick;
	int order;

	deleted = malloc((count + 1) * sizeof(*deleted));
	if (!deleted)
		return;
	for (i = 0; i < count; i++) {
		list[i].pair = -1;
		if (list[i].has_before && !list[i].has_after &&
		    list[i].before.kind == FYAI_PROJECT_FILE && list[i].before.blob.size)
			deleted[n++] = i;
	}
	qsort_r(deleted, n, sizeof(*deleted), change_digest_order, list);
	for (i = 0; i < count; i++) {
		if (list[i].has_before || !list[i].has_after || list[i].after.kind != FYAI_PROJECT_FILE ||
		    !list[i].after.blob.size)
			continue;
		low = 0;
		high = n;
		while (low < high) {
			mid = (low + high) / 2;
			order = memcmp(list[deleted[mid]].before.blob.digest, list[i].after.blob.digest,
				       FYAI_CAS_HASH_SIZE);
			if (order < 0)
				low = mid + 1;
			else
				high = mid;
		}
		pick = n;
		for (k = low; k < n && !memcmp(list[deleted[k]].before.blob.digest,
					       list[i].after.blob.digest, FYAI_CAS_HASH_SIZE);
		     k++) {
			if (list[deleted[k]].pair >= 0)
				continue;
			if (pick == n)
				pick = k;
			if (!strcmp(change_base(list[deleted[k]].path), change_base(list[i].path))) {
				pick = k;
				break;
			}
		}
		if (pick == n)
			continue;
		list[i].pair = (long)deleted[pick];
		list[deleted[pick]].pair = (long)i;
	}
	free(deleted);
}

int fyai_view_apply(struct fy_generic_builder *gb, int project_fd, int objects_fd,
		    const struct fyai_manifest *baseline, const struct fyai_manifest *result,
		    const char *const *paths, size_t path_count, bool dry_run, fy_generic *rows,
		    struct fyai_apply_summary *summary)
{
	struct fy_blake3_hasher_cfg cfg = { 0 };
	struct apply apply = { .gb = gb,
			       .project = project_fd,
			       .objects = objects_fd,
			       .baseline = baseline,
			       .result = result,
			       .dry_run = dry_run,
			       .summary = summary };
	struct change *list = NULL, *c;
	fy_generic changes, row;
	const char *path, *hex, *status;
	size_t count, i, used = 0, removals = 0, *removal = NULL;
	int rc = -1, saved, done;

	memset(summary, 0, sizeof(*summary));
	apply.rows = fy_sequence(gb);
	apply.hasher = fy_blake3_hasher_create(&cfg);
	if (!apply.hasher) {
		errno = ENOMEM;
		return -1;
	}
	changes = fyai_manifest_diff(gb, baseline, result);
	if (!fy_is_sequence(changes))
		goto out;
	count = fy_len(changes);
	list = calloc(count + 1, sizeof(*list));
	removal = calloc(count + 1, sizeof(*removal));
	if (!list || !removal) {
		errno = ENOMEM;
		goto out;
	}
	fy_foreach(row, changes) {
		path = fy_get(row, "path", "");
		hex = fy_get(row, "path_hex", "");
		status = fy_get(row, "status", "");
		if (!strcmp(path, ".") || !apply_selected(path, paths, path_count))
			continue;
		if (!apply_path_safe(path) || strlen(hex) != 2 * strlen(path)) {
			if (apply_report(&apply, path, status, SKIPPED, "the path is not a plain name"))
				goto out;
			continue;
		}
		c = &list[used++];
		c->path = path;
		c->status = status;
		c->pair = -1;
		c->has_before = fyai_manifest_lookup(baseline, (const unsigned char *)path,
						     strlen(path), &c->before, NULL);
		c->has_after = fyai_manifest_lookup(result, (const unsigned char *)path,
						    strlen(path), &c->after, NULL);
	}
	changes_pair(list, used);
	for (i = 0; i < used; i++) {
		c = &list[i];
		if (c->done)
			continue;
		if (c->pair >= 0 && c->has_after) {
			done = apply_rename(&apply, list[c->pair].path, c->path);
			if (done < 0)
				goto out;
			if (done) {
				list[c->pair].done = true;
				continue;
			}
		}
		/* A removal waits for the rest: what is beneath a directory goes before it. */
		if (!c->has_after || (c->has_before && c->before.kind == FYAI_PROJECT_DIRECTORY &&
				      c->after.kind != FYAI_PROJECT_DIRECTORY)) {
			removal[removals++] = i;
			continue;
		}
		if (apply_one(&apply, c->path, c->status, false))
			goto out;
	}
	while (removals) {
		c = &list[removal[--removals]];
		if (c->done)
			continue;
		if (apply_one(&apply, c->path, c->has_after ? "modified" : "deleted", true))
			goto out;
	}
	*rows = apply.rows;
	rc = 0;
out:
	saved = errno;
	free(list);
	free(removal);
	fy_blake3_hasher_destroy(apply.hasher);
	errno = saved;
	return rc;
}
