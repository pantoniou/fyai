/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "fyai_project_capture.h"

struct project_capture {
	struct fy_generic_builder *gb;
	const struct fyai_project_capture_opts *opts;
	fy_generic objects;
	dev_t device;
	size_t count;
	char *error;
	size_t error_size;
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

static int capture_copy_blob(int objects, const struct fyai_cas_blob *blob, int target)
{
	int source, saved;
	ssize_t copied;
	uint64_t remaining;

	source = openat(objects, blob->digest, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (source < 0)
		return -1;
	remaining = blob->size;
	while (remaining) {
		copied = sendfile(target, source, NULL,
			remaining > (1U << 30) ? (1U << 30) : (size_t)remaining);
		if (copied < 0 && errno == EINTR)
			continue;
		if (copied <= 0) {
			if (!copied)
				errno = EIO;
			saved = errno;
			close(source);
			errno = saved;
			return -1;
		}
		remaining -= (uint64_t)copied;
	}
	close(source);
	return 0;
}

static fy_generic capture_directory(struct project_capture *capture, int source,
				    int target, const char *path, unsigned int depth,
				    char digest[FYAI_CAS_DIGEST_SIZE]);

static fy_generic capture_child(struct project_capture *capture, int source,
				int target, const char *name, const char *path,
				unsigned int depth, struct fyai_project_entry *entry)
{
	struct stat before, after;
	struct fyai_project_metadata metadata;
	struct fyai_cas_blob blob;
	struct timespec times[2];
	fy_generic object = fy_invalid;
	char link[PATH_MAX];
	ssize_t length;
	int fd = -1, output = -1, rc, saved;

	rc = fstatat(source, name, &before, AT_SYMLINK_NOFOLLOW);
	if (rc)
		goto out;
	if (before.st_dev != capture->device || ++capture->count > 100000) {
		errno = EXDEV;
		goto out;
	}
	metadata = capture_metadata(capture, &before);
	if (S_ISLNK(before.st_mode)) {
		length = readlinkat(source, name, link, sizeof(link));
		if (length < 0)
			goto out;
		if ((size_t)length == sizeof(link)) {
			errno = ENAMETOOLONG;
			goto out;
		}
		entry->kind = FYAI_PROJECT_SYMLINK;
		object = fyai_project_symlink(capture->gb, &metadata,
			(const unsigned char *)link, length, entry->digest);
		if (!fy_is_valid(object))
			goto out;
		if (target >= 0) {
			link[length] = '\0';
			rc = symlinkat(link, target, name);
			if (rc)
				goto out;
			rc = fchownat(target, name, before.st_uid, before.st_gid, AT_SYMLINK_NOFOLLOW);
			if (rc)
				goto out;
			times[0] = (struct timespec){ .tv_nsec = UTIME_OMIT };
			times[1] = before.st_mtim;
			rc = utimensat(target, name, times, AT_SYMLINK_NOFOLLOW);
			if (rc)
				goto out;
		}
	} else if (S_ISREG(before.st_mode) || S_ISDIR(before.st_mode)) {
		fd = openat(source, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
			(S_ISDIR(before.st_mode) ? O_DIRECTORY : 0));
		if (fd < 0)
			goto out;
		rc = fstat(fd, &after);
		if (rc)
			goto out;
		if (!capture_same(&before, &after)) {
			errno = EAGAIN;
			goto out;
		}
		rc = capture_xattrs(capture, fd);
		if (rc)
			goto out;
		if (S_ISDIR(before.st_mode)) {
			entry->kind = FYAI_PROJECT_DIRECTORY;
			if (target >= 0) {
				rc = mkdirat(target, name, 0700);
				if (rc)
					goto out;
				output = openat(target, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
				if (output < 0)
					goto out;
			}
			object = capture_directory(capture, fd, output, path, depth + 1, entry->digest);
			if (!fy_is_valid(object))
				goto out;
		} else {
			if (before.st_nlink != 1 || (before.st_mode & 06000)) {
				errno = ENOTSUP;
				goto out;
			}
			entry->kind = FYAI_PROJECT_FILE;
			rc = fyai_cas_put(capture->opts->objects_fd, fd, &blob);
			if (rc)
				goto out;
			object = fyai_project_file(capture->gb, &metadata, &blob, entry->digest);
			if (!fy_is_valid(object))
				goto out;
			if (target >= 0) {
				output = openat(target, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
				if (output < 0)
					goto out;
				rc = capture_copy_blob(capture->opts->objects_fd, &blob, output);
				if (!rc)
					rc = capture_apply(output, &before);
				if (rc)
					goto out;
			}
		}
	} else {
		errno = ENOTSUP;
		goto out;
	}
	rc = fstatat(source, name, &after, AT_SYMLINK_NOFOLLOW);
	if (rc)
		goto out;
	if (!capture_same(&before, &after)) {
		errno = EAGAIN;
		goto out;
	}
	if (fd >= 0) {
		rc = fstat(fd, &after);
		if (rc)
			goto out;
		if (!capture_same(&before, &after)) {
			errno = EAGAIN;
			goto out;
		}
	}
	if (output >= 0)
		close(output);
	if (fd >= 0)
		close(fd);
	return object;
out:
	saved = errno;
	if (capture->error && capture->error_size && !capture->error[0])
		snprintf(capture->error, capture->error_size, "%s", path);
	if (output >= 0)
		close(output);
	if (fd >= 0)
		close(fd);
	errno = saved;
	return fy_invalid;
}

static fy_generic capture_directory(struct project_capture *capture, int source,
				    int target, const char *path, unsigned int depth,
				    char digest[FYAI_CAS_DIGEST_SIZE])
{
	struct fyai_project_entry *entries = NULL, *grown;
	struct stat before, after;
	struct fyai_project_metadata metadata;
	struct dirent *dent;
	DIR *directory = NULL;
	fy_generic object, result = fy_invalid;
	char child_path[PATH_MAX];
	size_t count = 0, capacity = 0, i;
	int fd, rc, saved;

	if (depth > 128) {
		errno = ELOOP;
		return fy_invalid;
	}
	rc = fstat(source, &before);
	if (rc)
		return fy_invalid;
	rc = capture_xattrs(capture, source);
	if (rc)
		return fy_invalid;
	fd = openat(source, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return fy_invalid;
	directory = fdopendir(fd);
	if (!directory) {
		close(fd);
		return fy_invalid;
	}
	for (;;) {
		errno = 0;
		dent = readdir(directory);
		if (!dent) {
			if (errno)
				goto out;
			break;
		}
		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..") ||
		    (!depth && !strcmp(dent->d_name, ".fyai")))
			continue;
		if (count == capacity) {
			capacity = capacity ? capacity * 2 : 16;
			grown = realloc(entries, capacity * sizeof(*entries));
			if (!grown)
				goto out;
			entries = grown;
		}
		memset(&entries[count], 0, sizeof(entries[count]));
		entries[count].name = (unsigned char *)strdup(dent->d_name);
		if (!entries[count].name)
			goto out;
		entries[count].name_length = strlen(dent->d_name);
		rc = snprintf(child_path, sizeof(child_path), "%s%s%s", path,
			      *path ? "/" : "", dent->d_name);
		if (rc < 0 || rc >= (int)sizeof(child_path)) {
			free((void *)entries[count].name);
			errno = ENAMETOOLONG;
			goto out;
		}
		object = capture_child(capture, source, target, dent->d_name,
				       child_path, depth, &entries[count]);
		if (!fy_is_valid(object)) {
			free((void *)entries[count].name);
			goto out;
		}
		capture->objects = fy_assoc(capture->gb, capture->objects,
			fy_value(capture->gb, entries[count].digest), object);
		count++;
		if (!fy_is_valid(capture->objects)) {
			errno = ENOMEM;
			goto out;
		}
	}
	rc = fstat(source, &after);
	if (rc)
		goto out;
	if (!capture_same(&before, &after)) {
		errno = EAGAIN;
		goto out;
	}
	if (target >= 0) {
		if (!depth) {
			rc = mkdirat(target, ".fyai", 0700);
			if (rc)
				goto out;
		}
		rc = capture_apply(target, &before);
		if (rc)
			goto out;
	}
	metadata = capture_metadata(capture, &before);
	result = fyai_project_directory(capture->gb, &metadata, entries, count, !depth, digest);
out:
	saved = errno;
	for (i = 0; i < count; i++)
		free((void *)entries[i].name);
	free(entries);
	closedir(directory);
	if (!fy_is_valid(result) && capture->error && capture->error_size && !capture->error[0])
		snprintf(capture->error, capture->error_size, "%s", *path ? path : ".");
	errno = saved;
	return result;
}

fy_generic fyai_project_capture(struct fy_generic_builder *gb,
		const struct fyai_project_capture_opts *opts,
		char *error_path, size_t error_size)
{
	struct project_capture capture = { .gb = gb, .opts = opts,
		.objects = fy_invalid, .error = error_path, .error_size = error_size };
	struct stat st;
	fy_generic root;
	char digest[FYAI_CAS_DIGEST_SIZE];
	int rc;

	if (error_path && error_size)
		error_path[0] = '\0';
	rc = fstat(opts->source_fd, &st);
	if (rc)
		return fy_invalid;
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return fy_invalid;
	}
	capture.device = st.st_dev;
	capture.objects = fy_mapping(gb);
	root = capture_directory(&capture, opts->source_fd, opts->baseline_fd, "", 0, digest);
	if (!fy_is_valid(root))
		return fy_invalid;
	capture.objects = fy_assoc(gb, capture.objects, fy_value(gb, digest), root);
	return fy_mapping(gb, "version", 1LL, "algorithm", "blake3",
		"root", fy_value(gb, digest), "objects", capture.objects);
}
