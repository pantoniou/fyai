/* SPDX-License-Identifier: MIT */
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
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
	       a->st_mode == b->st_mode && a->st_uid == b->st_uid &&
	       a->st_gid == b->st_gid && a->st_size == b->st_size &&
	       a->st_nlink == b->st_nlink &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
	       a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
	       a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
	       a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
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
		.mode = mode, .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV };

	return syscall(SYS_openat2, root, *path ? path : ".", &how, sizeof(how));
}

static int capture_add(struct project_capture *capture, size_t parent,
		       const char *name, const struct stat *st, size_t *index)
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
	node->path = path;
	node->before = *st;
	node->entry.name = (const unsigned char *)(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	node->entry.name_length = strlen((const char *)node->entry.name);
	if (S_ISREG(st->st_mode)) {
		if (st->st_nlink != 1 || (st->st_mode & 06000)) {
			errno = ENOTSUP;
			return -1;
		}
		node->entry.kind = FYAI_PROJECT_FILE;
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

static int capture_scan(struct project_capture *capture, size_t index, unsigned int depth)
{
	struct stat before, after;
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
		    (!depth && !strcmp(dent->d_name, ".fyai")))
			continue;
		rc = fstatat(source, dent->d_name, &before, AT_SYMLINK_NOFOLLOW);
		if (rc)
			goto out;
		rc = capture_add(capture, index, dent->d_name, &before, &child);
		if (rc)
			goto out;
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

static void capture_files(void *arg)
{
	struct capture_worker *worker = arg;
	struct project_capture *capture = worker->capture;
	struct capture_node *node;
	struct stat after;
	size_t index;
	int source, target, rc, saved, object;

	for (;;) {
		index = atomic_fetch_add_explicit(&capture->next, 1, memory_order_relaxed);
		if (index >= capture->count)
			break;
		node = &capture->nodes[index];
		if (node->entry.kind != FYAI_PROJECT_FILE)
			continue;
		target = -1;
		source = capture_open(capture->opts->source_fd, node->path, O_RDONLY | O_NONBLOCK, 0);
		if (source < 0) {
			node->error = errno;
			continue;
		}
		rc = fstat(source, &after);
		if (!rc && !capture_same(&node->before, &after)) {
			errno = EAGAIN;
			rc = -1;
		}
		if (!rc)
			rc = capture_xattrs(capture, source);
		if (!rc)
			rc = fyai_cas_put_hasher(capture->opts->objects_fd, source, &node->blob, worker->hasher);
		if (!rc && capture->opts->baseline_fd >= 0) {
			target = capture_open(capture->opts->baseline_fd, node->path, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (target < 0)
				rc = -1;
			if (!rc) {
				object = openat(capture->opts->objects_fd, node->blob.digest, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
				if (object < 0)
					rc = -1;
				else {
					rc = fyai_cas_copy(object, target, node->blob.size);
					saved = errno;
					close(object);
					errno = saved;
				}
			}
			if (!rc)
				rc = capture_apply(target, &node->before);
		}
		if (!rc) {
			rc = fstat(source, &after);
			if (!rc && !capture_same(&node->before, &after)) {
				errno = EAGAIN;
				rc = -1;
			}
		}
		if (rc)
			node->error = errno ? errno : EIO;
		if (target >= 0)
			close(target);
		close(source);
	}
}

static int capture_compare_source(int source, int object, uint64_t size,
				 unsigned char *buffer)
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
	int source = -1, object = -1, baseline = -1, rc = -1, saved;

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
		if (node->entry.kind != FYAI_PROJECT_FILE)
			continue;
		source = capture_open(capture->opts->source_fd, node->path, O_RDONLY | O_NONBLOCK, 0);
		if (source < 0)
			goto failed;
		rc = fstat(source, &after);
		if (rc)
			goto failed;
		if (!capture_same(&node->before, &after)) {
			errno = EAGAIN;
			goto failed;
		}
		rc = fyai_cas_verify_hasher(capture->opts->objects_fd, &node->blob, hasher);
		if (rc)
			goto failed;
		object = openat(capture->opts->objects_fd, node->blob.digest, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (object < 0)
			goto failed;
		rc = capture_compare_source(source, object, node->blob.size, buffer);
		if (rc)
			goto failed;
		if (capture->opts->baseline_fd >= 0) {
			baseline = capture_open(capture->opts->baseline_fd, node->path, O_RDONLY, 0);
			if (baseline < 0)
				goto failed;
			rc = fyai_cas_verify_file(baseline, &node->blob, hasher);
			if (rc)
				goto failed;
			close(baseline);
			baseline = -1;
		}
		rc = fstat(source, &after);
		if (rc)
			goto failed;
		if (!capture_same(&node->before, &after)) {
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
		source = capture_open(capture->opts->source_fd, node->path, O_RDONLY | O_NONBLOCK |
			(node->entry.kind == FYAI_PROJECT_DIRECTORY ? O_DIRECTORY : 0), 0);
		if (source < 0)
			goto out;
		rc = fstat(source, &after);
	}
	if (rc)
		goto out;
	if (!capture_same(&node->before, &after)) {
		errno = EAGAIN;
		goto out;
	}
	if (node->entry.kind == FYAI_PROJECT_FILE)
		result = fyai_project_file(capture->gb, &metadata, &node->blob, node->entry.digest);
	else if (node->entry.kind == FYAI_PROJECT_SYMLINK) {
		result = fyai_project_symlink(capture->gb, &metadata,
			(const unsigned char *)node->link, strlen(node->link), node->entry.digest);
		if (!fy_is_valid(result))
			goto out;
		if (capture->opts->baseline_fd >= 0) {
			target = capture_open(capture->opts->baseline_fd, parent, O_RDONLY | O_DIRECTORY, 0);
			if (target < 0)
				goto invalid;
			rc = symlinkat(node->link, target, name);
			if (!rc)
				rc = fchownat(target, name, node->before.st_uid, node->before.st_gid, AT_SYMLINK_NOFOLLOW);
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
			target = capture_open(capture->opts->baseline_fd, node->path, O_RDONLY | O_DIRECTORY, 0);
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
		result = fyai_project_directory(capture->gb, &metadata, entries, node->count, !index, node->entry.digest);
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

fy_generic fyai_project_capture(struct fy_generic_builder *gb,
		const struct fyai_project_capture_opts *opts,
		char *error_path, size_t error_size)
{
	struct project_capture capture = { .gb = gb, .opts = opts,
		.error = error_path, .error_size = error_size };
	struct fy_thread_pool_cfg pool_cfg = { .flags = FYTPCF_STEAL_MODE };
	struct fy_blake3_hasher_cfg hash_cfg = { 0 };
	struct capture_worker *workers = NULL;
	struct stat st;
	cpu_set_t affinity;
	fy_generic objects, object, result = fy_invalid;
	size_t root, i, worker_count = 0, initialized = 0;
	int rc, saved, cpus;

	if (error_path && error_size)
		error_path[0] = '\0';
	if (!gb || !opts) {
		errno = EINVAL;
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
	rc = capture_add(&capture, SIZE_MAX, "", &st, &root);
	if (rc)
		goto out;
	rc = capture_scan(&capture, root, 0);
	if (rc)
		goto out;
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
	fy_thread_arg_array_join(capture.pool, capture_files, NULL, workers, sizeof(*workers), worker_count);
	if (opts->verify) {
		rc = capture_verify(&capture);
		if (rc)
			goto out;
	}
	objects = fy_mapping(gb);
	for (i = capture.count; i > 0; i--) {
		object = capture_finish(&capture, i - 1);
		if (!fy_is_valid(object))
			goto out;
		objects = fy_assoc(gb, objects, fy_value(gb, capture.nodes[i - 1].entry.digest), object);
		if (!fy_is_valid(objects)) {
			errno = ENOMEM;
			goto out;
		}
	}
	result = fy_mapping(gb, "version", 1LL, "algorithm", "blake3",
		"root", fy_value(gb, capture.nodes[0].entry.digest), "objects", objects);
out:
	saved = errno;
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
		const struct fyai_project_capture_opts *opts,
		char *error_path, size_t error_size)
{
	(void)gb;
	(void)opts;
	if (error_path && error_size)
		error_path[0] = '\0';
	errno = ENOTSUP;
	return fy_invalid;
}
#endif
