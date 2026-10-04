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
#include "fyai_manifest.h"
#include "fyai_scratch.h"

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
#include <pthread.h>
#include <time.h>
#include <stdarg.h>

/* How a node reached its baseline; the first values are the CAS copy methods. */
enum capture_storage {
	CAPTURE_STORAGE_HARDLINK = FYAI_CAS_METHOD_SENDFILE + 1,
	CAPTURE_STORAGE_METACOPY,
	CAPTURE_STORAGE_COUNT,
};

struct capture_node {
	char *path;
	char *link;
	/* The baseline manifest has an object at this path; entry.digest names it. */
	bool has_baseline;
	/* The directory entries, sorted, once the children are finished. */
	struct fyai_project_entry *entries;
	/* The timestamp that the manifest object of this digest stores. */
	int64_t object_sec;
	uint32_t object_nsec;
	bool reused;
	bool materialized;
	bool link_blob;
	int storage;
	int source_root, other_root, staged_fd;
	const char *source_path;
	bool git;
	struct stat original, other_before;
	struct stat before;
	struct stat metadata;
	/* The entry holds the identity of the object in binary. */
	struct fyai_project_entry entry;
	struct fyai_cas_blob blob;
	size_t *children;
	size_t count;
	size_t capacity;
	int error;
};

struct project_capture {
	/* Nodes, paths and the other temporaries of one capture; released at the end. */
	struct fyai_scratch scratch;
	const struct fyai_project_capture_opts *opts;
	/* The baseline manifest, or NULL. */
	const struct fyai_manifest *base;
	struct fy_blake3_hasher *finish_hasher;
	struct capture_node *nodes;
	struct fy_thread_pool *pool;
	struct fyai_cas_copy_state copy;
	struct fyai_project_capture_stats stats;
	/* The credentials of the process, read once: they do not change during a capture. */
	uid_t euid;
	gid_t egid;
	struct timespec started;
	atomic_uint_fast64_t completed, storage[CAPTURE_STORAGE_COUNT];
	atomic_size_t next;
	dev_t device;
	int git_private, git_common, git_pointer, common_pointer;
	struct stat git_before, common_before;
	char *git_path;
	/* The nodes are added by the scan workers; count is the next free index. */
	atomic_size_t count;
	/* The nodes of level d, the directories of depth d, are [levels[d], levels[d + 1]). */
	size_t *levels;
	size_t level_count;
	size_t level_first, level_last;
	char *error;
	size_t error_size;
	pthread_mutex_t error_lock;
};

/* A level with fewer nodes is finished by the caller. */
#define CAPTURE_FINISH_PARALLEL 256

struct capture_worker {
	struct project_capture *capture;
	struct fy_blake3_hasher *hasher;
};

static void capture_progress(struct project_capture *capture)
{
	struct timespec now;
	struct fyai_project_capture_stats *stats = &capture->stats;

	clock_gettime(CLOCK_MONOTONIC, &now);
	stats->elapsed_ms = (now.tv_sec - capture->started.tv_sec) * 1000LL +
			    (now.tv_nsec - capture->started.tv_nsec) / 1000000LL;
	stats->completed = atomic_load_explicit(&capture->completed, memory_order_relaxed);
	stats->copied_bytes =
		atomic_load_explicit(&capture->copy.copied_bytes, memory_order_relaxed);
	stats->reflinked_bytes =
		atomic_load_explicit(&capture->copy.reflinked_bytes, memory_order_relaxed);
	stats->copy_backend = atomic_load_explicit(&capture->copy.backend, memory_order_relaxed);
	stats->reflinks = atomic_load_explicit(&capture->storage[FYAI_CAS_METHOD_REFLINK],
					       memory_order_relaxed);
	stats->copies = atomic_load_explicit(&capture->storage[FYAI_CAS_METHOD_COPY_RANGE],
					     memory_order_relaxed) +
			atomic_load_explicit(&capture->storage[FYAI_CAS_METHOD_SENDFILE],
					     memory_order_relaxed);
	stats->hardlinks = atomic_load_explicit(&capture->storage[CAPTURE_STORAGE_HARDLINK],
						memory_order_relaxed);
	stats->metacopies = atomic_load_explicit(&capture->storage[CAPTURE_STORAGE_METACOPY],
						 memory_order_relaxed);
	if (capture->opts->stats)
		*capture->opts->stats = *stats;
	if (capture->opts->progress)
		capture->opts->progress(capture->opts->progress_arg, stats);
}

struct capture_join {
	struct project_capture *capture;
	void *args;
	size_t argsize;
	size_t count;
	fy_work_exec_fn fn;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	bool done;
};

static void *capture_join_thread(void *arg)
{
	struct capture_join *join = arg;

	fy_thread_arg_array_join(join->capture->pool, join->fn, NULL, join->args, join->argsize,
				 join->count);
	pthread_mutex_lock(&join->mutex);
	join->done = true;
	pthread_cond_signal(&join->condition);
	pthread_mutex_unlock(&join->mutex);
	return NULL;
}

/*
 * Run fn for each of the count items of args on the pool of the capture. The
 * caller renders the progress while it waits: only the caller does.
 */
static int capture_join(struct project_capture *capture, fy_work_exec_fn fn, void *args,
			size_t argsize, size_t count)
{
	struct capture_join join = { .capture = capture,
				     .args = args,
				     .argsize = argsize,
				     .count = count,
				     .fn = fn,
				     .mutex = PTHREAD_MUTEX_INITIALIZER,
				     .condition = PTHREAD_COND_INITIALIZER };
	struct timespec deadline;
	pthread_t thread;
	int rc;

	if (!capture->opts->progress) {
		fy_thread_arg_array_join(capture->pool, fn, NULL, args, argsize, count);
		return 0;
	}
	rc = pthread_create(&thread, NULL, capture_join_thread, &join);
	if (rc) {
		errno = rc;
		return -1;
	}
	pthread_mutex_lock(&join.mutex);
	while (!join.done) {
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 200000000;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&join.condition, &join.mutex, &deadline);
		pthread_mutex_unlock(&join.mutex);
		capture_progress(capture);
		pthread_mutex_lock(&join.mutex);
	}
	pthread_mutex_unlock(&join.mutex);
	rc = pthread_join(thread, NULL);
	pthread_cond_destroy(&join.condition);
	pthread_mutex_destroy(&join.mutex);
	if (rc)
		errno = rc;
	return rc ? -1 : 0;
}

static int capture_join_workers(struct project_capture *capture, fy_work_exec_fn fn,
				struct capture_worker *workers, size_t count)
{
	return capture_join(capture, fn, workers, sizeof(*workers), count);
}

/* Record the first error path; the scan workers can fail together. */
static void capture_set_error(struct project_capture *capture, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static void capture_set_error(struct project_capture *capture, const char *format, ...)
{
	va_list ap;

	if (!capture->error || !capture->error_size)
		return;
	pthread_mutex_lock(&capture->error_lock);
	if (!capture->error[0]) {
		va_start(ap, format);
		vsnprintf(capture->error, capture->error_size, format, ap);
		va_end(ap);
	}
	pthread_mutex_unlock(&capture->error_lock);
}

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
	struct stat observed = *after, original;

	if (node->blob.borrowed) {
		observed.st_nlink = node->before.st_nlink;
		observed.st_ctim = node->before.st_ctim;
	}
	if (!capture_same(&node->before, &observed))
		return false;
	if (node->staged_fd >= 0) {
		if (fstatat(node->source_root, node->source_path, &original, AT_SYMLINK_NOFOLLOW) ||
		    !capture_same(&node->original, &original))
			return false;
	}
	if (node->other_root >= 0) {
		if (fstatat(node->other_root, *node->source_path ? node->source_path : ".",
			    &original, AT_SYMLINK_NOFOLLOW) ||
		    !capture_same(&node->other_before, &original))
			return false;
	}
	return true;
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

	length = flistxattr(fd, NULL, 0);
	if (length <= 0)
		return length < 0 ? -1 : 0;
	if (!capture->opts->mapped_owner) {
		errno = ENOTSUP;
		return -1;
	}
	names = fyai_scratch_alloc(&capture->scratch, length);
	if (!names)
		return -1;
	actual = flistxattr(fd, names, length);
	if (actual < 0)
		return -1;
	for (name = names; name < names + actual; name += strlen(name) + 1) {
		if (strncmp(name, "user.overlay.", 13)) {
			errno = ENOTSUP;
			return -1;
		}
	}
	return 0;
}

static int capture_apply_metadata(int fd, const struct stat *st)
{
	struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, st->st_mtim };
	int rc;

	rc = fchown(fd, st->st_uid, st->st_gid);
	if (!rc)
		rc = fchmod(fd, st->st_mode & 07777);
	if (!rc)
		rc = futimens(fd, times);
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

static int capture_node_open(const struct capture_node *node, int flags)
{
	int fd, saved;

	if (node->staged_fd < 0)
		return capture_open(node->source_root, node->source_path, flags, 0);
	fd = fcntl(node->staged_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return -1;
	if (lseek(fd, 0, SEEK_SET) >= 0)
		return fd;
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}

static int capture_git_text(int fd, char text[PATH_MAX])
{
	struct stat st;
	ssize_t length;

	if (fstat(fd, &st))
		return -1;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= PATH_MAX) {
		errno = ENOTSUP;
		return -1;
	}
	length = pread(fd, text, st.st_size, 0);
	if (length != st.st_size) {
		if (length >= 0)
			errno = EAGAIN;
		return -1;
	}
	if (memchr(text, '\0', length)) {
		errno = ENOTSUP;
		return -1;
	}
	while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
		length--;
	text[length] = '\0';
	return 0;
}

static int capture_git_prepare(struct project_capture *capture)
{
	char text[PATH_MAX];
	struct stat st, common;
	int fd, rc;

	if (capture->opts->incremental)
		return 0;
	rc = fstatat(capture->opts->source_fd, ".git", &st, AT_SYMLINK_NOFOLLOW);
	if (rc)
		return errno == ENOENT ? 0 : -1;
	if (S_ISDIR(st.st_mode))
		return 0;
	if (!S_ISREG(st.st_mode)) {
		errno = ENOTSUP;
		return -1;
	}
	capture->git_before = st;
	capture->git_pointer =
		openat(capture->opts->source_fd, ".git", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (capture->git_pointer < 0)
		return -1;
	rc = capture_git_text(capture->git_pointer, text);
	if (rc)
		return -1;
	if (fstat(capture->git_pointer, &st) || !capture_same(&capture->git_before, &st)) {
		errno = EAGAIN;
		return -1;
	}
	if (strncmp(text, "gitdir: ", 8) || !text[8]) {
		errno = ENOTSUP;
		return -1;
	}
	capture->git_path = fyai_scratch_strndup(&capture->scratch, text + 8, strlen(text + 8));
	if (!capture->git_path)
		return -1;
	capture->git_private = openat(capture->opts->source_fd, capture->git_path,
				      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (capture->git_private < 0)
		return -1;
	fd = openat(capture->git_private, "commondir", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT)
			return -1;
		capture->git_common = fcntl(capture->git_private, F_DUPFD_CLOEXEC, 3);
	} else {
		capture->common_pointer = fd;
		rc = fstat(fd, &capture->common_before);
		if (!rc)
			rc = capture_git_text(fd, text);
		if (rc)
			return -1;
		capture->git_common = openat(capture->git_private, text,
					     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	}
	if (capture->git_common < 0)
		return -1;
	if (fstat(capture->git_private, &st) || fstat(capture->git_common, &common))
		return -1;
	if (st.st_dev != capture->device || common.st_dev != capture->device) {
		errno = EXDEV;
		return -1;
	}
	fd = openat(capture->git_common, "objects",
		    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

/* Configuration bytes are private; the original inode remains a consistency
 * input. */
static int capture_git_config(struct project_capture *capture, struct capture_node *node)
{
	static const char suffix[] = "\n\n[core]\n\tbare = false\n\tworktree = ..\n";
	struct stat after;
	char *bytes = NULL;
	ssize_t amount;
	size_t size, offset;
	int source, rc = -1, saved, directory;

	if (node->before.st_size < 0 || node->before.st_size > (1U << 20)) {
		errno = E2BIG;
		return -1;
	}
	source = capture_node_open(node, O_RDONLY);
	if (source < 0)
		return -1;
	rc = capture_xattrs(capture, source);
	if (rc)
		goto out;
	rc = -1;
	size = node->before.st_size;
	bytes = fyai_scratch_alloc(&capture->scratch, size + sizeof(suffix) - 1);
	if (!bytes)
		goto out;
	amount = pread(source, bytes, size, 0);
	if (amount != (ssize_t)size) {
		if (amount >= 0)
			errno = EAGAIN;
		goto out;
	}
	memcpy(bytes + size, suffix, sizeof(suffix) - 1);
	size += sizeof(suffix) - 1;
	directory = capture->opts->baseline_fd >= 0 ? capture->opts->baseline_fd :
						      capture->opts->objects_fd;
	node->staged_fd = openat(directory, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
	if (node->staged_fd < 0)
		goto out;
	for (offset = 0; offset < size; offset += amount) {
		amount = write(node->staged_fd, bytes + offset, size - offset);
		if (amount < 0 && errno == EINTR) {
			amount = 0;
			continue;
		}
		if (amount <= 0)
			goto out;
	}
	rc = fstat(source, &after);
	if (!rc && !capture_same(&node->before, &after)) {
		errno = EAGAIN;
		rc = -1;
	}
	if (rc)
		goto out;
	node->original = node->before;
	rc = capture_apply_metadata(node->staged_fd, &node->metadata);
	if (!rc)
		rc = fstat(node->staged_fd, &node->before);
	if (!rc)
		__atomic_fetch_add(&capture->stats.logical_bytes, sizeof(suffix) - 1,
				   __ATOMIC_RELAXED);
out:
	saved = errno;
	close(source);
	errno = saved;
	return rc;
}

static int capture_add(struct project_capture *capture, size_t parent, const char *name,
		       const struct stat *st, size_t *index)
{
	struct capture_node *node;
	char *path = NULL, *slash;
	size_t capacity, *children, parent_length = 0, name_length = strlen(name), next;

	if (st->st_dev != capture->device) {
		errno = EXDEV;
		goto err_out;
	}
	if (parent != SIZE_MAX)
		parent_length = strlen(capture->nodes[parent].path);
	if (parent_length + 1 + name_length >= PATH_MAX) {
		errno = ENAMETOOLONG;
		goto err_out;
	}
	path = fyai_scratch_alloc(&capture->scratch, parent_length + 1 + name_length + 1);
	if (!path)
		goto err_out;
	if (parent == SIZE_MAX) {
		*path = '\0';
	} else {
		memcpy(path, capture->nodes[parent].path, parent_length);
		if (parent_length)
			path[parent_length++] = '/';
		memcpy(path + parent_length, name, name_length + 1);
	}
	/* The nodes array has room for the most nodes a capture can hold. */
	next = atomic_load_explicit(&capture->count, memory_order_relaxed);
	do {
		if (next >= FYAI_PROJECT_MAX_NODES) {
			errno = E2BIG;
			goto err_out;
		}
	} while (!atomic_compare_exchange_weak_explicit(&capture->count, &next, next + 1,
							memory_order_relaxed, memory_order_relaxed));
	*index = next;
	node = &capture->nodes[*index];
	memset(node, 0, sizeof(*node));
	node->source_root = capture->opts->source_fd;
	node->other_root = node->staged_fd = -1;
	node->source_path = path;
	node->path = path;
	node->before = *st;
	node->metadata = *st;
	if (!capture->opts->incremental && capture->euid) {
		node->metadata.st_uid = capture->euid;
		node->metadata.st_gid = capture->egid;
	}
	slash = strrchr(node->path, '/');
	node->entry.name = (const unsigned char *)(slash ? slash + 1 : node->path);
	node->entry.name_length = strlen((const char *)node->entry.name);
	if (S_ISREG(st->st_mode)) {
		if (st->st_mode & 06000) {
			errno = ENOTSUP;
			goto err_out;
		}
		node->entry.kind = FYAI_PROJECT_FILE;
		node->blob.borrowed = capture->opts->borrow_git &&
				      !strncmp(node->path, ".git/objects/", 13) &&
				      (!capture->euid || st->st_uid == capture->euid);
	} else if (S_ISDIR(st->st_mode))
		node->entry.kind = FYAI_PROJECT_DIRECTORY;
	else if (S_ISLNK(st->st_mode))
		node->entry.kind = FYAI_PROJECT_SYMLINK;
	else {
		errno = ENOTSUP;
		goto err_out;
	}
	if (parent != SIZE_MAX) {
		node = &capture->nodes[parent];
		if (node->count == node->capacity) {
			capacity = node->capacity ? node->capacity * 2 : 16;
			children = fyai_scratch_grow(&capture->scratch, node->children, node->count,
						     capacity, sizeof(*children));
			if (!children)
				goto err_out;
			node->children = children;
			node->capacity = capacity;
		}
		node->children[node->count++] = *index;
	}
	if (capture->nodes[*index].entry.kind == FYAI_PROJECT_FILE) {
		__atomic_fetch_add(&capture->stats.files, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&capture->stats.logical_bytes, (uint64_t)st->st_size,
				   __ATOMIC_RELAXED);
		if (capture->nodes[*index].blob.borrowed) {
			__atomic_fetch_add(&capture->stats.borrowed_files, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&capture->stats.borrowed_bytes, (uint64_t)st->st_size,
					   __ATOMIC_RELAXED);
		}
	} else if (capture->nodes[*index].entry.kind == FYAI_PROJECT_DIRECTORY) {
		__atomic_fetch_add(&capture->stats.directories, 1, __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_add(&capture->stats.symlinks, 1, __ATOMIC_RELAXED);
	}

	return 0;
err_out:
	return -1;
}

/*
 * Decode the entries of a directory of the baseline, in name order, into an
 * array of the scratch arena; the names point into the manifest.
 */
static struct fyai_project_entry *capture_base_entries(struct project_capture *capture,
						      const struct fyai_mobject *directory,
						      size_t *count)
{
	struct fyai_project_entry *entries;
	struct fyai_mdir iterator;
	size_t i = 0;

	*count = 0;
	if (!directory->entry_count)
		return NULL;
	entries = fyai_scratch_alloc(&capture->scratch, directory->entry_count * sizeof(*entries));
	if (!entries || fyai_mdir_open(directory, &iterator))
		return NULL;
	while (i < directory->entry_count && fyai_mdir_next(&iterator, &entries[i]))
		i++;
	if (i != directory->entry_count)
		return NULL;
	*count = i;
	return entries;
}

/* Find the baseline entry of name in the sorted entries of its directory. */
static const struct fyai_project_entry *capture_base_entry(const struct fyai_project_entry *entries,
							   size_t count, const char *name,
							   size_t length)
{
	size_t low = 0, high = count, middle, common;
	int rc;

	while (low < high) {
		middle = low + (high - low) / 2;
		common = entries[middle].name_length < length ? entries[middle].name_length : length;
		rc = memcmp(name, entries[middle].name, common);
		if (!rc)
			rc = (length > entries[middle].name_length) - (length < entries[middle].name_length);
		if (rc < 0)
			high = middle;
		else if (rc > 0)
			low = middle + 1;
		else
			return &entries[middle];
	}
	return NULL;
}

static bool capture_git_link_safe(const char *path, const char *link)
{
	const char *p, *end;
	size_t depth = 0, length;

	if (*link == '/')
		return false;
	for (p = path; *p; p++)
		if (*p == '/')
			depth++;
	for (p = link; *p; p = *end ? end + 1 : end) {
		end = strchrnul(p, '/');
		length = end - p;
		if (length == 2 && p[0] == '.' && p[1] == '.') {
			if (!depth)
				return false;
			depth--;
		} else if (length && !(length == 1 && p[0] == '.'))
			depth++;
	}
	return true;
}

/*
 * One directory of the scan, one item of a level: the directories that it finds
 * are the items of the next level.
 */
struct scan_item {
	struct project_capture *capture;
	size_t index;
	unsigned int depth;
	int rc, error;
	size_t *subs;
	size_t sub_count, sub_capacity;
};

static int scan_item_add(struct scan_item *item, size_t child)
{
	size_t capacity, *subs;

	if (item->sub_count == item->sub_capacity) {
		capacity = item->sub_capacity ? item->sub_capacity * 2 : 16;
		subs = fyai_scratch_grow(&item->capture->scratch, item->subs, item->sub_count,
					 capacity, sizeof(*subs));
		if (!subs)
			return -1;
		item->subs = subs;
		item->sub_capacity = capacity;
	}
	item->subs[item->sub_count++] = child;
	return 0;
}

static int capture_scan_dir(struct project_capture *capture, size_t index, unsigned int depth,
			    struct scan_item *item)
{
	struct stat before, after, upper, alternate;
	struct dirent *dent;
	DIR *directory = NULL;
	char link[PATH_MAX];
	const char *path = capture->nodes[index].path;
	struct fyai_project_entry *base_entries = NULL;
	const struct fyai_project_entry *base_entry;
	struct fyai_mobject base_directory;
	size_t child, base_count = 0;
	ssize_t length;
	int source = -1, other = -1, current, target = -1, rc = -1, saved, iterator;
	bool secondary = false, projected;

	if (depth > FYAI_PROJECT_MAX_DEPTH) {
		errno = ELOOP;
		goto out;
	}
	source = capture_node_open(&capture->nodes[index], O_RDONLY | O_DIRECTORY);
	if (source < 0)
		goto out;
	rc = fstat(source, &before);
	if (rc)
		goto out;
	if (!capture_node_same(&capture->nodes[index], &before))
		goto err_out_eagain;
	/* The entries of the baseline directory, to match the children by name. */
	if (capture->nodes[index].has_baseline && capture->base &&
	    fyai_manifest_find(capture->base, capture->nodes[index].entry.digest, &base_directory) &&
	    base_directory.kind == FYAI_PROJECT_DIRECTORY) {
		base_entries = capture_base_entries(capture, &base_directory, &base_count);
		if (base_directory.entry_count && !base_entries)
			goto out;
	}
	rc = capture_xattrs(capture, source);
	if (rc)
		goto out;
	if (capture->nodes[index].other_root >= 0) {
		other = capture_open(capture->nodes[index].other_root,
				     capture->nodes[index].source_path, O_RDONLY | O_DIRECTORY, 0);
		if (other < 0)
			goto err_out;
		rc = capture_xattrs(capture, other);
		if (rc)
			goto out;
	}
	if (capture->opts->baseline_fd >= 0) {
		target = capture_open(capture->opts->baseline_fd, path, O_RDONLY | O_DIRECTORY, 0);
		if (target < 0)
			goto err_out;
	}
next_directory:
	current = secondary ? other : source;
	iterator = fcntl(current, F_DUPFD_CLOEXEC, 3);
	if (iterator < 0)
		goto err_out;
	directory = fdopendir(iterator);
	if (!directory) {
		close(iterator);
		goto err_out;
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
		if (capture->nodes[index].git && !strcmp(path, ".git") &&
		    (!strcmp(dent->d_name, "gitdir") || !strcmp(dent->d_name, "commondir") ||
		     !strcmp(dent->d_name, "worktrees") ||
		     (secondary && !strcmp(dent->d_name, "config.worktree"))))
			continue;
		if (secondary) {
			rc = fstatat(source, dent->d_name, &before, AT_SYMLINK_NOFOLLOW);
			if (!rc)
				continue;
			if (errno != ENOENT)
				goto out;
		}
		rc = fstatat(current, dent->d_name, &before, AT_SYMLINK_NOFOLLOW);
		if (rc)
			goto out;
		projected = !depth && !strcmp(dent->d_name, ".git") && capture->git_private >= 0;
		if (projected) {
			if (!capture_same(&capture->git_before, &before))
				goto err_out_eagain;
			rc = fstat(capture->git_private, &before);
			if (rc)
				goto out;
		} else if (S_ISREG(before.st_mode) && !strcmp(dent->d_name, ".git")) {
			errno = ENOTSUP;
			capture_set_error(capture, "%s/.git (nested Git files and "
						   "submodules are unsupported)", path);
			goto err_out;
		}
		rc = capture_add(capture, index, dent->d_name, &before, &child);
		if (rc)
			goto out;
		if (projected) {
			capture->nodes[child].git = true;
			capture->nodes[child].source_root = capture->git_private;
			capture->nodes[child].source_path = "";
			rc = fstat(capture->git_common, &alternate);
			if (rc)
				goto out;
			if (alternate.st_dev != before.st_dev ||
			    alternate.st_ino != before.st_ino) {
				capture->nodes[child].other_root = capture->git_common;
				capture->nodes[child].other_before = alternate;
			}
		} else if (capture->nodes[index].git) {
			capture->nodes[child].git = true;
			capture->nodes[child].source_root =
				secondary ? capture->nodes[index].other_root :
					    capture->nodes[index].source_root;
			capture->nodes[child].source_path = capture->nodes[child].path + 5;
			if (!secondary && other >= 0 && S_ISDIR(before.st_mode)) {
				rc = fstatat(other, dent->d_name, &alternate, AT_SYMLINK_NOFOLLOW);
				if (rc && errno != ENOENT)
					goto out;
				if (!rc && S_ISDIR(alternate.st_mode)) {
					capture->nodes[child].other_root =
						capture->nodes[index].other_root;
					capture->nodes[child].other_before = alternate;
				}
			}
		}
		if (!strcmp(capture->nodes[child].path, ".git/objects/info/alternates") &&
		    before.st_size) {
			errno = ENOTSUP;
			capture_set_error(capture, ".git/objects/info/alternates "
						   "(external object alternates are "
						   "unsupported)");
			goto err_out;
		}
		if (capture->nodes[child].git && !S_ISREG(before.st_mode) &&
		    (!strcmp(capture->nodes[child].path, ".git/config") ||
		     !strcmp(capture->nodes[child].path, ".git/config.worktree"))) {
			errno = ENOTSUP;
			capture_set_error(capture, "%s (Git configuration must be a "
						   "regular file)", capture->nodes[child].path);
			goto err_out;
		}
		if (capture->nodes[child].git && S_ISREG(before.st_mode) &&
		    (!strcmp(capture->nodes[child].path, ".git/config") ||
		     !strcmp(capture->nodes[child].path, ".git/config.worktree"))) {
			rc = capture_git_config(capture, &capture->nodes[child]);
			if (rc)
				goto out;
		}
		base_entry = base_entries ? capture_base_entry(base_entries, base_count, dent->d_name,
							       strlen(dent->d_name)) : NULL;
		if (base_entry) {
			capture->nodes[child].has_baseline = true;
			memcpy(capture->nodes[child].entry.digest, base_entry->digest,
			       FYAI_CAS_HASH_SIZE);
		}
		if (capture->opts->incremental) {
			rc = fstatat(capture->opts->upper_fd, capture->nodes[child].path, &upper,
				     AT_SYMLINK_NOFOLLOW);
			if (rc && errno != ENOENT && errno != ENOTDIR)
				goto out;
			if (rc && capture->nodes[child].has_baseline) {
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
			rc = scan_item_add(item, child);
			if (rc)
				goto out;
		} else if (S_ISLNK(before.st_mode)) {
			length = readlinkat(current, dent->d_name, link, sizeof(link));
			if (length < 0 || (size_t)length == sizeof(link)) {
				if (length >= 0)
					errno = ENAMETOOLONG;
				goto err_out;
			}
			link[length] = '\0';
			if (capture->nodes[child].git &&
			    !capture_git_link_safe(capture->nodes[child].path, link)) {
				errno = ENOTSUP;
				capture_set_error(capture, "%s (Git metadata symlink "
							   "leaves the project)",
						  capture->nodes[child].path);
				goto err_out;
			}
			capture->nodes[child].link =
				fyai_scratch_strndup(&capture->scratch, link, (size_t)length);
			if (!capture->nodes[child].link)
				goto err_out;
		}
	}
	closedir(directory);
	directory = NULL;
	if (!rc && !secondary && other >= 0) {
		secondary = true;
		goto next_directory;
	}
	if (rc)
		goto out;
	rc = fstat(source, &after);
	if (rc)
		goto out;
	if (!capture_node_same(&capture->nodes[index], &after))
		goto err_out_eagain;
	goto out;
err_out_eagain:
	errno = EAGAIN;
err_out:
	rc = -1;
out:
	saved = errno;
	if (directory)
		closedir(directory);
	if (source >= 0)
		close(source);
	if (other >= 0)
		close(other);
	if (target >= 0)
		close(target);
	if (rc)
		capture_set_error(capture, "%s", *path ? path : ".");
	errno = saved;
	return rc;
}


static void capture_scan_item(void *arg)
{
	struct scan_item *item = arg;

	item->rc = capture_scan_dir(item->capture, item->index, item->depth, item);
	item->error = item->rc ? errno : 0;
}

/*
 * Scan the tree one level of directories at a time. The directories of a level
 * are independent, so the pool takes them in parallel; the nodes get their
 * indexes as they are added, and a child always has a higher index than its
 * parent.
 */
static int capture_scan(struct project_capture *capture, size_t root)
{
	struct scan_item *items;
	size_t *level, *next, count = 1, next_count, i, j;
	unsigned int depth = 0;

	level = fyai_scratch_alloc(&capture->scratch, sizeof(*level));
	if (!level)
		return -1;
	level[0] = root;
	capture->levels = fyai_scratch_alloc(&capture->scratch, (FYAI_PROJECT_MAX_DEPTH + 2) *
							 sizeof(*capture->levels));
	if (!capture->levels)
		return -1;
	capture->levels[0] = root;
	while (count) {
		items = fyai_scratch_calloc(&capture->scratch, count, sizeof(*items));
		if (!items)
			return -1;
		for (i = 0; i < count; i++) {
			items[i].capture = capture;
			items[i].index = level[i];
			items[i].depth = depth;
		}
		if (capture_join(capture, capture_scan_item, items, sizeof(*items), count))
			return -1;
		next_count = 0;
		for (i = 0; i < count; i++) {
			if (items[i].rc) {
				errno = items[i].error;
				return -1;
			}
			next_count += items[i].sub_count;
		}
		next = next_count ? fyai_scratch_alloc(&capture->scratch,
						       next_count * sizeof(*next)) : NULL;
		if (next_count && !next)
			return -1;
		for (i = 0, next_count = 0; i < count; i++)
			for (j = 0; j < items[i].sub_count; j++)
				next[next_count++] = items[i].subs[j];
		level = next;
		count = next_count;
		depth++;
		if (depth > FYAI_PROJECT_MAX_DEPTH) {
			errno = E2BIG;
			return -1;
		}
		capture->levels[depth] = atomic_load_explicit(&capture->count, memory_order_relaxed);
		capture_progress(capture);
	}
	capture->level_count = depth;
	capture->levels[depth] = atomic_load_explicit(&capture->count, memory_order_relaxed);
	return 0;
}

static int capture_owned_file(struct capture_worker *worker, struct capture_node *node, int source)
{
	struct project_capture *capture = worker->capture;
	struct stat previous;
	struct fyai_mobject before = { 0 };
	int target, old = -1, rc, saved;
	bool reusable = false;

	/* The object of the baseline at this path: entry.digest is not yet the new one. */
	if (node->has_baseline && capture->base)
		fyai_manifest_find(capture->base, node->entry.digest, &before);
	if (capture->opts->reuse_baseline && strncmp(node->path, ".git/objects/", 13) &&
	    before.kind == FYAI_PROJECT_FILE && !before.blob.borrowed)
		old = capture_open(capture->opts->previous_baseline_fd, node->path, O_RDONLY, 0);
	if (old >= 0) {
		rc = fstat(old, &previous);
		reusable = !rc && previous.st_size == node->before.st_size &&
			   (previous.st_mode & 07777) == (node->before.st_mode & 07777) &&
			   previous.st_uid == node->metadata.st_uid &&
			   previous.st_gid == node->metadata.st_gid &&
			   previous.st_mtim.tv_sec == node->before.st_mtim.tv_sec &&
			   previous.st_mtim.tv_nsec == node->before.st_mtim.tv_nsec;
		close(old);
	}
	target = capture_open(capture->opts->baseline_fd, node->path, O_RDWR | O_CREAT | O_EXCL,
			      0600);
	if (target < 0)
		return -1;
	rc = fyai_cas_clone_method(source, target, node->before.st_size, &capture->copy,
				   &node->storage);
	if (!rc)
		rc = fyai_cas_hash_file_sized(target, node->before.st_size, &node->blob,
					      worker->hasher);
	/*
	 * Only private captured bytes are hashed; the prior lower is immutable.
	 */
	if (reusable && !rc)
		reusable = node->blob.size == before.blob.size &&
			   !memcmp(node->blob.digest, before.blob.digest, sizeof(node->blob.digest));
	else
		reusable = false;
	if (!rc && !reusable)
		rc = capture_apply_metadata(target, &node->metadata);
	saved = errno;
	if (close(target) < 0 && !rc) {
		rc = -1;
		saved = errno;
	}
	errno = saved;
	if (rc)
		return -1;
	if (reusable) {
		rc = unlinkat(capture->opts->baseline_fd, node->path, 0);
		if (!rc)
			rc = linkat(capture->opts->previous_baseline_fd, node->path,
				    capture->opts->baseline_fd, node->path, 0);
		if (!rc) {
			node->materialized = true;
			node->storage = CAPTURE_STORAGE_HARDLINK;
		}
		return rc;
	}
	node->materialized = true;
	return 0;
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

	files = fyai_scratch_alloc(&capture->scratch, (capture->count + 1) * sizeof(*files));
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
	return rc;
}

/* Store one file in the CAS; the caller closes source. errno names a failure. */
static int capture_file(struct capture_worker *worker, struct capture_node *node, int source)
{
	struct project_capture *capture = worker->capture;
	struct fyai_project_metadata metadata;
	struct stat after;
	int object, rc, saved;

	rc = fstat(source, &after);
	if (rc)
		goto err_out;
	if (!capture_node_same(node, &after))
		goto err_out_eagain;
	rc = capture_xattrs(capture, source);
	if (rc)
		goto err_out;
	if (node->blob.borrowed) {
		rc = fyai_cas_hash_file_sized(source, node->before.st_size, &node->blob,
					      worker->hasher);
		if (rc)
			goto err_out;
		rc = fyai_cas_link_hashed_state(capture->opts->objects_fd, node->source_root,
						node->source_path, &node->blob, worker->hasher,
						source, &capture->copy);
		if (rc)
			goto err_out;
		object = fyai_cas_open(capture->opts->objects_fd, &node->blob);
		if (object < 0)
			goto err_out;
		rc = fstat(object, &after);
		saved = errno;
		close(object);
		errno = saved;
		if (rc)
			goto err_out;
		node->blob.source_device = after.st_dev;
		node->blob.source_inode = after.st_ino;
		node->blob.source_mode = after.st_mode & 07777;
		node->blob.source_uid = after.st_uid;
		node->blob.source_gid = after.st_gid;
		/* Publication adds a link and changes ctime, but not project metadata. */
		rc = fstat(source, &after);
		if (rc)
			goto err_out;
		node->before.st_nlink = after.st_nlink;
		node->before.st_ctim = after.st_ctim;
	} else if (capture->opts->baseline_fd >= 0 && !capture->opts->metacopy) {
		rc = capture_owned_file(worker, node, source);
		if (rc)
			goto err_out;
	} else {
		rc = fyai_cas_put_sized_state(capture->opts->objects_fd, source,
					      node->before.st_size, &node->blob, worker->hasher,
					      &capture->copy);
		if (rc)
			goto err_out;
	}
	rc = fstat(source, &after);
	if (rc)
		goto err_out;
	if (!capture_node_same(node, &after))
		goto err_out_eagain;
	/* The identity of the file, after the last check of the file. */
	metadata = capture_metadata(capture, &node->metadata);
	rc = fyai_project_file_digest(worker->hasher, &metadata, &node->blob, node->entry.digest);
	if (rc)
		goto err_out;
	return 0;
err_out_eagain:
	errno = EAGAIN;
err_out:
	return -1;
}

static void capture_files(void *arg)
{
	struct capture_worker *worker = arg;
	struct project_capture *capture = worker->capture;
	struct capture_node *node;
	size_t index;
	int source, rc;

	for (;;) {
		index = atomic_fetch_add_explicit(&capture->next, 1, memory_order_relaxed);
		if (index >= capture->count)
			break;
		node = &capture->nodes[index];
		if (node->reused || node->entry.kind != FYAI_PROJECT_FILE)
			continue;
		source = capture_node_open(node, O_RDONLY | O_NONBLOCK);
		if (source < 0) {
			node->error = errno;
			continue;
		}
		rc = capture_file(worker, node, source);
		if (rc)
			node->error = errno ? errno : EIO;
		else {
			atomic_fetch_add_explicit(&capture->completed, 1, memory_order_relaxed);
			if (node->materialized)
				atomic_fetch_add_explicit(&capture->storage[node->storage], 1,
							  memory_order_relaxed);
		}
		close(source);
	}
}

/* CAS names expose complete immutable files; durability is a capture-level
 * policy. */
static void capture_publish_owned(void *arg)
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
		if (node->entry.kind != FYAI_PROJECT_FILE || node->error || !node->materialized ||
		    node->blob.borrowed || node->storage == CAPTURE_STORAGE_HARDLINK)
			continue;
		rc = fyai_cas_link_hashed_state(capture->opts->objects_fd,
						capture->opts->baseline_fd, node->path, &node->blob,
						worker->hasher, FYAI_CAS_HASHED_BY_PATH,
						&capture->copy);
		if (rc)
			node->error = errno ? errno : EIO;
	}
}

static int capture_materialize_file(struct project_capture *capture, struct capture_node *node)
{
	struct stat object_stat;
	char redirect[FYAI_CAS_REDIRECT_SIZE], object_name[9 + FYAI_CAS_DIGEST_SIZE];
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
	    object_stat.st_uid == node->metadata.st_uid &&
	    object_stat.st_gid == node->metadata.st_gid &&
	    object_stat.st_mtim.tv_sec == node->before.st_mtim.tv_sec &&
	    object_stat.st_mtim.tv_nsec == node->before.st_mtim.tv_nsec) {
		rc = fyai_cas_name(object_name, sizeof(object_name), &node->blob);
		if (rc)
			goto out;
		rc = linkat(capture->opts->objects_fd, object_name, capture->opts->baseline_fd,
			    node->path, 0);
		if (!rc)
			node->storage = CAPTURE_STORAGE_HARDLINK;
		goto out;
	}
	target = capture_open(capture->opts->baseline_fd, node->path, O_RDWR | O_CREAT | O_EXCL,
			      0600);
	if (target < 0) {
		rc = -1;
		goto out;
	}
	if (capture->opts->metacopy) {
		node->storage = CAPTURE_STORAGE_METACOPY;
		rc = fyai_cas_name(object_name, sizeof(object_name), &node->blob);
		if (rc)
			goto out;
		fyai_cas_redirect(redirect, &node->blob);
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
		rc = fyai_cas_clone_method(object, target, node->blob.size, &capture->copy,
					   &node->storage);
	if (!rc)
		rc = capture_apply_metadata(target, &node->metadata);
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
		else {
			atomic_fetch_add_explicit(&capture->completed, 1, memory_order_relaxed);
			atomic_fetch_add_explicit(&capture->storage[node->storage], 1,
						  memory_order_relaxed);
		}
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
	char redirect[FYAI_CAS_REDIRECT_SIZE], actual[sizeof(redirect)];
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
		source = capture_node_open(node, O_RDONLY | O_NONBLOCK);
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
				fyai_cas_redirect(redirect, &node->blob);
				rc = fstat(baseline, &metadata);
				if (!rc &&
				    (metadata.st_size != (off_t)node->blob.size ||
				     (metadata.st_mode & 07777) != (node->before.st_mode & 07777) ||
				     metadata.st_uid != node->metadata.st_uid ||
				     metadata.st_gid != node->metadata.st_gid ||
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
		atomic_fetch_add_explicit(&capture->completed, 1, memory_order_relaxed);
		capture_progress(capture);
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

/*
 * Finish the nodes that the workers do not: a symlink and a directory get their
 * identity, and with a baseline directory the entry is made in it. A file is
 * done by its worker, which computes its identity after the last check of the
 * file.
 */
static int capture_finish(struct project_capture *capture, size_t index,
			  struct fy_blake3_hasher *hasher)
{
	struct capture_node *node = &capture->nodes[index];
	struct fyai_project_metadata metadata = capture_metadata(capture, &node->metadata);
	struct fyai_project_entry *entries = NULL;
	struct capture_node *child;
	struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, node->before.st_mtim };
	struct stat after;
	char *parent = NULL, *slash;
	const char *name = NULL;
	size_t i;
	int source = -1, target = -1, rc = -1, saved;

	if (node->reused || node->entry.kind == FYAI_PROJECT_FILE)
		return 0;
	if (node->error) {
		errno = node->error;
		goto out;
	}
	if (node->entry.kind == FYAI_PROJECT_SYMLINK) {
		parent = fyai_scratch_strndup(&capture->scratch, node->path, strlen(node->path));
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
		source = capture_open(node->source_root,
				      node->git ? (!strcmp(parent, ".git") ? "" : parent + 5) :
						  parent,
				      O_RDONLY | O_DIRECTORY, 0);
		if (source < 0)
			goto out;
		rc = fstatat(source, name, &after, AT_SYMLINK_NOFOLLOW);
	} else {
		source = capture_node_open(node, O_RDONLY | O_NONBLOCK | O_DIRECTORY);
		if (source < 0)
			goto out;
		rc = fstat(source, &after);
	}
	if (rc)
		goto out;
	if (!capture_node_same(node, &after)) {
		errno = EAGAIN;
		rc = -1;
		goto out;
	}
	if (node->entry.kind == FYAI_PROJECT_SYMLINK) {
		rc = fyai_project_symlink_digest(hasher, &metadata,
						 (const unsigned char *)node->link, strlen(node->link),
						 node->entry.digest);
		if (rc)
			goto out;
		if (capture->opts->baseline_fd >= 0) {
			target = capture_open(capture->opts->baseline_fd, parent,
					      O_RDONLY | O_DIRECTORY, 0);
			if (target < 0) {
				rc = -1;
				goto out;
			}
			rc = symlinkat(node->link, target, name);
			if (!rc)
				rc = fchownat(target, name, node->metadata.st_uid,
					      node->metadata.st_gid, AT_SYMLINK_NOFOLLOW);
			if (!rc)
				rc = utimensat(target, name, times, AT_SYMLINK_NOFOLLOW);
			if (rc)
				goto out;
		}
	} else {
		if (node->count) {
			entries = fyai_scratch_calloc(&capture->scratch, node->count, sizeof(*entries));
			if (!entries) {
				rc = -1;
				goto out;
			}
			for (i = 0; i < node->count; i++) {
				child = &capture->nodes[node->children[i]];
				entries[i] = child->entry;
			}
			rc = fyai_project_entries_prepare(entries, node->count, !index);
			if (rc)
				goto out;
		}
		node->entries = entries;
		if (capture->opts->baseline_fd >= 0) {
			target = capture_open(capture->opts->baseline_fd, node->path,
					      O_RDONLY | O_DIRECTORY, 0);
			if (target < 0) {
				rc = -1;
				goto out;
			}
			if (!index) {
				rc = mkdirat(target, ".fyai", 0700);
				if (rc)
					goto out;
			}
			rc = capture_apply_metadata(target, &node->metadata);
			if (rc)
				goto out;
		}
		rc = fyai_project_directory_digest(hasher, &metadata, entries,
						   node->count, node->entry.digest);
	}
out:
	saved = errno;
	if (source >= 0)
		close(source);
	if (target >= 0)
		close(target);
	if (rc)
		capture_set_error(capture, "%s", *node->path ? node->path : ".");
	errno = saved;
	return rc;
}

/* The nodes of one level are independent: a directory reads only the level below. */
static void capture_finish_level(void *arg)
{
	struct capture_worker *worker = arg;
	struct project_capture *capture = worker->capture;
	size_t index;

	for (;;) {
		index = capture->level_first +
			atomic_fetch_add_explicit(&capture->next, 1, memory_order_relaxed);
		if (index >= capture->level_last)
			break;
		if (capture_finish(capture, index, worker->hasher))
			capture->nodes[index].error = errno ? errno : EIO;
	}
}

/*
 * Finish the directories and symlinks from the deepest level up. A level of many
 * nodes goes to the workers; a small one is not worth a join.
 */
static int capture_finish_all(struct project_capture *capture, struct capture_worker *workers,
			      size_t worker_count)
{
	size_t level, index, count;
	int rc;

	for (level = capture->level_count + 1; level > 0; level--) {
		capture->level_first = capture->levels[level - 1];
		capture->level_last = capture->levels[level];
		count = capture->level_last - capture->level_first;
		if (!workers || count < CAPTURE_FINISH_PARALLEL) {
			for (index = capture->level_last; index > capture->level_first; index--) {
				rc = capture_finish(capture, index - 1, capture->finish_hasher);
				if (rc)
					return rc;
			}
			continue;
		}
		atomic_store_explicit(&capture->next, 0, memory_order_relaxed);
		if (capture_join_workers(capture, capture_finish_level, workers, worker_count))
			return -1;
		for (index = capture->level_first; index < capture->level_last; index++) {
			if (!capture->nodes[index].error)
				continue;
			errno = capture->nodes[index].error;
			return -1;
		}
	}
	return 0;
}

/* The files are checked once more, in parallel, after the last of the work. */
static void capture_recheck_files(void *arg)
{
	struct capture_worker *worker = arg;
	struct project_capture *capture = worker->capture;
	struct capture_node *node;
	struct stat after;
	size_t index;
	int rc;

	for (;;) {
		index = atomic_fetch_add_explicit(&capture->next, 1, memory_order_relaxed);
		if (index >= capture->count)
			break;
		node = &capture->nodes[index];
		if (node->reused || node->error || node->entry.kind != FYAI_PROJECT_FILE)
			continue;
		/*
		 * The file is compared by its identity, which a path lookup gives as an open
		 * descriptor does: a replaced path or file differs in device, inode or time.
		 */
		if (node->staged_fd >= 0)
			rc = fstat(node->staged_fd, &after);
		else
			rc = fstatat(node->source_root, *node->source_path ? node->source_path : ".",
				     &after, AT_SYMLINK_NOFOLLOW);
		if (rc)
			node->error = errno;
		else if (!capture_node_same(node, &after))
			node->error = EAGAIN;
	}
}

/*
 * The objects of the result by digest. A slot holds either a record that is
 * copied from the baseline, or the node whose object is written.
 */
struct capture_slot {
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	bool used;
	const struct capture_node *node;
	const unsigned char *record;
	size_t length;
	/* The timestamp that the object stores. */
	int64_t sec;
	uint32_t nsec;
};

struct capture_map {
	struct capture_slot *slots;
	size_t capacity, count;
};

static struct capture_slot *capture_map_slot(struct capture_map *map,
					     const unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	uint64_t hash;
	size_t slot;

	memcpy(&hash, digest, sizeof(hash));
	slot = (size_t)hash & (map->capacity - 1);
	while (map->slots[slot].used && memcmp(map->slots[slot].digest, digest, FYAI_CAS_HASH_SIZE))
		slot = (slot + 1) & (map->capacity - 1);
	return &map->slots[slot];
}

/* Copy an object of the baseline and, for a directory, everything below it. */
static int capture_reuse(struct project_capture *capture, struct capture_map *map,
			 const unsigned char digest[FYAI_CAS_HASH_SIZE], unsigned int depth)
{
	struct capture_slot *slot = capture_map_slot(map, digest);
	struct fyai_mobject object;
	struct fyai_mdir directory;
	struct fyai_project_entry entry;
	const unsigned char *record;
	size_t length;

	if (slot->used)
		return 0;
	if (depth > FYAI_PROJECT_MAX_DEPTH || map->count >= FYAI_PROJECT_MAX_NODES) {
		errno = E2BIG;
		return -1;
	}
	record = fyai_manifest_record(capture->base, digest, &length);
	if (!record || !fyai_manifest_find(capture->base, digest, &object)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(slot->digest, digest, FYAI_CAS_HASH_SIZE);
	slot->used = true;
	slot->node = NULL;
	slot->record = record;
	slot->length = length;
	slot->sec = object.meta.mtime_sec;
	slot->nsec = object.meta.mtime_nsec;
	map->count++;
	if (object.kind != FYAI_PROJECT_DIRECTORY)
		return 0;
	if (fyai_mdir_open(&object, &directory)) {
		errno = EINVAL;
		return -1;
	}
	while (fyai_mdir_next(&directory, &entry))
		if (capture_reuse(capture, map, entry.digest, depth + 1))
			return -1;
	return 0;
}

/* Hash set of the paths of the nodes that this capture visited. */
static size_t capture_path_hash(const char *path)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (; *path; path++)
		hash = (hash ^ (unsigned char)*path) * UINT64_C(1099511628211);
	return (size_t)hash;
}

static const char *const *capture_path_find(const char *const *set, size_t capacity,
					    const unsigned char *path, size_t length)
{
	size_t slot = 0, i;
	uint64_t hash = UINT64_C(14695981039346656037);

	for (i = 0; i < length; i++)
		hash = (hash ^ path[i]) * UINT64_C(1099511628211);
	slot = (size_t)hash & (capacity - 1);
	while (set[slot]) {
		if (strlen(set[slot]) == length && !memcmp(set[slot], path, length))
			return &set[slot];
		slot = (slot + 1) & (capacity - 1);
	}
	return NULL;
}

/*
 * The objects of the nodes that this capture visited, by digest. The nodes get
 * their indexes in the order that the scan workers add them, which varies from
 * run to run, so nothing here depends on that order. Of the nodes that share a
 * digest the one with the smallest path selects the blob provenance. The
 * baseline object, else the oldest node, selects the stored timestamp.
 */
static int capture_slot_map(struct project_capture *capture, struct capture_map *map)
{
	struct capture_slot *slot;
	struct capture_node *node;
	struct fyai_mobject previous;
	size_t i, length, capacity = 16;
	int64_t sec;
	uint32_t nsec;

	length = (capture->base ? capture->base->object_count : 0) + capture->count;
	if (length > 2 * FYAI_PROJECT_MAX_NODES) {
		errno = E2BIG;
		return -1;
	}
	while (capacity < length * 2)
		capacity *= 2;
	map->capacity = capacity;
	map->slots = fyai_scratch_calloc(&capture->scratch, capacity, sizeof(*map->slots));
	if (!map->slots)
		return -1;
	for (i = 0; i < capture->count; i++) {
		node = &capture->nodes[i];
		if (node->reused)
			continue;
		slot = capture_map_slot(map, node->entry.digest);
		sec = node->metadata.st_mtim.tv_sec;
		nsec = (uint32_t)node->metadata.st_mtim.tv_nsec;
		if (capture->base && fyai_manifest_find(capture->base, node->entry.digest, &previous)) {
			sec = previous.meta.mtime_sec;
			nsec = previous.meta.mtime_nsec;
		} else if (slot->used && (slot->sec < sec || (slot->sec == sec && slot->nsec <= nsec))) {
			sec = slot->sec;
			nsec = slot->nsec;
		}
		if (!slot->used) {
			if (map->count >= FYAI_PROJECT_MAX_NODES) {
				errno = E2BIG;
				return -1;
			}
			memcpy(slot->digest, node->entry.digest, FYAI_CAS_HASH_SIZE);
			slot->used = true;
			map->count++;
		} else if (slot->node && strcmp(node->path, slot->node->path) > 0) {
			node = (struct capture_node *)slot->node;
		}
		slot->node = node;
		slot->record = NULL;
		slot->length = 0;
		slot->sec = sec;
		slot->nsec = nsec;
	}
	return 0;
}

/* The object that a slot of a node stands for. */
static void capture_slot_object(struct project_capture *capture, const struct capture_slot *slot,
				struct fyai_mobject *object)
{
	const struct capture_node *node = slot->node;

	memset(object, 0, sizeof(*object));
	object->kind = node->entry.kind;
	object->meta = capture_metadata(capture, &node->metadata);
	object->meta.mtime_sec = slot->sec;
	object->meta.mtime_nsec = slot->nsec;
	if (node->entry.kind == FYAI_PROJECT_FILE) {
		object->blob = node->blob;
	} else if (node->entry.kind == FYAI_PROJECT_SYMLINK) {
		object->target = (const unsigned char *)node->link;
		object->target_length = strlen(node->link);
	} else {
		object->entry_count = (uint32_t)node->count;
	}
}

/* Assemble the manifest of the result from the nodes and the baseline. */
static int capture_assemble(struct project_capture *capture, struct fyai_manifest *out)
{
	struct fyai_manifest_builder builder;
	struct capture_map map = { 0 };
	struct capture_slot *slot;
	struct capture_node *node;
	struct fyai_mobject object;
	const char **visited = NULL;
	const unsigned char *path;
	size_t i, length, visited_capacity = 16, hole;
	int64_t sec;
	uint32_t nsec;
	bool incremental = capture->opts->incremental;

	if (capture_slot_map(capture, &map) || fyai_manifest_builder_init(&builder, &capture->scratch))
		return -1;
	for (i = 0; i < capture->count; i++) {
		node = &capture->nodes[i];
		if (node->reused && capture_reuse(capture, &map, node->entry.digest, 0))
			return -1;
	}
	for (i = 0; i < map.capacity; i++) {
		slot = &map.slots[i];
		if (!slot->used)
			continue;
		if (slot->record) {
			if (fyai_manifest_builder_add_record(&builder, slot->digest, slot->record,
							     slot->length))
				return -1;
			continue;
		}
		node = (struct capture_node *)slot->node;
		capture_slot_object(capture, slot, &object);
		if (fyai_manifest_builder_add(&builder, slot->digest, &object, node->entries))
			return -1;
	}
	/* Attributes: a time that differs from the one of the object of the path. */
	if (incremental) {
		while (visited_capacity < capture->count * 2)
			visited_capacity *= 2;
		visited = fyai_scratch_calloc(&capture->scratch, visited_capacity, sizeof(*visited));
		if (!visited)
			return -1;
		for (i = 0; i < capture->count; i++) {
			node = &capture->nodes[i];
			if (node->reused)
				continue;
			hole = capture_path_hash(node->path) & (visited_capacity - 1);
			while (visited[hole])
				hole = (hole + 1) & (visited_capacity - 1);
			visited[hole] = node->path;
		}
		for (i = 0; capture->base && i < capture->base->attribute_count; i++) {
			if (!fyai_manifest_attribute_at(capture->base, i, &path, &length, &sec, &nsec))
				return errno = EBADMSG, -1;
			if (capture_path_find((const char *const *)visited, visited_capacity, path, length))
				continue;
			if (fyai_manifest_builder_attribute(&builder, path, length, sec, nsec))
				return -1;
		}
	}
	for (i = 0; i < capture->count; i++) {
		node = &capture->nodes[i];
		if (node->reused)
			continue;
		slot = capture_map_slot(&map, node->entry.digest);
		if (node->before.st_mtim.tv_sec == slot->sec &&
		    (uint32_t)node->before.st_mtim.tv_nsec == slot->nsec)
			continue;
		if (fyai_manifest_builder_attribute(&builder, (const unsigned char *)node->path,
						    strlen(node->path),
						    node->before.st_mtim.tv_sec,
						    (uint32_t)node->before.st_mtim.tv_nsec))
			return -1;
	}
	return fyai_manifest_builder_finish(&builder, capture->nodes[0].entry.digest, out);
}

/* Add a path to a set of paths; the capacity is a power of two and has room. */
static void capture_path_add(const char **set, size_t capacity, const char *path)
{
	size_t slot = capture_path_hash(path) & (capacity - 1);

	while (set[slot])
		slot = (slot + 1) & (capacity - 1);
	set[slot] = path;
}

/*
 * Assemble the result as a delta over the baseline manifest, without building
 * the whole tree. The own records are the objects of the visited nodes that the
 * baseline lacks or holds in another form. A walk of the retained subtrees finds
 * the objects that the tree still has, and the baseline objects that it lacks are
 * removed, so the delta is exact. A path timestamp is removed when its path went
 * away or no longer needs one.
 */
static int capture_assemble_delta(struct project_capture *capture,
				  const unsigned char base_digest[FYAI_CAS_HASH_SIZE],
				  struct fyai_manifest *out)
{
	const struct fyai_manifest *base = capture->base;
	struct fyai_manifest_builder builder;
	struct capture_map map = { 0 };
	struct capture_slot *slot;
	struct capture_node *node;
	struct fyai_mobject object;
	unsigned char digest[FYAI_CAS_HASH_SIZE], *record;
	const unsigned char *stored, *path;
	const char **all = NULL, **reused = NULL;
	size_t i, length, stored_length, size, own = 0, set_capacity = 16, path_length, prefix;
	int64_t sec, base_sec;
	uint32_t nsec, base_nsec;
	bool has;

	if (!base || base->delta || !capture->opts->incremental) {
		errno = EINVAL;
		return -1;
	}
	if (capture_slot_map(capture, &map) || fyai_manifest_builder_init(&builder, &capture->scratch))
		return -1;
	for (i = 0; i < capture->count; i++) {
		node = &capture->nodes[i];
		if (node->reused && capture_reuse(capture, &map, node->entry.digest, 0))
			return -1;
	}
	/* The objects that the visited nodes add or change. */
	for (i = 0; i < map.capacity; i++) {
		slot = &map.slots[i];
		if (!slot->used || slot->record)
			continue;
		node = (struct capture_node *)slot->node;
		capture_slot_object(capture, slot, &object);
		size = fyai_manifest_record_size(&object, node->entries);
		record = fyai_scratch_alloc(&capture->scratch, size);
		if (!size || !record) {
			if (!size)
				errno = EINVAL;
			return -1;
		}
		fyai_manifest_record_put(record, &object, node->entries);
		stored = fyai_manifest_record(base, slot->digest, &stored_length);
		if (stored && stored_length == size && !memcmp(stored, record, size))
			continue;
		if (fyai_manifest_builder_add_record(&builder, slot->digest, record, size))
			return -1;
		own++;
	}
	/* The baseline objects that the tree has not. */
	for (i = 0; i < base->object_count; i++) {
		if (!fyai_manifest_object_at(base, i, digest, &object)) {
			errno = EBADMSG;
			return -1;
		}
		if (!capture_map_slot(&map, digest)->used &&
		    fyai_manifest_builder_remove(&builder, digest))
			return -1;
	}
	/* A delta needs an object of its own; an unchanged tree keeps its root. */
	if (!own) {
		stored = fyai_manifest_record(base, capture->nodes[0].entry.digest, &stored_length);
		if (!stored || fyai_manifest_builder_add_record(&builder,
								capture->nodes[0].entry.digest,
								stored, stored_length)) {
			if (!stored)
				errno = EBADMSG;
			return -1;
		}
	}
	/* Path timestamps. First the paths of the nodes: what each needs, against the base. */
	while (set_capacity < capture->count * 2)
		set_capacity *= 2;
	all = fyai_scratch_calloc(&capture->scratch, set_capacity, sizeof(*all));
	reused = fyai_scratch_calloc(&capture->scratch, set_capacity, sizeof(*reused));
	if (!all || !reused)
		return -1;
	for (i = 0; i < capture->count; i++) {
		node = &capture->nodes[i];
		capture_path_add(all, set_capacity, node->path);
		if (node->reused) {
			capture_path_add(reused, set_capacity, node->path);
			continue;
		}
		slot = capture_map_slot(&map, node->entry.digest);
		path_length = strlen(node->path);
		sec = node->before.st_mtim.tv_sec;
		nsec = (uint32_t)node->before.st_mtim.tv_nsec;
		has = fyai_manifest_attribute(base, (const unsigned char *)node->path, path_length,
					      &base_sec, &base_nsec);
		if (sec != slot->sec || nsec != slot->nsec) {
			if (has && base_sec == sec && base_nsec == nsec)
				continue;
			if (fyai_manifest_builder_attribute(&builder, (const unsigned char *)node->path,
							    path_length, sec, nsec))
				return -1;
		} else if (has &&
			   fyai_manifest_builder_remove_attribute(&builder,
								  (const unsigned char *)node->path,
								  path_length)) {
			return -1;
		}
	}
	/*
	 * Then the timestamps of the base: one is kept when its path was visited or lies
	 * in a retained subtree, and is removed when its path is gone.
	 */
	for (i = 0; i < base->attribute_count; i++) {
		if (!fyai_manifest_attribute_at(base, i, &path, &length, &base_sec, &base_nsec)) {
			errno = EBADMSG;
			return -1;
		}
		if (capture_path_find((const char *const *)all, set_capacity, path, length))
			continue;
		has = false;
		for (prefix = 0; prefix < length && !has; prefix++)
			if (path[prefix] == '/')
				has = capture_path_find((const char *const *)reused, set_capacity, path,
							prefix) != NULL;
		if (!has && fyai_manifest_builder_remove_attribute(&builder, path, length))
			return -1;
	}
	return fyai_manifest_builder_finish_delta(&builder, capture->nodes[0].entry.digest,
						  base_digest, map.count, out);
}

/* With base_name set, the result is a delta over the baseline file of that name. */
static int capture_run(const struct fyai_project_capture_opts *opts, const char *base_name,
		       struct fyai_manifest *manifest, char *error_path, size_t error_size)
{
	unsigned char base_digest[FYAI_CAS_HASH_SIZE];

	struct project_capture capture = { .opts = opts,
					   .git_private = -1,
					   .git_common = -1,
					   .git_pointer = -1,
					   .common_pointer = -1,
					   .error = error_path,
					   .error_size = error_size,
					   .error_lock = PTHREAD_MUTEX_INITIALIZER };
	struct fy_thread_pool_cfg pool_cfg = { .flags = FYTPCF_STEAL_MODE };
	struct fy_blake3_hasher_cfg hash_cfg = { 0 };
	struct fyai_manifest converted = { 0 };
	struct fyai_mobject base_root;
	struct capture_worker *workers = NULL;
	struct capture_map linked = { 0 };
	struct capture_slot *linked_slot;
	struct stat st;
	cpu_set_t affinity;
	size_t root, i, worker_count = 0, initialized = 0, files = 0, capacity = 16;
	int status = -1, rc, saved, cpus, git_check;
	struct stat git_root, checked_root;

	clock_gettime(CLOCK_MONOTONIC, &capture.started);
	capture.euid = geteuid();
	capture.egid = getegid();
	rc = fyai_scratch_open(&capture.scratch);
	if (rc)
		goto out;
	atomic_init(&capture.completed, 0);
	atomic_init(&capture.copy.copied_bytes, 0);
	atomic_init(&capture.copy.reflinked_bytes, 0);
	atomic_init(&capture.copy.backend, FYAI_CAS_METHOD_NONE);
	capture.copy.defer_directory_sync = true;
	capture.copy.defer_file_sync = true;
	for (i = 0; i < CAPTURE_STORAGE_COUNT; i++)
		atomic_init(&capture.storage[i], 0);

	if (error_path && error_size)
		error_path[0] = '\0';
	if (!opts || !manifest || (opts->metacopy && (opts->data_fd < 0 || opts->baseline_fd < 0))) {
		errno = EINVAL;
		goto out;
	}
	/* The baseline is a manifest, or a snapshot that is converted here. */
	if (opts->baseline_manifest) {
		capture.base = opts->baseline_manifest;
	} else if (fy_is_mapping(opts->snapshot)) {
		if (fyai_manifest_from_snapshot(&converted, opts->snapshot))
			goto out;
		capture.base = &converted;
	}
	if (capture.base && (opts->incremental || opts->borrow_git)) {
		rc = fyai_manifest_check_borrowed(capture.base, opts->objects_fd, opts->verify,
						  error_path, error_size);
		if (rc)
			goto out;
	}
	rc = fstat(opts->source_fd, &st);
	if (rc)
		goto out;
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		goto out;
	}
	capture.device = st.st_dev;
	rc = capture_git_prepare(&capture);
	if (rc) {
		if (error_path && error_size)
			snprintf(error_path, error_size,
				 errno == EXDEV ? ".git (worktree metadata must be on "
						  "the project filesystem)" :
						  ".git (cannot resolve external Git "
						  "metadata)");
		goto out;
	}
	atomic_init(&capture.next, 0);
	atomic_init(&capture.count, 0);
	/*
	 * The pool serves the scan, the ingest and the checks. A scan worker adds
	 * nodes, so the array has room for the most that a capture can hold; its
	 * pages are used as the nodes are added.
	 */
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
	capture.nodes = fyai_scratch_alloc(&capture.scratch,
					   (FYAI_PROJECT_MAX_NODES + 1) * sizeof(*capture.nodes));
	if (!capture.nodes)
		goto out;
	capture_progress(&capture);
	rc = capture_add(&capture, SIZE_MAX, "", &st, &root);
	if (rc)
		goto out;
	if (opts->incremental || opts->reuse_baseline) {
		if (!capture.base || (opts->incremental && (opts->upper_fd < 0 || opts->baseline_fd >= 0)) ||
		    !fyai_manifest_find(capture.base, capture.base->root, &base_root) ||
		    base_root.kind != FYAI_PROJECT_DIRECTORY) {
			errno = EINVAL;
			goto out;
		}
		capture.nodes[root].has_baseline = true;
		memcpy(capture.nodes[root].entry.digest, capture.base->root, FYAI_CAS_HASH_SIZE);
	}
	capture.finish_hasher = fy_blake3_hasher_create(&hash_cfg);
	if (!capture.finish_hasher) {
		errno = ENOMEM;
		goto out;
	}
	rc = capture_scan(&capture, root);
	if (!rc)
		rc = capture_links(&capture);
	if (rc)
		goto out;
	for (i = 0; i < capture.count; i++)
		if (!capture.nodes[i].reused && capture.nodes[i].entry.kind == FYAI_PROJECT_FILE)
			files++;
	if (!files)
		goto finish;
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
	capture.stats.workers = worker_count;
	capture.stats.phase = FYAI_PROJECT_INGEST;
	capture_progress(&capture);
	rc = capture_join_workers(&capture, capture_files, workers, worker_count);
	if (rc)
		goto out;
	if (opts->baseline_fd >= 0 && !opts->metacopy) {
		atomic_store_explicit(&capture.next, 0, memory_order_relaxed);
		rc = capture_join_workers(&capture, capture_publish_owned, workers, worker_count);
		if (rc)
			goto out;
	}
	for (i = 0; i < capture.count; i++) {
		if (!capture.nodes[i].error)
			continue;
		if (capture.error && capture.error_size)
			snprintf(capture.error, capture.error_size, "%s", capture.nodes[i].path);
		errno = capture.nodes[i].error;
		goto out;
	}
	/* A borrowed blob is linked into the baseline once: its first node does it. */
	while (capacity < capture.count * 2)
		capacity *= 2;
	linked.capacity = capacity;
	linked.slots = fyai_scratch_calloc(&capture.scratch, capacity, sizeof(*linked.slots));
	if (!linked.slots)
		goto out;
	/* The node of the smallest path links; the choice does not follow the scan order. */
	for (i = 0; i < capture.count; i++) {
		if (!capture.nodes[i].blob.borrowed || capture.nodes[i].error)
			continue;
		linked_slot = capture_map_slot(&linked, capture.nodes[i].blob.digest);
		if (linked_slot->used && strcmp(capture.nodes[i].path, linked_slot->node->path) > 0)
			continue;
		memcpy(linked_slot->digest, capture.nodes[i].blob.digest, FYAI_CAS_HASH_SIZE);
		linked_slot->used = true;
		linked_slot->node = &capture.nodes[i];
	}
	for (i = 0; i < linked.capacity; i++)
		if (linked.slots[i].used)
			((struct capture_node *)linked.slots[i].node)->link_blob = true;
	if (opts->baseline_fd >= 0) {
		atomic_store_explicit(&capture.next, 0, memory_order_relaxed);
		capture.stats.phase = FYAI_PROJECT_MATERIALIZE;
		atomic_store_explicit(&capture.completed, 0, memory_order_relaxed);
		for (i = 0; i < capture.count; i++)
			if (capture.nodes[i].materialized)
				atomic_fetch_add_explicit(&capture.completed, 1,
							  memory_order_relaxed);
		capture_progress(&capture);
		rc = capture_join_workers(&capture, capture_materialize_files, workers,
					  worker_count);
		if (rc)
			goto out;
	}
finish:
	if (opts->verify) {
		capture.stats.phase = FYAI_PROJECT_VERIFY;
		atomic_store_explicit(&capture.completed, 0, memory_order_relaxed);
		capture_progress(&capture);
		rc = capture_verify(&capture);
		if (rc)
			goto out;
	}
	capture.stats.phase = FYAI_PROJECT_MANIFEST;
	capture_progress(&capture);
	if (files) {
		atomic_store_explicit(&capture.next, 0, memory_order_relaxed);
		rc = capture_join_workers(&capture, capture_recheck_files, workers, worker_count);
		if (rc)
			goto out;
		for (i = 0; i < capture.count; i++) {
			if (!capture.nodes[i].error)
				continue;
			if (capture.error && capture.error_size)
				snprintf(capture.error, capture.error_size, "%s",
					 capture.nodes[i].path);
			errno = capture.nodes[i].error;
			goto out;
		}
	}
	rc = capture_finish_all(&capture, workers, worker_count);
	if (rc)
		goto out;
	if (base_name) {
		rc = fyai_cas_digest_parse(base_digest, base_name);
		if (rc)
			errno = EINVAL;
		else
			rc = capture_assemble_delta(&capture, base_digest, manifest);
	} else {
		rc = capture_assemble(&capture, manifest);
	}
	if (rc)
		goto out;
	if (capture.git_private >= 0) {
		git_check = openat(opts->source_fd, capture.git_path,
				   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (git_check < 0) {
			rc = -1;
		} else {
			rc = fstat(git_check, &checked_root);
			if (!rc)
				rc = fstat(capture.git_private, &git_root);
			close(git_check);
		}
		if (rc || checked_root.st_dev != git_root.st_dev ||
		    checked_root.st_ino != git_root.st_ino) {
			if (error_path && error_size)
				snprintf(error_path, error_size,
					 ".git (Git metadata directory "
					 "changed)");
			errno = EAGAIN;
			fyai_manifest_close(manifest);
			goto out;
		}
	}
	if (capture.common_pointer >= 0) {
		rc = fstatat(capture.git_private, "commondir", &st, AT_SYMLINK_NOFOLLOW);
		if (rc || !capture_same(&capture.common_before, &st)) {
			if (error_path && error_size)
				snprintf(error_path, error_size,
					 ".git/commondir (common repository "
					 "pointer changed)");
			errno = EAGAIN;
			fyai_manifest_close(manifest);
			goto out;
		}
	}
	if (capture.git_pointer >= 0) {
		rc = fstatat(opts->source_fd, ".git", &st, AT_SYMLINK_NOFOLLOW);
		if (rc || !capture_same(&capture.git_before, &st)) {
			if (error_path && error_size)
				snprintf(error_path, error_size, ".git (worktree pointer changed)");
			errno = EAGAIN;
			fyai_manifest_close(manifest);
			goto out;
		}
	}
	if (!opts->defer_sync && opts->objects_fd >= 0 && syncfs(opts->objects_fd)) {
		if (error_path && error_size)
			snprintf(error_path, error_size, "CAS persistence barrier");
		fyai_manifest_close(manifest);
		goto out;
	}
	status = 0;
	capture.stats.phase = FYAI_PROJECT_DONE;
	capture_progress(&capture);
out:
	saved = errno;
	for (i = 0; i < initialized; i++)
		fy_blake3_hasher_destroy(workers[i].hasher);
	free(workers);
	if (capture.finish_hasher)
		fy_blake3_hasher_destroy(capture.finish_hasher);
	if (capture.pool)
		fy_thread_pool_destroy(capture.pool);
	for (i = 0; i < capture.count; i++) {
		if (capture.nodes[i].staged_fd >= 0)
			close(capture.nodes[i].staged_fd);
	}
	if (capture.git_pointer >= 0)
		close(capture.git_pointer);
	if (capture.git_private >= 0)
		close(capture.git_private);
	if (capture.git_common >= 0)
		close(capture.git_common);
	if (capture.common_pointer >= 0)
		close(capture.common_pointer);
	fyai_manifest_close(&converted);
	pthread_mutex_destroy(&capture.error_lock);
	fyai_scratch_close(&capture.scratch);
	errno = saved;
	return status;
}

int fyai_project_capture_manifest(const struct fyai_project_capture_opts *opts,
				  struct fyai_manifest *manifest, char *error_path,
				  size_t error_size)
{
	return capture_run(opts, NULL, manifest, error_path, error_size);
}

int fyai_project_capture_delta(const struct fyai_project_capture_opts *opts, const char *base_name,
			       struct fyai_manifest *delta, char *error_path, size_t error_size)
{
	if (!base_name) {
		errno = EINVAL;
		return -1;
	}
	return capture_run(opts, base_name, delta, error_path, error_size);
}

fy_generic fyai_project_capture(struct fy_generic_builder *gb,
				const struct fyai_project_capture_opts *opts, char *error_path,
				size_t error_size)
{
	struct fyai_manifest manifest = { 0 };
	fy_generic snapshot;
	int saved;

	if (!gb) {
		errno = EINVAL;
		return fy_invalid;
	}
	if (fyai_project_capture_manifest(opts, &manifest, error_path, error_size))
		return fy_invalid;
	snapshot = fyai_manifest_to_snapshot(gb, &manifest);
	saved = errno;
	fyai_manifest_close(&manifest);
	errno = saved;
	return snapshot;
}

#else
int fyai_project_capture_manifest(const struct fyai_project_capture_opts *opts,
				  struct fyai_manifest *manifest, char *error_path,
				  size_t error_size)
{
	(void)opts;
	(void)manifest;
	if (error_path && error_size)
		error_path[0] = '\0';
	errno = ENOTSUP;
	return -1;
}

int fyai_project_capture_delta(const struct fyai_project_capture_opts *opts, const char *base_name,
			       struct fyai_manifest *delta, char *error_path, size_t error_size)
{
	(void)opts;
	(void)base_name;
	(void)delta;
	if (error_path && error_size)
		error_path[0] = '\0';
	errno = ENOTSUP;
	return -1;
}

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
