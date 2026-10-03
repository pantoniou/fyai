/*
 * fyai_project_capture.c - capture a host project tree into manifests and CAS
 * blobs
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include "fyai_project_capture.h"

#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#include <sched.h>
#include <stdatomic.h>
#include <libfyaml/libfyaml-thread.h>
#include <libfyaml/libfyaml-blake3.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <unistd.h>

struct capture_node {
	char *path;
	char *link;
	fy_generic baseline;
	bool reused;
	bool materialized;
	bool link_blob;
	struct stat before;
	struct fyai_project_entry entry;
	struct fyai_cas_blob blob;
	size_t *children;
	size_t count;
	size_t capacity;
	int error;
};

struct project_capture {
	struct fy_generic_builder *gb;
	const struct fyai_project_capture_opts *opts;
	struct capture_node *nodes;
	struct fy_thread_pool *pool;
	struct fyai_cas_copy_state copy;
	atomic_size_t next;
	dev_t device;
	size_t count;
	size_t capacity;
	char *error;
	size_t error_size;
};

struct capture_worker {
	struct project_capture *capture;
	struct fy_blake3_hasher *hasher;
};

static bool capture_same(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
	       a->st_uid == b->st_uid && a->st_gid == b->st_gid && a->st_size == b->st_size &&
	       a->st_nlink == b->st_nlink && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
	       a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
	       a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static bool capture_node_same(const struct capture_node *node, const struct stat *after)
{
	struct stat observed = *after;

	if (node->blob.borrowed) {
		observed.st_nlink = node->before.st_nlink;
		observed.st_ctim = node->before.st_ctim;
	}
	return capture_same(&node->before, &observed);
}

static struct fyai_project_metadata capture_metadata(struct project_capture *capture,
						     const struct stat *st)
{
	struct fyai_project_metadata metadata = {
		.mode = st->st_mode & 07777,
		.uid = st->st_uid,
		.gid = st->st_gid,
		.mtime_sec = st->st_mtim.tv_sec,
		.mtime_nsec = st->st_mtim.tv_nsec,
	};

	if (capture->opts->mapped_owner) {
		if (!st->st_uid)
			metadata.uid = capture->opts->host_uid;
		if (!st->st_gid)
			metadata.gid = capture->opts->host_gid;
	}
	return metadata;
}

static int capture_xattrs(struct project_capture *capture, int fd)
{
	char *names, *name;
	ssize_t length, actual;
	int saved;

	length = flistxattr(fd, NULL, 0);
	if (length <= 0)
		return length < 0 ? -1 : 0;
	if (!capture->opts->mapped_owner) {
		errno = ENOTSUP;
		return -1;
	}
	names = malloc(length);
	if (!names)
		return -1;
	actual = flistxattr(fd, names, length);
	if (actual < 0) {
		saved = errno;
		free(names);
		errno = saved;
		return -1;
	}
	for (name = names; name < names + actual; name += strlen(name) + 1) {
		if (strncmp(name, "user.overlay.", 13)) {
			free(names);
			errno = ENOTSUP;
			return -1;
		}
	}
	free(names);
	return 0;
}

static int capture_apply(int fd, const struct stat *st)
{
	struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, st->st_mtim };
	int rc;

	rc = fchown(fd, st->st_uid, st->st_gid);
	if (!rc)
		rc = fchmod(fd, st->st_mode & 07777);
	if (!rc)
		rc = futimens(fd, times);
	if (!rc)
		rc = fsync(fd);
	return rc;
}

static int capture_open(int root, const char *path, int flags, unsigned int mode)
{
	struct open_how how = { .flags = flags | O_NOFOLLOW | O_CLOEXEC,
				.mode = mode,
				.resolve =
					RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV };

	return syscall(SYS_openat2, root, *path ? path : ".", &how, sizeof(how));
}

static int capture_add(struct project_capture *capture, size_t parent, const char *name,
		       const struct stat *st, size_t *index)
{
	struct capture_node *nodes, *node;
	char *path;
	size_t capacity, *children;
	int rc;

	if (capture->count >= 100001) {
		errno = E2BIG;
		return -1;
	}
	if (st->st_dev != capture->device) {
		errno = EXDEV;
		return -1;
	}
	if (parent == SIZE_MAX)
		path = strdup("");
	else {
		rc = asprintf(&path, "%s%s%s", capture->nodes[parent].path,
			      *capture->nodes[parent].path ? "/" : "", name);
		if (rc < 0)
			return -1;
	}
	if (!path)
		return -1;
	if (strlen(path) >= PATH_MAX) {
		free(path);
		errno = ENAMETOOLONG;
		return -1;
	}
	if (capture->count == capture->capacity) {
		capacity = capture->capacity ? capture->capacity * 2 : 64;
		nodes = realloc(capture->nodes, capacity * sizeof(*nodes));
		if (!nodes) {
			free(path);
			return -1;
		}
		capture->nodes = nodes;
		capture->capacity = capacity;
	}
	*index = capture->count++;
	node = &capture->nodes[*index];
	memset(node, 0, sizeof(*node));
	node->baseline = fy_invalid;
	node->path = path;
	node->before = *st;
	node->entry.name =
		(const unsigned char *)(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	node->entry.name_length = strlen((const char *)node->entry.name);
	if (S_ISREG(st->st_mode)) {
		if (st->st_mode & 06000) {
			errno = ENOTSUP;
			return -1;
		}
		node->entry.kind = FYAI_PROJECT_FILE;
		node->blob.borrowed =
			capture->opts->borrow_git && !strncmp(path, ".git/objects/", 13);
	} else if (S_ISDIR(st->st_mode))
		node->entry.kind = FYAI_PROJECT_DIRECTORY;
	else if (S_ISLNK(st->st_mode))
		node->entry.kind = FYAI_PROJECT_SYMLINK;
	else {
		errno = ENOTSUP;
		return -1;
	}
	if (parent != SIZE_MAX) {
		node = &capture->nodes[parent];
		if (node->count == node->capacity) {
			capacity = node->capacity ? node->capacity * 2 : 16;
			children = realloc(node->children, capacity * sizeof(*children));
			if (!children)
				return -1;
			node->children = children;
			node->capacity = capacity;
		}
		node->children[node->count++] = *index;
	}
	return 0;
}

/* Directory entries are sorted by filename bytes, equivalently by their hex
 * encoding. */
static fy_generic capture_baseline_child(struct project_capture *capture, size_t parent,
					 const char *name, char digest[FYAI_CAS_DIGEST_SIZE])
{
	static const char hex[] = "0123456789abcdef";
	fy_generic entries, entry, value;
	char encoded[NAME_MAX * 2 + 1];
	const char *stored;
	size_t i, length = strlen(name), low = 0, high, middle;
	int cmp;

	if (!capture->opts->incremental || length > NAME_MAX)
		return fy_invalid;
	entries = fy_get(capture->nodes[parent].baseline, "entries", fy_invalid);
	if (!fy_is_sequence(entries))
		return fy_invalid;
	for (i = 0; i < length; i++) {
		encoded[i * 2] = hex[(unsigned char)name[i] >> 4];
		encoded[i * 2 + 1] = hex[(unsigned char)name[i] & 15];
	}
	encoded[length * 2] = '\0';
	high = fy_len(entries);
	while (low < high) {
		middle = low + (high - low) / 2;
		entry = fy_get_at(entries, middle);
		stored = fy_get(entry, "name_hex", "");
		cmp = strcmp(encoded, stored);
		if (cmp < 0)
			high = middle;
		else if (cmp > 0)
			low = middle + 1;
		else {
			stored = fy_get(entry, "digest", "");
			if (strlen(stored) != FYAI_CAS_DIGEST_SIZE - 1)
				return fy_invalid;
			memcpy(digest, stored, FYAI_CAS_DIGEST_SIZE);
			value = fy_get(capture->opts->snapshot, "objects", fy_invalid);
			return fy_get(value, stored, fy_invalid);
		}
	}
	return fy_invalid;
}

static int capture_scan(struct project_capture *capture, size_t index, unsigned int depth)
{
	struct stat before, after, upper;
	struct dirent *dent;
	DIR *directory = NULL;
	char link[PATH_MAX];
	const char *path = capture->nodes[index].path;
	size_t child;
	ssize_t length;
	int source = -1, target = -1, rc = -1, saved;

	if (depth > 128) {
		errno = ELOOP;
		goto out;
	}
	source = capture_open(capture->opts->source_fd, path, O_RDONLY | O_DIRECTORY, 0);
	if (source < 0)
		goto out;
	rc = fstat(source, &before);
	if (rc)
		goto out;
	if (!capture_same(&capture->nodes[index].before, &before)) {
		errno = EAGAIN;
		rc = -1;
		goto out;
	}
	rc = capture_xattrs(capture, source);
	if (rc)
		goto out;
	if (capture->opts->baseline_fd >= 0) {
		target = capture_open(capture->opts->baseline_fd, path, O_RDONLY | O_DIRECTORY, 0);
		if (target < 0) {
			rc = -1;
			goto out;
		}
	}
	directory = fdopendir(source);
	if (!directory) {
		rc = -1;
		goto out;
	}
	for (;;) {
		errno = 0;
		dent = readdir(directory);
		if (!dent) {
			rc = errno ? -1 : 0;
			break;
		}
		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..") ||
		    !strcmp(dent->d_name, ".fyai"))
			continue;
		rc = fstatat(source, dent->d_name, &before, AT_SYMLINK_NOFOLLOW);
		if (rc)
			goto out;
		rc = capture_add(capture, index, dent->d_name, &before, &child);
		if (rc)
			goto out;
		capture->nodes[child].baseline = capture_baseline_child(
			capture, index, dent->d_name, capture->nodes[child].entry.digest);
		if (capture->opts->incremental) {
			rc = fstatat(capture->opts->upper_fd, capture->nodes[child].path, &upper,
				     AT_SYMLINK_NOFOLLOW);
			if (rc && errno != ENOENT && errno != ENOTDIR)
				goto out;
			if (rc && fy_is_mapping(capture->nodes[child].baseline)) {
				capture->nodes[child].reused = true;
				continue;
			}
		}
		if (S_ISDIR(before.st_mode)) {
			if (target >= 0) {
				rc = mkdirat(target, dent->d_name, 0700);
				if (rc)
					goto out;
			}
			rc = capture_scan(capture, child, depth + 1);
			if (rc)
				goto out;
		} else if (S_ISLNK(before.st_mode)) {
			length = readlinkat(source, dent->d_name, link, sizeof(link));
			if (length < 0 || (size_t)length == sizeof(link)) {
				if (length >= 0)
					errno = ENAMETOOLONG;
				rc = -1;
				goto out;
			}
			link[length] = '\0';
			capture->nodes[child].link = strdup(link);
			if (!capture->nodes[child].link) {
				rc = -1;
				goto out;
			}
		}
	}
	if (!rc) {
		rc = fstat(source, &after);
		if (!rc && !capture_same(&capture->nodes[index].before, &after)) {
			errno = EAGAIN;
			rc = -1;
		}
	}
out:
	saved = errno;
	if (directory)
		closedir(directory);
	else if (source >= 0)
		close(source);
	if (target >= 0)
		close(target);
	if (rc && capture->error && capture->error_size && !capture->error[0])
		snprintf(capture->error, capture->error_size, "%s", *path ? path : ".");
	errno = saved;
	return rc;
}

static int capture_owned_file(struct capture_worker *worker, struct capture_node *node, int source)
{
	struct project_capture *capture = worker->capture;
	struct stat previous;
	int target, old = -1, rc, saved;

	if (capture->opts->reuse_baseline && strncmp(node->path, ".git/objects/", 13))
		old = capture_open(capture->opts->previous_baseline_fd, node->path, O_RDONLY, 0);
	if (old >= 0) {
		rc = fstat(old, &previous);
		if (!rc && previous.st_size == node->before.st_size &&
		    (previous.st_mode & 07777) == (node->before.st_mode & 07777) &&
		    previous.st_uid == node->before.st_uid &&
		    previous.st_gid == node->before.st_gid &&
		    previous.st_mtim.tv_sec == node->before.st_mtim.tv_sec &&
		    previous.st_mtim.tv_nsec == node->before.st_mtim.tv_nsec) {
			/* Capture mutable source bytes before comparing with an
			 * immutable lower. */
			rc = fyai_cas_put_hasher_state(capture->opts->objects_fd, source,
						       &node->blob, worker->hasher, &capture->copy);
			if (!rc)
				rc = fyai_cas_verify_file(old, &node->blob, worker->hasher);
			if (!rc)
				rc = linkat(capture->opts->previous_baseline_fd, node->path,
					    capture->opts->baseline_fd, node->path, 0);
			close(old);
			if (!rc) {
				node->materialized = true;
				return 0;
			}
			if (errno != EIO)
				return -1;
			if (lseek(source, 0, SEEK_SET) < 0)
				return -1;
		} else
			close(old);
	}
	target = capture_open(capture->opts->baseline_fd, node->path, O_RDWR | O_CREAT | O_EXCL,
			      0600);
	if (target < 0)
		return -1;
	rc = fyai_cas_clone_state(source, target, node->before.st_size, &capture->copy);
	if (!rc)
		rc = capture_apply(target, &node->before);
	if (!rc && lseek(target, 0, SEEK_SET) < 0)
		rc = -1;
	if (!rc)
		rc = fyai_cas_hash_file(target, &node->blob, worker->hasher);
	saved = errno;
	if (close(target) < 0 && !rc) {
		rc = -1;
		saved = errno;
	}
	errno = saved;
	if (!rc)
		rc = fyai_cas_link(capture->opts->objects_fd, capture->opts->baseline_fd,
				   node->path, &node->blob, worker->hasher);
	if (!rc)
		node->materialized = true;
	return rc;
}

static int capture_inode_compare(const void *a, const void *b)
{
	const struct capture_node *const *left = a, *const *right = b;

	if ((*left)->before.st_dev != (*right)->before.st_dev)
		return ((*left)->before.st_dev > (*right)->before.st_dev) ? 1 : -1;
	return ((*left)->before.st_ino > (*right)->before.st_ino) -
	       ((*left)->before.st_ino < (*right)->before.st_ino);
}

static int capture_links(struct project_capture *capture)
{
	struct capture_node **files;
	size_t i, count = 0;
	int rc = 0;

	files = malloc(capture->count * sizeof(*files));
	if (!files)
		return -1;
	for (i = 0; i < capture->count; i++)
		if (capture->nodes[i].entry.kind == FYAI_PROJECT_FILE)
			files[count++] = &capture->nodes[i];
	qsort(files, count, sizeof(*files), capture_inode_compare);
	for (i = 1; i < count; i++) {
		if (!capture_inode_compare(&files[i - 1], &files[i]) &&
		    (strncmp(files[i - 1]->path, ".git/objects/", 13) ||
		     strncmp(files[i]->path, ".git/objects/", 13))) {
			if (capture->error && capture->error_size)
				snprintf(capture->error, capture->error_size, "%s", files[i]->path);
			errno = ENOTSUP;
			rc = -1;
			break;
		}
	}
	free(files);
	return rc;
}

static void capture_files(void *arg)
{
	struct capture_worker *worker = arg;
	struct project_capture *capture = worker->capture;
	struct capture_node *node;
	struct stat after;
	size_t index;
	int source, object, rc;

	for (;;) {
		index = atomic_fetch_add_explicit(&capture->next, 1, memory_order_relaxed);
		if (index >= capture->count)
			break;
		node = &capture->nodes[index];
		if (node->reused || node->entry.kind != FYAI_PROJECT_FILE)
			continue;
		source = capture_open(capture->opts->source_fd, node->path, O_RDONLY | O_NONBLOCK,
				      0);
		if (source < 0) {
			node->error = errno;
			continue;
		}
		rc = fstat(source, &after);
		if (!rc && !capture_node_same(node, &after)) {
			errno = EAGAIN;
			rc = -1;
		}
		if (!rc)
			rc = capture_xattrs(capture, source);
		if (!rc && capture->opts->borrow_git && !strncmp(node->path, ".git/objects/", 13)) {
			node->blob.borrowed = true;
			rc = fyai_cas_hash_file(source, &node->blob, worker->hasher);
			if (!rc)
				rc = fyai_cas_link(capture->opts->objects_fd,
						   capture->opts->source_fd, node->path,
						   &node->blob, worker->hasher);
			if (!rc) {
				object = fyai_cas_open(capture->opts->objects_fd, &node->blob);
				if (object < 0)
					rc = -1;
				else {
					rc = fstat(object, &after);
					close(object);
					if (!rc) {
						node->blob.source_device = after.st_dev;
						node->blob.source_inode = after.st_ino;
						node->blob.source_mode = after.st_mode & 07777;
						node->blob.source_uid = after.st_uid;
						node->blob.source_gid = after.st_gid;
					}
				}
			}
			/* Publication adds a link and changes ctime, but not
			 * project metadata. */
			if (!rc)
				rc = fstat(source, &after);
			if (!rc) {
				node->before.st_nlink = after.st_nlink;
				node->before.st_ctim = after.st_ctim;
			}
		} else if (!rc && capture->opts->baseline_fd >= 0 && !capture->opts->metacopy)
			rc = capture_owned_file(worker, node, source);
		else if (!rc)
			rc = fyai_cas_put_hasher_state(capture->opts->objects_fd, source,
						       &node->blob, worker->hasher, &capture->copy);

		if (!rc) {
			rc = fstat(source, &after);
			if (!rc && !capture_node_same(node, &after)) {
				errno = EAGAIN;
				rc = -1;
			}
		}
		if (rc)
			node->error = errno ? errno : EIO;
		close(source);
	}
}

static int capture_materialize_file(struct project_capture *capture, struct capture_node *node)
{
	struct stat object_stat;
	char redirect[sizeof(node->blob.digest) + 3], object_name[sizeof(node->blob.digest) + 9];
	int object = -1, target = -1, rc = -1, saved;

	object = fyai_cas_open(capture->opts->objects_fd, &node->blob);
	if (object < 0)
		goto out;
	rc = fstat(object, &object_stat);
	if (rc)
		goto out;
	if (!S_ISREG(object_stat.st_mode) || (uint64_t)object_stat.st_size != node->blob.size) {
		errno = EINVAL;
		rc = -1;
		goto out;
	}
	if (node->link_blob && !capture->opts->metacopy &&
	    (object_stat.st_mode & 07777) == (node->before.st_mode & 07777) &&
	    object_stat.st_uid == node->before.st_uid &&
	    object_stat.st_gid == node->before.st_gid &&
	    object_stat.st_mtim.tv_sec == node->before.st_mtim.tv_sec &&
	    object_stat.st_mtim.tv_nsec == node->before.st_mtim.tv_nsec) {
		snprintf(object_name, sizeof(object_name), "borrowed/%s", node->blob.digest);
		rc = linkat(capture->opts->objects_fd, object_name, capture->opts->baseline_fd,
			    node->path, 0);
		goto out;
	}
	target = capture_open(capture->opts->baseline_fd, node->path, O_RDWR | O_CREAT | O_EXCL,
			      0600);
	if (target < 0) {
		rc = -1;
		goto out;
	}
	if (capture->opts->metacopy) {
		snprintf(object_name, sizeof(object_name), "%s%s",
			 node->blob.borrowed ? "borrowed/" : "", node->blob.digest);
		snprintf(redirect, sizeof(redirect), "/%s%s", node->blob.borrowed ? "b-" : "",
			 node->blob.digest);
		rc = linkat(capture->opts->objects_fd, object_name, capture->opts->data_fd,
			    redirect + 1, 0);
		if (rc && errno == EEXIST)
			rc = 0;
		if (!rc)
			rc = ftruncate(target, node->blob.size);

		if (!rc)
			rc = fsetxattr(target, "user.overlay.metacopy", "", 0, XATTR_CREATE);
		if (!rc)
			rc = fsetxattr(target, "user.overlay.redirect", redirect, strlen(redirect),
				       XATTR_CREATE);
	} else
		rc = fyai_cas_clone_state(object, target, node->blob.size, &capture->copy);
	if (!rc)
		rc = capture_apply(target, &node->before);
out:
	saved = errno;
	if (target >= 0)
		close(target);
	if (object >= 0)
		close(object);
	errno = saved;
	return rc;
}

static void capture_materialize_files(void *arg)
{
	struct capture_worker *worker = arg;
	struct project_capture *capture = worker->capture;
	struct capture_node *node;
	size_t index;
	int rc;

	for (;;) {
		index = atomic_fetch_add_explicit(&capture->next, 1, memory_order_relaxed);
		if (index >= capture->count)
			break;
		node = &capture->nodes[index];
		if (node->reused || node->entry.kind != FYAI_PROJECT_FILE || node->error ||
		    node->materialized)
			continue;
		rc = capture_materialize_file(capture, node);
		if (rc)
			node->error = errno ? errno : EIO;
	}
}

static int capture_compare_source(int source, int object, uint64_t size, unsigned char *buffer)
{
	const unsigned char *mapped;
	size_t offset = 0, amount;
	ssize_t count;
	int rc = -1, saved;

	if (!size)
		return 0;
	mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, object, 0);
	if (mapped == MAP_FAILED)
		return -1;
	while (offset < size) {
		amount = size - offset > (1U << 20) ? (1U << 20) : (size_t)(size - offset);
		count = pread(source, buffer, amount, offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0 || memcmp(buffer, mapped + offset, count > 0 ? (size_t)count : 0)) {
			if (count >= 0)
				errno = EAGAIN;
			goto out;
		}
		offset += (size_t)count;
	}
	rc = 0;
out:
	saved = errno;
	munmap((void *)mapped, size);
	errno = saved;
	return rc;
}

static int capture_verify(struct project_capture *capture)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	struct capture_node *node;
	struct stat after;
	unsigned char *buffer;
	size_t i;
	struct stat metadata;
	char redirect[sizeof(node->blob.digest) + 3], actual[sizeof(redirect)];
	ssize_t length;
	int source = -1, object = -1, baseline = -1, data = -1, rc = -1, saved;

	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		return -1;
	}
	buffer = malloc(1U << 20);
	if (!buffer)
		goto out;
	for (i = 0; i < capture->count; i++) {
		node = &capture->nodes[i];
		if (node->error) {
			errno = node->error;
			goto failed;
		}
		if (node->reused || node->entry.kind != FYAI_PROJECT_FILE)
			continue;
		source = capture_open(capture->opts->source_fd, node->path, O_RDONLY | O_NONBLOCK,
				      0);
		if (source < 0)
			goto failed;
		rc = fstat(source, &after);
		if (rc)
			goto failed;
		if (!capture_node_same(node, &after)) {
			errno = EAGAIN;
			goto failed;
		}
		rc = fyai_cas_verify_hasher(capture->opts->objects_fd, &node->blob, hasher);
		if (rc)
			goto failed;
		object = fyai_cas_open(capture->opts->objects_fd, &node->blob);
		if (object < 0)
			goto failed;
		rc = capture_compare_source(source, object, node->blob.size, buffer);
		if (rc)
			goto failed;
		if (capture->opts->baseline_fd >= 0) {
			baseline =
				capture_open(capture->opts->baseline_fd, node->path, O_RDONLY, 0);
			if (baseline < 0)
				goto failed;
			if (capture->opts->metacopy) {
				snprintf(redirect, sizeof(redirect), "/%s%s",
					 node->blob.borrowed ? "b-" : "", node->blob.digest);
				rc = fstat(baseline, &metadata);
				if (!rc &&
				    (metadata.st_size != (off_t)node->blob.size ||
				     (metadata.st_mode & 07777) != (node->before.st_mode & 07777) ||
				     metadata.st_uid != node->before.st_uid ||
				     metadata.st_gid != node->before.st_gid ||
				     metadata.st_mtim.tv_sec != node->before.st_mtim.tv_sec ||
				     metadata.st_mtim.tv_nsec != node->before.st_mtim.tv_nsec)) {
					errno = EIO;
					rc = -1;
				}
				length = fgetxattr(baseline, "user.overlay.redirect", actual,
						   sizeof(actual));
				if (!rc && (length != (ssize_t)strlen(redirect) ||
					    memcmp(actual, redirect, length))) {
					errno = EIO;
					rc = -1;
				}
				if (!rc &&
				    fgetxattr(baseline, "user.overlay.metacopy", NULL, 0) != 0) {
					errno = EIO;
					rc = -1;
				}
				if (!rc) {
					data = openat(capture->opts->data_fd, redirect + 1,
						      O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
					if (data < 0)
						rc = -1;
					else {
						rc = fyai_cas_verify_file(data, &node->blob,
									  hasher);
						close(data);
						data = -1;
					}
				}
			} else
				rc = fyai_cas_verify_file(baseline, &node->blob, hasher);
			if (rc)
				goto failed;
			close(baseline);
			baseline = -1;
		}
		rc = fstat(source, &after);
		if (rc)
			goto failed;
		if (!capture_node_same(node, &after)) {
			errno = EAGAIN;
			goto failed;
		}
		close(source);
		close(object);
		source = object = -1;
	}
	rc = 0;
	goto out;
failed:
	rc = -1;
	if (capture->error && capture->error_size)
		snprintf(capture->error, capture->error_size, "%s", node->path);
out:
	saved = errno;
	if (source >= 0)
		close(source);
	if (object >= 0)
		close(object);
	if (baseline >= 0)
		close(baseline);
	free(buffer);
	fy_blake3_hasher_destroy(hasher);
	errno = saved;
	return rc;
}

static fy_generic capture_finish(struct project_capture *capture, size_t index)
{
	struct capture_node *node = &capture->nodes[index];
	struct fyai_project_metadata metadata = capture_metadata(capture, &node->before);
	struct fyai_project_entry *entries = NULL;
	struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, node->before.st_mtim };
	struct stat after;
	fy_generic result = fy_invalid;
	char *parent = NULL, *slash;
	const char *name;
	size_t i;
	int source = -1, target = -1, rc, saved;

	if (node->reused)
		return node->baseline;
	if (node->error) {
		errno = node->error;
		goto out;
	}
	if (node->entry.kind == FYAI_PROJECT_SYMLINK) {
		parent = strdup(node->path);
		if (!parent)
			goto out;
		slash = strrchr(parent, '/');
		if (slash) {
			*slash = '\0';
			name = slash + 1;
		} else {
			name = node->path;
			*parent = '\0';
		}
		source = capture_open(capture->opts->source_fd, parent, O_RDONLY | O_DIRECTORY, 0);
		if (source < 0)
			goto out;
		rc = fstatat(source, name, &after, AT_SYMLINK_NOFOLLOW);
	} else {
		source = capture_open(
			capture->opts->source_fd, node->path,
			O_RDONLY | O_NONBLOCK |
				(node->entry.kind == FYAI_PROJECT_DIRECTORY ? O_DIRECTORY : 0),
			0);
		if (source < 0)
			goto out;
		rc = fstat(source, &after);
	}
	if (rc)
		goto out;
	if (!capture_node_same(node, &after)) {
		errno = EAGAIN;
		goto out;
	}
	if (node->entry.kind == FYAI_PROJECT_FILE)
		result = fyai_project_file(capture->gb, &metadata, &node->blob, node->entry.digest);
	else if (node->entry.kind == FYAI_PROJECT_SYMLINK) {
		result = fyai_project_symlink(capture->gb, &metadata,
					      (const unsigned char *)node->link, strlen(node->link),
					      node->entry.digest);
		if (!fy_is_valid(result))
			goto out;
		if (capture->opts->baseline_fd >= 0) {
			target = capture_open(capture->opts->baseline_fd, parent,
					      O_RDONLY | O_DIRECTORY, 0);
			if (target < 0)
				goto invalid;
			rc = symlinkat(node->link, target, name);
			if (!rc)
				rc = fchownat(target, name, node->before.st_uid,
					      node->before.st_gid, AT_SYMLINK_NOFOLLOW);
			if (!rc)
				rc = utimensat(target, name, times, AT_SYMLINK_NOFOLLOW);
			if (rc)
				goto invalid;
		}
	} else {
		if (node->count) {
			entries = calloc(node->count, sizeof(*entries));
			if (!entries)
				goto out;
			for (i = 0; i < node->count; i++)
				entries[i] = capture->nodes[node->children[i]].entry;
		}
		if (capture->opts->baseline_fd >= 0) {
			target = capture_open(capture->opts->baseline_fd, node->path,
					      O_RDONLY | O_DIRECTORY, 0);
			if (target < 0)
				goto out;
			if (!index) {
				rc = mkdirat(target, ".fyai", 0700);
				if (rc)
					goto out;
			}
			rc = capture_apply(target, &node->before);
			if (rc)
				goto out;
		}
		result = fyai_project_directory(capture->gb, &metadata, entries, node->count,
						!index, node->entry.digest);
	}
	goto out;
invalid:
	result = fy_invalid;
out:
	saved = errno;
	free(entries);
	free(parent);
	if (source >= 0)
		close(source);
	if (target >= 0)
		close(target);
	if (!fy_is_valid(result) && capture->error && capture->error_size && !capture->error[0])
		snprintf(capture->error, capture->error_size, "%s", *node->path ? node->path : ".");
	errno = saved;
	return result;
}

static int capture_reused_objects(struct project_capture *capture, const char *digest,
				  fy_generic *objects, unsigned int depth, size_t *count)
{
	fy_generic source, object, entries, entry;
	const char *child;
	int rc;

	if (depth > 128 || ++*count > 100001) {
		errno = E2BIG;
		return -1;
	}
	if (fy_is_valid(fy_get(*objects, digest, fy_invalid)))
		return 0;
	source = fy_get(capture->opts->snapshot, "objects", fy_invalid);
	object = fy_get(source, digest, fy_invalid);
	if (!fy_is_mapping(object)) {
		errno = EINVAL;
		return -1;
	}
	*objects = fy_assoc(capture->gb, *objects, fy_value(capture->gb, digest), object);
	if (!fy_is_valid(*objects)) {
		errno = ENOMEM;
		return -1;
	}
	entries = fy_get(object, "entries", fy_invalid);
	fy_foreach(entry, entries) {
		child = fy_get(entry, "digest", "");
		rc = capture_reused_objects(capture, child, objects, depth + 1, count);
		if (rc)
			return -1;
	}
	return 0;
}

fy_generic fyai_project_capture(struct fy_generic_builder *gb,
				const struct fyai_project_capture_opts *opts, char *error_path,
				size_t error_size)
{
	struct project_capture capture = {
		.gb = gb, .opts = opts, .error = error_path, .error_size = error_size
	};
	struct fy_thread_pool_cfg pool_cfg = { .flags = FYTPCF_STEAL_MODE };
	struct fy_blake3_hasher_cfg hash_cfg = { 0 };
	struct capture_worker *workers = NULL;
	struct stat st;
	const char *digest;
	fy_generic source;
	cpu_set_t affinity;
	fy_generic objects, object, base, attributes, timestamps, key, value, linked,
		result = fy_invalid;
	char *encoded = NULL;
	size_t length, j;
	static const char hex[] = "0123456789abcdef";
	size_t root, i, worker_count = 0, initialized = 0, reused_count = 0, files = 0;
	int rc, saved, cpus;

	if (error_path && error_size)
		error_path[0] = '\0';
	if (!gb || !opts ||
	    (fy_is_mapping(opts->snapshot) &&
	     !fy_equal(fy_get(opts->snapshot, "version", fy_invalid), 2LL)) ||
	    (opts->metacopy && (opts->data_fd < 0 || opts->baseline_fd < 0))) {
		errno = EINVAL;
		return fy_invalid;
	}
	if (fy_is_mapping(opts->snapshot) && (opts->incremental || opts->borrow_git)) {
		rc = fyai_project_verify_borrowed(opts->objects_fd, opts->snapshot, error_path,
						  error_size);
		if (rc)
			return fy_invalid;
	}
	rc = fstat(opts->source_fd, &st);
	if (rc)
		return fy_invalid;
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return fy_invalid;
	}
	capture.device = st.st_dev;
	atomic_init(&capture.next, 0);
	atomic_init(&capture.copy.backend, 0);
	rc = capture_add(&capture, SIZE_MAX, "", &st, &root);
	if (rc)
		goto out;
	if (opts->incremental) {
		digest = fy_get(opts->snapshot, "root", "");
		source = fy_get(opts->snapshot, "objects", fy_invalid);

		capture.nodes[root].baseline = fy_get(source, digest, fy_invalid);
		if (!fy_equal(fy_get(opts->snapshot, "version", fy_invalid), 2LL) ||
		    opts->upper_fd < 0 || opts->baseline_fd >= 0 ||
		    !fy_is_mapping(capture.nodes[root].baseline)) {
			errno = EINVAL;
			goto out;
		}
	}
	rc = capture_scan(&capture, root, 0);
	if (!rc)
		rc = capture_links(&capture);
	if (rc)
		goto out;
	for (i = 0; i < capture.count; i++)
		if (!capture.nodes[i].reused && capture.nodes[i].entry.kind == FYAI_PROJECT_FILE)
			files++;
	if (!files)
		goto finish;
	cpus = sched_getaffinity(0, sizeof(affinity), &affinity) ? 1 : CPU_COUNT(&affinity);
	if (cpus < 1)
		cpus = 1;
	if (cpus > 128)
		cpus = 128;
	pool_cfg.num_threads = opts->workers ? opts->workers : (unsigned int)cpus;
	if (pool_cfg.num_threads > 128) {
		errno = EINVAL;
		goto out;
	}
	capture.pool = fy_thread_pool_create(&pool_cfg);
	if (!capture.pool) {
		errno = ENOMEM;
		goto out;
	}
	worker_count = pool_cfg.num_threads + 1;
	workers = calloc(worker_count, sizeof(*workers));
	if (!workers)
		goto out;
	hash_cfg.tp = capture.pool;
	for (i = 0; i < worker_count; i++) {
		workers[i].capture = &capture;
		workers[i].hasher = fy_blake3_hasher_create(&hash_cfg);
		if (!workers[i].hasher) {
			errno = ENOMEM;
			goto out;
		}
		initialized++;
	}
	fy_thread_arg_array_join(capture.pool, capture_files, NULL, workers, sizeof(*workers),
				 worker_count);
	linked = fy_mapping(gb);
	for (i = 0; i < capture.count; i++) {
		if (!capture.nodes[i].blob.borrowed || capture.nodes[i].error)
			continue;
		if (!fy_is_valid(fy_get(linked, capture.nodes[i].blob.digest, fy_invalid))) {
			capture.nodes[i].link_blob = true;
			linked = fy_assoc(gb, linked, fy_value(gb, capture.nodes[i].blob.digest),
					  true);
			if (!fy_is_mapping(linked)) {
				errno = ENOMEM;
				goto out;
			}
		}
	}
	if (opts->baseline_fd >= 0) {
		atomic_store_explicit(&capture.next, 0, memory_order_relaxed);
		fy_thread_arg_array_join(capture.pool, capture_materialize_files, NULL, workers,
					 sizeof(*workers), worker_count);
	}
finish:
	if (opts->verify) {
		rc = capture_verify(&capture);
		if (rc)
			goto out;
	}
	objects = fy_mapping(gb);
	attributes = fy_get(opts->snapshot, "attributes", fy_mapping(gb));
	for (i = capture.count; i > 0; i--) {
		if (capture.nodes[i - 1].reused) {
			rc = capture_reused_objects(&capture, capture.nodes[i - 1].entry.digest,
						    &objects, 0, &reused_count);
			if (rc)
				goto out;
			continue;
		}
		object = capture_finish(&capture, i - 1);
		if (!fy_is_valid(object))
			goto out;
		base = fy_get(fy_get(opts->snapshot, "objects", fy_invalid),
			      capture.nodes[i - 1].entry.digest, fy_invalid);
		if (!fy_is_valid(base)) {
			base = fy_get(objects, capture.nodes[i - 1].entry.digest, object);
			if (fy_get(object, "mtime_sec", 0LL) < fy_get(base, "mtime_sec", 0LL) ||
			    (fy_get(object, "mtime_sec", 0LL) == fy_get(base, "mtime_sec", 0LL) &&
			     fy_get(object, "mtime_nsec", 0LL) < fy_get(base, "mtime_nsec", 0LL)))
				base = object;
		}
		if (capture.nodes[i - 1].entry.kind == FYAI_PROJECT_FILE) {
			base = fy_assoc(gb, base, "blob", fy_get(object, "blob", fy_invalid));
			base = fy_disassoc(gb, base, "borrowed");
			if (fy_is_valid(fy_get(object, "borrowed", fy_invalid)))
				base = fy_assoc(gb, base, "borrowed",
						fy_get(object, "borrowed", fy_invalid));
		}
		objects = fy_assoc(gb, objects, fy_value(gb, capture.nodes[i - 1].entry.digest),
				   base);

		if (!fy_is_valid(objects)) {
			errno = ENOMEM;
			goto out;
		}
	}
	for (i = 0; i < capture.count; i++) {
		if (capture.nodes[i].reused)
			continue;
		base = fy_get(objects, capture.nodes[i].entry.digest, fy_invalid);
		length = strlen(capture.nodes[i].path);
		encoded = malloc(length * 2 + 1);
		if (!encoded)
			goto out;
		for (j = 0; j < length; j++) {
			encoded[j * 2] = hex[(unsigned char)capture.nodes[i].path[j] >> 4];
			encoded[j * 2 + 1] = hex[(unsigned char)capture.nodes[i].path[j] & 15];
		}
		encoded[length * 2] = '\0';
		attributes = fy_disassoc(gb, attributes, encoded);
		if (!fy_equal((long long)capture.nodes[i].before.st_mtim.tv_sec,
			      fy_get(base, "mtime_sec", fy_invalid)) ||
		    !fy_equal((long long)capture.nodes[i].before.st_mtim.tv_nsec,
			      fy_get(base, "mtime_nsec", fy_invalid))) {
			timestamps = fy_mapping(
				gb, "mtime_sec", (long long)capture.nodes[i].before.st_mtim.tv_sec,
				"mtime_nsec", (long long)capture.nodes[i].before.st_mtim.tv_nsec);
			attributes = fy_assoc(gb, attributes, fy_value(gb, encoded), timestamps);
		}
		free(encoded);
		encoded = NULL;
		if (!fy_is_mapping(attributes)) {
			errno = ENOMEM;
			goto out;
		}
	}
	result = fy_mapping(gb, "version", 2LL, "algorithm", "blake3", "root",
			    fy_value(gb, capture.nodes[0].entry.digest), "objects", objects,
			    "attributes", attributes);
	base = attributes;
	fy_foreach_key_value(key, value, base) {
		object = fyai_project_lookup(result, fy_castp(&key, ""));
		if (!fy_is_mapping(object))
			attributes = fy_disassoc(gb, attributes, key);
	}
	result = fy_assoc(gb, result, "attributes", attributes);

out:
	saved = errno;
	free(encoded);
	for (i = 0; i < initialized; i++)
		fy_blake3_hasher_destroy(workers[i].hasher);
	free(workers);
	if (capture.pool)
		fy_thread_pool_destroy(capture.pool);
	for (i = 0; i < capture.count; i++) {
		free(capture.nodes[i].path);
		free(capture.nodes[i].link);
		free(capture.nodes[i].children);
	}
	free(capture.nodes);
	errno = saved;
	return result;
}

#else
fy_generic fyai_project_capture(struct fy_generic_builder *gb,
				const struct fyai_project_capture_opts *opts, char *error_path,
				size_t error_size)
{
	(void)gb;
	(void)opts;
	if (error_path && error_size)
		error_path[0] = '\0';
	errno = ENOTSUP;
	return fy_invalid;
}
#endif
