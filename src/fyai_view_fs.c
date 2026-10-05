/*
 * fyai_view_fs.c - file operations of `view cp` and `view rm`
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_view_fs.h"

#ifdef __APPLE__
#define view_fs_mtime(st) ((st)->st_mtimespec)
#else
#define view_fs_mtime(st) ((st)->st_mtim)
#endif

#define VIEW_FS_DEPTH_MAX 128
#define VIEW_FS_CHUNK 65536

/* The kinds of an entry of the stream. The end of the stream is a header of kind 0. */
enum view_fs_kind {
	VIEW_FS_END = 0,
	VIEW_FS_DIRECTORY = 'd',
	VIEW_FS_FILE = 'f',
	VIEW_FS_LINK = 'l',
};

/*
 * One entry: the header, the path of path_length bytes, then size bytes of file
 * data or of a link target. Both ends are one program on one machine, so the
 * integers have the byte order of the host.
 */
struct view_fs_header {
	uint8_t kind;
	uint8_t reserved[3];
	uint32_t mode;
	uint32_t path_length;
	uint32_t reserved2;
	uint64_t size;
	int64_t mtime_sec;
	int64_t mtime_nsec;
};

static void view_fs_error(char *error, size_t size, const char *path)
{
	if (error && size)
		snprintf(error, size, "%s", path && *path ? path : ".");
}

/* A name of a reserved directory: it is never read, written or removed. */
static bool view_fs_reserved(const char *name, size_t length)
{
	return (length == 4 && !memcmp(name, ".git", 4)) || (length == 5 && !memcmp(name, ".fyai", 5));
}

/* Whether a normalized path is made of plain names that are not reserved. */
static bool view_fs_path_safe(const char *path)
{
	const char *p, *end;
	size_t length;

	if (!path || !*path || *path == '/' || strlen(path) >= PATH_MAX)
		return false;
	for (p = path;; p = end + 1) {
		end = strchr(p, '/');
		length = end ? (size_t)(end - p) : strlen(p);
		if (!length || (length == 1 && *p == '.') || (length == 2 && p[0] == '.' && p[1] == '.') ||
		    view_fs_reserved(p, length))
			return false;
		if (!end)
			return true;
	}
}

bool fyai_view_fs_path(const char *in, char *out, size_t size)
{
	size_t length = 0;

	if (!in || !size)
		return false;
	/* Leading "./" and repeated slashes are not part of the name. */
	while (!strncmp(in, "./", 2))
		in += 2;
	for (; *in; in++) {
		if (*in == '/' && length && out[length - 1] == '/')
			continue;
		if (length + 1 >= size)
			return false;
		out[length++] = *in;
	}
	while (length && out[length - 1] == '/')
		length--;
	out[length] = '\0';
	return view_fs_path_safe(out);
}

/*
 * Open the directory that holds a path, walking its names from root without a
 * symlink. With create the directories that are absent are made. The leaf is the
 * last name, which the caller gets in the buffer; the descriptor is the caller's.
 */
static int view_fs_parent(int root, const char *path, bool create, char leaf[NAME_MAX + 1])
{
	char copy[PATH_MAX], *name, *next;
	int fd, child;

	if (strlen(path) >= sizeof(copy)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(copy, path);
	fd = fcntl(root, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return -1;
	for (name = copy;; name = next + 1) {
		next = strchr(name, '/');
		if (!next)
			break;
		*next = '\0';
		child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0 && errno == ENOENT && create && !mkdirat(fd, name, 0755))
			child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(fd);
		if (child < 0)
			return -1;
		fd = child;
	}
	if (strlen(name) > NAME_MAX) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(leaf, name);
	return fd;
}

struct view_fs_pack {
	fyai_view_fs_emit_fn emit;
	void *arg;
	char *error;
	size_t error_size;
	char buffer[VIEW_FS_CHUNK];
};

static int view_fs_emit(struct view_fs_pack *pack, const void *data, size_t length)
{
	return pack->emit && length ? pack->emit(pack->arg, data, length) : 0;
}

/* The name of an entry below a path that the stream renames: the new name, then what follows. */
static int view_fs_name(char *out, size_t size, const char *name, const char *tail)
{
	int n = snprintf(out, size, "%s%s%s", name, *tail ? "/" : "", tail);

	if (n < 0 || (size_t)n >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int view_fs_pack_entry(struct view_fs_pack *pack, int parent, const char *leaf,
			      const char *name, const char *tail, unsigned int depth)
{
	struct view_fs_header header = { 0 };
	struct dirent *entry;
	struct stat st;
	char path[PATH_MAX], target[PATH_MAX], next_tail[PATH_MAX];
	DIR *directory;
	size_t remaining;
	ssize_t n;
	int fd, rc = -1, saved;

	if (depth > VIEW_FS_DEPTH_MAX) {
		errno = ELOOP;
		return -1;
	}
	if (view_fs_name(path, sizeof(path), name, tail) ||
	    fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW))
		return -1;
	header.mode = st.st_mode & 0777;
	header.mtime_sec = view_fs_mtime(&st).tv_sec;
	header.mtime_nsec = view_fs_mtime(&st).tv_nsec;
	header.path_length = (uint32_t)strlen(path);
	if (S_ISDIR(st.st_mode)) {
		header.kind = VIEW_FS_DIRECTORY;
		if (view_fs_emit(pack, &header, sizeof(header)) ||
		    view_fs_emit(pack, path, header.path_length))
			return -1;
		fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return -1;
		directory = fdopendir(fd);
		if (!directory) {
			saved = errno;
			close(fd);
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
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
			    view_fs_reserved(entry->d_name, strlen(entry->d_name)))
				continue;
			if ((size_t)snprintf(next_tail, sizeof(next_tail), "%s%s%s", tail,
					     *tail ? "/" : "", entry->d_name) >= sizeof(next_tail)) {
				errno = ENAMETOOLONG;
				rc = -1;
				break;
			}
			rc = view_fs_pack_entry(pack, dirfd(directory), entry->d_name, name, next_tail,
						depth + 1);
			if (rc)
				break;
		}
		saved = errno;
		closedir(directory);
		errno = saved;
		return rc;
	}
	if (S_ISLNK(st.st_mode)) {
		n = readlinkat(parent, leaf, target, sizeof(target));
		if (n < 0 || (size_t)n == sizeof(target)) {
			if (n >= 0)
				errno = ENAMETOOLONG;
			return -1;
		}
		header.kind = VIEW_FS_LINK;
		header.size = (uint64_t)n;
		if (view_fs_emit(pack, &header, sizeof(header)) ||
		    view_fs_emit(pack, path, header.path_length) || view_fs_emit(pack, target, (size_t)n))
			return -1;
		return 0;
	}
	if (!S_ISREG(st.st_mode)) {
		errno = ENOTSUP;
		return -1;
	}
	header.kind = VIEW_FS_FILE;
	header.size = (uint64_t)st.st_size;
	if (!pack->emit)
		return 0;
	fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = view_fs_emit(pack, &header, sizeof(header)) ||
	     view_fs_emit(pack, path, header.path_length);
	for (remaining = (size_t)st.st_size; !rc && remaining;) {
		n = read(fd, pack->buffer, remaining < sizeof(pack->buffer) ? remaining : sizeof(pack->buffer));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			/* A file that shrank cannot give the size that the header promised. */
			if (!n)
				errno = EIO;
			rc = -1;
			break;
		}
		rc = view_fs_emit(pack, pack->buffer, (size_t)n);
		remaining -= (size_t)n;
	}
	saved = errno;
	close(fd);
	errno = saved;
	return rc ? -1 : 0;
}

int fyai_view_fs_pack(int root, const char *const *paths, const char *const *names, size_t count,
		      fyai_view_fs_emit_fn emit, void *arg, char *error, size_t error_size)
{
	struct view_fs_pack *pack;
	struct view_fs_header end = { .kind = VIEW_FS_END };
	char leaf[NAME_MAX + 1];
	size_t i;
	int parent, rc = -1, saved;

	pack = calloc(1, sizeof(*pack));
	if (!pack)
		return -1;
	pack->emit = emit;
	pack->arg = arg;
	for (i = 0; i < count; i++) {
		view_fs_error(error, error_size, paths[i]);
		parent = view_fs_parent(root, paths[i], false, leaf);
		if (parent < 0)
			goto out;
		rc = view_fs_pack_entry(pack, parent, leaf, names ? names[i] : paths[i], "", 0);
		saved = errno;
		close(parent);
		errno = saved;
		if (rc)
			goto out;
	}
	if (error && error_size)
		*error = '\0';
	rc = view_fs_emit(pack, &end, sizeof(end));
out:
	saved = errno;
	free(pack);
	errno = saved;
	return rc;
}

/* Remove a name and what it holds below a directory. */
static int view_fs_remove_tree(int parent, const char *name, unsigned int depth)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int fd, rc = 0, saved;
	bool removed;

	if (depth > VIEW_FS_DEPTH_MAX) {
		errno = ELOOP;
		return -1;
	}
	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW))
		return -1;
	if (!S_ISDIR(st.st_mode))
		return unlinkat(parent, name, 0);
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	directory = fdopendir(fd);
	if (!directory) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	/* The names that a pass removes are not read again: it starts over until none is left. */
	do {
		removed = false;
		rewinddir(directory);
		while (!rc && (errno = 0, entry = readdir(directory))) {
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
				continue;
			rc = view_fs_remove_tree(dirfd(directory), entry->d_name, depth + 1);
			removed = true;
		}
		if (!rc && errno)
			rc = -1;
	} while (!rc && removed);
	saved = errno;
	closedir(directory);
	errno = saved;
	return rc ? -1 : unlinkat(parent, name, AT_REMOVEDIR);
}

int fyai_view_fs_remove(int root, const char *const *paths, size_t count, bool force, char *error,
			size_t error_size)
{
	char leaf[NAME_MAX + 1];
	struct stat st;
	size_t i;
	int parent, rc, saved;

	for (i = 0; i < count; i++) {
		view_fs_error(error, error_size, paths[i]);
		parent = view_fs_parent(root, paths[i], false, leaf);
		if (parent < 0) {
			if (force && (errno == ENOENT || errno == ENOTDIR))
				continue;
			return -1;
		}
		rc = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
		if (rc && errno == ENOENT && force) {
			close(parent);
			continue;
		}
		if (!rc)
			rc = view_fs_remove_tree(parent, leaf, 0);
		saved = errno;
		close(parent);
		errno = saved;
		if (rc)
			return -1;
	}
	if (error && error_size)
		*error = '\0';
	return 0;
}

struct view_fs_unpack {
	fyai_view_fs_fill_fn fill;
	void *arg;
	char buffer[VIEW_FS_CHUNK];
};

/* Read exactly length bytes. A stream that ends first is truncated (EPIPE). */
static int view_fs_read(struct view_fs_unpack *unpack, void *data, size_t length)
{
	char *p = data;
	long n;

	while (length) {
		n = unpack->fill(unpack->arg, p, length);
		if (n < 0)
			return -1;
		if (!n) {
			errno = EPIPE;
			return -1;
		}
		p += n;
		length -= (size_t)n;
	}
	return 0;
}

/* Consume length bytes into a file, or discard them when fd is negative. */
static int view_fs_copy_in(struct view_fs_unpack *unpack, int fd, uint64_t length)
{
	size_t n, chunk;
	ssize_t written;
	char *p;

	while (length) {
		chunk = n = length < sizeof(unpack->buffer) ? (size_t)length : sizeof(unpack->buffer);
		if (view_fs_read(unpack, unpack->buffer, n))
			return -1;
		for (p = unpack->buffer; fd >= 0 && n;) {
			written = write(fd, p, n);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				return -1;
			p += written;
			n -= (size_t)written;
		}
		length -= chunk;
	}
	return 0;
}

static int view_fs_place_file(struct view_fs_unpack *unpack, int parent, const char *leaf,
			      const struct view_fs_header *header)
{
	struct timespec times[2];
	char temp[64];
	struct stat st;
	int fd, saved;

	snprintf(temp, sizeof(temp), ".fyai-cp-%ld", (long)getpid());
	unlinkat(parent, temp, 0);
	fd = openat(parent, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	if (view_fs_copy_in(unpack, fd, header->size) || fchmod(fd, header->mode & 0777))
		goto fail;
	times[0].tv_sec = times[1].tv_sec = header->mtime_sec;
	times[0].tv_nsec = times[1].tv_nsec = header->mtime_nsec;
	if (futimens(fd, times) || close(fd)) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	/* A directory cannot be renamed over by a file: it goes first. */
	if (!fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode) &&
	    view_fs_remove_tree(parent, leaf, 0))
		goto fail;
	if (renameat(parent, temp, parent, leaf))
		goto fail;
	return 0;
fail:
	saved = errno;
	if (fd >= 0)
		close(fd);
	unlinkat(parent, temp, 0);
	errno = saved;
	return -1;
}

static int view_fs_place(struct view_fs_unpack *unpack, int root, const struct view_fs_header *header,
			 const char *path, char *target)
{
	char leaf[NAME_MAX + 1];
	struct stat st;
	int parent, rc = -1, saved, have;

	parent = view_fs_parent(root, path, true, leaf);
	if (parent < 0)
		return -1;
	have = !fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
	switch (header->kind) {
	case VIEW_FS_DIRECTORY:
		if (have && !S_ISDIR(st.st_mode) && unlinkat(parent, leaf, 0))
			break;
		/* The owner can always write the directory, so the entries below it can be made. */
		if ((!have || !S_ISDIR(st.st_mode)) && mkdirat(parent, leaf, (header->mode & 0777) | 0700))
			break;
		rc = 0;
		break;
	case VIEW_FS_LINK:
		if (have && view_fs_remove_tree(parent, leaf, 0))
			break;
		target[header->size] = '\0';
		rc = symlinkat(target, parent, leaf);
		break;
	case VIEW_FS_FILE:
		rc = view_fs_place_file(unpack, parent, leaf, header);
		break;
	default:
		errno = EPROTO;
	}
	saved = errno;
	close(parent);
	errno = saved;
	return rc;
}

int fyai_view_fs_unpack(int root, fyai_view_fs_fill_fn fill, void *arg, char *error,
			size_t error_size)
{
	struct view_fs_unpack *unpack;
	struct view_fs_header header;
	char path[PATH_MAX], clean[PATH_MAX], target[PATH_MAX];
	int rc = -1, saved;

	unpack = calloc(1, sizeof(*unpack));
	if (!unpack)
		return -1;
	unpack->fill = fill;
	unpack->arg = arg;
	*path = '\0';
	for (;;) {
		if (view_fs_read(unpack, &header, sizeof(header)))
			goto out;
		if (header.kind == VIEW_FS_END)
			break;
		if (!header.path_length || header.path_length >= sizeof(path) ||
		    (header.kind == VIEW_FS_LINK && header.size >= sizeof(target)) ||
		    ((header.kind == VIEW_FS_DIRECTORY) && header.size)) {
			errno = EPROTO;
			goto out;
		}
		if (view_fs_read(unpack, path, header.path_length))
			goto out;
		path[header.path_length] = '\0';
		view_fs_error(error, error_size, path);
		/* The stream is data: a path that is not plain names is refused. */
		if (!fyai_view_fs_path(path, clean, sizeof(clean)) || strcmp(path, clean)) {
			errno = EINVAL;
			goto out;
		}
		if (header.kind == VIEW_FS_LINK && view_fs_read(unpack, target, header.size))
			goto out;
		if (view_fs_place(unpack, root, &header, path, target))
			goto out;
	}
	if (error && error_size)
		*error = '\0';
	rc = 0;
out:
	saved = errno;
	free(unpack);
	errno = saved;
	return rc;
}
