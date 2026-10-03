/*
 * fyai_project_test.c - unit tests for project manifests and capture
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif
#include <unistd.h>

#include "fyai_project.h"
#include "fyai_project_capture.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(project, borrowed_git, project_borrowed_git)
FYAI_TEST_ENTRY(project, identity_attributes, project_identity_attributes)
FYAI_TEST_ENTRY(project, directory_order, project_directory_order)
FYAI_TEST_ENTRY(project, directory_ancestors, project_directory_ancestors)
FYAI_TEST_ENTRY(project, capture_parallel, project_capture_parallel)
FYAI_TEST_ENTRY(project, directory_validation, project_directory_validation)

static struct fy_generic_builder *project_builder(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&cfg);
}

static void project_entry(struct fyai_project_entry *entry, const char *name,
			  enum fyai_project_kind kind, char value)
{
	entry->name = (const unsigned char *)name;
	entry->name_length = strlen(name);
	entry->kind = kind;
	memset(entry->digest, value, FYAI_CAS_DIGEST_SIZE - 1);
	entry->digest[FYAI_CAS_DIGEST_SIZE - 1] = '\0';
}

int project_identity_attributes(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0644, .uid = 1000, .gid = 1000 };
	struct fyai_cas_blob blob = { .digest =
					      "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd35"
					      "9c6cd5bd9d85",
				      .size = 3 };
	fy_generic first, second;
	char a[FYAI_CAS_DIGEST_SIZE], b[FYAI_CAS_DIGEST_SIZE];

	gb = project_builder();
	FYAI_TCHECK(gb);
	first = fyai_project_file(gb, &meta, &blob, a);
	meta.mtime_sec = 123;
	meta.mtime_nsec = 456;
	second = fyai_project_file(gb, &meta, &blob, b);
	FYAI_TCHECK(fy_is_mapping(first) && fy_is_mapping(second));
	FYAI_TCHECK(!strcmp(a, b) && !fy_equal(first, second));
	meta.mode = 0755;
	second = fyai_project_file(gb, &meta, &blob, b);
	FYAI_TCHECK(fy_is_mapping(second) && strcmp(a, b));
	meta.mode = 0644;
	meta.uid++;
	second = fyai_project_file(gb, &meta, &blob, b);
	FYAI_TCHECK(fy_is_mapping(second) && strcmp(a, b));
	meta.uid--;
	meta.mtime_sec = 0;
	meta.mtime_nsec = 0;
	first = fyai_project_symlink(gb, &meta, (const unsigned char *)"abc", 3, a);
	meta.mtime_sec = 123;
	second = fyai_project_symlink(gb, &meta, (const unsigned char *)"abc", 3, b);
	FYAI_TCHECK(fy_is_mapping(first) && fy_is_mapping(second) && !strcmp(a, b));
	first = fyai_project_directory(gb, &meta, NULL, 0, true, a);
	meta.mtime_sec++;
	second = fyai_project_directory(gb, &meta, NULL, 0, true, b);
	FYAI_TCHECK(fy_is_mapping(first) && fy_is_mapping(second) && !strcmp(a, b));
	fy_generic_builder_destroy(gb);
	return 0;
}

int project_directory_order(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0755, .mtime_sec = -1 };
	struct fyai_project_entry entries[3], reverse[3];
	char first[FYAI_CAS_DIGEST_SIZE], second[FYAI_CAS_DIGEST_SIZE];
	fy_generic a, b, children, item;

	gb = project_builder();
	FYAI_TCHECK(gb);
	project_entry(&entries[0], "z", FYAI_PROJECT_FILE, 'a');
	project_entry(&entries[1], "\xff", FYAI_PROJECT_SYMLINK, 'b');
	project_entry(&entries[2], "a", FYAI_PROJECT_DIRECTORY, 'c');
	reverse[0] = entries[2];
	reverse[1] = entries[1];
	reverse[2] = entries[0];
	a = fyai_project_directory(gb, &meta, entries, 3, true, first);
	b = fyai_project_directory(gb, &meta, reverse, 3, true, second);
	FYAI_TCHECK(fy_is_valid(a) && fy_is_valid(b));
	FYAI_TCHECK(fy_equal(a, b) && !strcmp(first, second));
	FYAI_TCHECK(fy_is_invalid(fy_get(a, "blob", fy_invalid)));
	FYAI_TCHECK(fy_is_invalid(fy_get(a, "content", fy_invalid)));
	children = fy_get(a, "entries", fy_invalid);
	item = fy_get_at(children, 0);
	FYAI_TCHECK(fy_equal(fy_get(item, "name_hex", ""), "61"));
	item = fy_get_at(children, 2);
	FYAI_TCHECK(fy_equal(fy_get(item, "name_hex", ""), "ff"));
	memset(entries, 0, sizeof(entries));
	FYAI_TCHECK(fy_equal(a, b));
	fy_generic_builder_destroy(gb);
	return 0;
}

int project_directory_ancestors(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0755 };
	struct fyai_project_entry leaf, parent;
	char child1[FYAI_CAS_DIGEST_SIZE], child2[FYAI_CAS_DIGEST_SIZE];
	char root1[FYAI_CAS_DIGEST_SIZE], root2[FYAI_CAS_DIGEST_SIZE];
	fy_generic result;

	gb = project_builder();
	FYAI_TCHECK(gb);
	project_entry(&leaf, "file", FYAI_PROJECT_FILE, 'a');
	result = fyai_project_directory(gb, &meta, &leaf, 1, false, child1);
	FYAI_TCHECK(fy_is_valid(result));
	project_entry(&parent, "src", FYAI_PROJECT_DIRECTORY, '0');
	memcpy(parent.digest, child1, sizeof(parent.digest));
	result = fyai_project_directory(gb, &meta, &parent, 1, true, root1);
	FYAI_TCHECK(fy_is_valid(result));
	leaf.digest[0] = 'b';
	result = fyai_project_directory(gb, &meta, &leaf, 1, false, child2);
	FYAI_TCHECK(fy_is_valid(result) && strcmp(child1, child2));
	memcpy(parent.digest, child2, sizeof(parent.digest));
	result = fyai_project_directory(gb, &meta, &parent, 1, true, root2);
	FYAI_TCHECK(fy_is_valid(result) && strcmp(root1, root2));
	result = fyai_project_directory(gb, &meta, NULL, 0, true, root1);
	FYAI_TCHECK(fy_is_valid(result));
	FYAI_TCHECK(!strcmp(root1, "f31f8b8d27fcfb0f4c470eb80505932781dbeef01d6"
				   "3d79f57674f3855a115dc"));
	meta.mode = 0700;
	result = fyai_project_directory(gb, &meta, NULL, 0, true, root2);
	FYAI_TCHECK(fy_is_valid(result) && strcmp(root1, root2));
	fy_generic_builder_destroy(gb);
	return 0;
}

int project_directory_validation(void)
{
	static const char *const bad_names[] = { "", ".", "..", "a/b", ".fyai" };
	struct fy_generic_builder *gb;
	struct fyai_project_metadata meta = { .mode = 0755 };
	struct fyai_project_entry entries[2];
	char digest[FYAI_CAS_DIGEST_SIZE];
	fy_generic result;
	size_t i;

	gb = project_builder();
	FYAI_TCHECK(gb);
	for (i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
		project_entry(&entries[0], bad_names[i], FYAI_PROJECT_FILE, 'a');
		result = fyai_project_directory(gb, &meta, entries, 1, true, digest);
		FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	}
	project_entry(&entries[0], ".fyai", FYAI_PROJECT_DIRECTORY, 'a');
	result = fyai_project_directory(gb, &meta, entries, 1, false, digest);
	FYAI_TCHECK(fy_is_valid(result));
	project_entry(&entries[0], "same", FYAI_PROJECT_FILE, 'a');
	entries[1] = entries[0];
	result = fyai_project_directory(gb, &meta, entries, 2, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EEXIST);
	entries[0].name = (const unsigned char *)"a\0b";
	entries[0].name_length = 3;
	result = fyai_project_directory(gb, &meta, entries, 1, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	project_entry(&entries[0], "ok", FYAI_PROJECT_FILE, 'g');
	result = fyai_project_directory(gb, &meta, entries, 1, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	meta.mtime_nsec = 1000000000;
	result = fyai_project_directory(gb, &meta, NULL, 0, true, digest);
	FYAI_TCHECK(fy_is_invalid(result) && errno == EINVAL);
	fy_generic_builder_destroy(gb);
	return 0;
}

#ifdef __linux__
static void project_remove_tree(int fd)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int child;

	directory = fdopendir(dup(fd));
	FYAI_TCHECK(directory);
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		FYAI_TCHECK(!fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW));
		if (S_ISDIR(st.st_mode)) {
			child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			FYAI_TCHECK(child >= 0);
			project_remove_tree(child);
			close(child);
			FYAI_TCHECK(!unlinkat(fd, entry->d_name, AT_REMOVEDIR));
		} else
			FYAI_TCHECK(!unlinkat(fd, entry->d_name, 0));
	}
	closedir(directory);
}

int project_borrowed_git(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_capture_opts opts = { .borrow_git = true,
						  .verify = true,
						  .workers = 4 };
	fy_generic snapshot;
	char path[] = "/tmp/fyai-project-borrowed-XXXXXX", error[PATH_MAX];
	struct stat host, baseline, other;
	int root, git, source, fd, rc;

	FYAI_TCHECK(mkdtemp(path));
	root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(root >= 0);
	FYAI_TCHECK(!mkdirat(root, "source", 0700) && !mkdirat(root, "objects", 0700) &&
		    !mkdirat(root, "baseline", 0700));
	opts.source_fd = openat(root, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	opts.objects_fd = openat(root, "objects", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	opts.baseline_fd = openat(root, "baseline", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(opts.source_fd >= 0 && opts.objects_fd >= 0 && opts.baseline_fd >= 0);
	FYAI_TCHECK(!mkdirat(opts.source_fd, ".git", 0700));
	git = openat(opts.source_fd, ".git", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(git >= 0 && !mkdirat(git, "objects", 0700));
	source = openat(git, "objects", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(source >= 0);
	fd = openat(source, "a", O_RDWR | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(fd >= 0 && write(fd, "abc", 3) == 3 && !fchmod(fd, 0444));
	FYAI_TCHECK(!linkat(source, "a", source, "b", 0));
	close(fd);
	gb = project_builder();
	FYAI_TCHECK(gb);
	snapshot = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(snapshot));
	FYAI_TCHECK(!fyai_project_verify_borrowed(opts.objects_fd, snapshot, error, sizeof(error)));
	FYAI_TCHECK(!fstatat(source, "a", &host, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(!fstatat(opts.baseline_fd, ".git/objects/a", &baseline, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(!fstatat(opts.baseline_fd, ".git/objects/b", &other, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(baseline.st_ino != other.st_ino);
	FYAI_TCHECK(host.st_dev == baseline.st_dev &&
		    (host.st_ino == baseline.st_ino || host.st_ino == other.st_ino));
	/* Rename and deletion leave the borrowed inode available. */
	FYAI_TCHECK(!unlinkat(source, "a", 0));
	FYAI_TCHECK(!fyai_project_verify_borrowed(opts.objects_fd, snapshot, error, sizeof(error)));
	FYAI_TCHECK(!fchmodat(source, "b", 0644, 0));
	rc = fyai_project_verify_borrowed(opts.objects_fd, snapshot, error, sizeof(error));
	FYAI_TCHECK(rc < 0 && errno == EIO);
	FYAI_TCHECK(!fchmodat(source, "b", 0444, 0));
	fd = openat(source, "b", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		FYAI_TCHECK(!fchmodat(source, "b", 0644, 0));
		fd = openat(source, "b", O_WRONLY | O_CLOEXEC);
		FYAI_TCHECK(fd >= 0 && !fchmod(fd, 0444));
	}
	FYAI_TCHECK(fd >= 0 && write(fd, "bad", 3) == 3);
	close(fd);
	rc = fyai_project_verify_borrowed(opts.objects_fd, snapshot, error, sizeof(error));
	FYAI_TCHECK(rc < 0 && errno == EIO && strstr(error, "--copy-git-objects"));
	fy_generic_builder_destroy(gb);
	close(source);
	close(git);
	close(opts.source_fd);
	close(opts.objects_fd);
	close(opts.baseline_fd);
	project_remove_tree(root);
	close(root);
	FYAI_TCHECK(!rmdir(path));
	return 0;
}

int project_capture_parallel(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_capture_opts opts = { .verify = true, .workers = 1 };
	fy_generic first, second, incremental, refreshed, attrs;
	struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = 123, .tv_nsec = 456 } };
	char path[] = "/tmp/fyai-project-parallel-XXXXXX", name[32], error[PATH_MAX];
	unsigned char *data, *mapped;
	size_t i, j, size = 2U * 1024U * 1024U + 19;
	struct stat st, blob_stat, linked_stat;
	struct dirent *de;
	DIR *directory;
	fy_generic linked;
	int root, fd, rc, lower[2], objects, linked_lower, data_directory;
	bool shared = false;

	FYAI_TCHECK(mkdtemp(path));
	root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(root >= 0);
	FYAI_TCHECK(!mkdirat(root, "source", 0700));
	FYAI_TCHECK(!mkdirat(root, "objects", 0700));
	FYAI_TCHECK(!mkdirat(root, "one", 0700));
	FYAI_TCHECK(!mkdirat(root, "many", 0700));
	opts.source_fd = openat(root, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	opts.objects_fd = openat(root, "objects", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	lower[0] = openat(root, "one", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	lower[1] = openat(root, "many", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(opts.source_fd >= 0 && opts.objects_fd >= 0 && lower[0] >= 0 && lower[1] >= 0);
	data = malloc(size);
	FYAI_TCHECK(data);
	for (j = 0; j < size; j++)
		data[j] = (unsigned char)(j * 131 + j / 4096);
	for (i = 0; i < 32; i++) {
		rc = snprintf(name, sizeof(name), "file-%02zu", i);
		FYAI_TCHECK(rc > 0 && rc < (int)sizeof(name));
		fd = openat(opts.source_fd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
		FYAI_TCHECK(fd >= 0);
		/* Repeated bytes exercise concurrent publication of one CAS
		 * identity. */
		FYAI_TCHECK(write(fd, data, i < 8 ? size : 4096) == (ssize_t)(i < 8 ? size : 4096));
		close(fd);
	}
	FYAI_TCHECK(!fchmodat(opts.source_fd, "file-01", 0755, 0));
	FYAI_TCHECK(!linkat(opts.source_fd, "file-00", root, "external", 0));
	fd = openat(opts.source_fd, "zero", O_WRONLY | O_CREAT | O_EXCL, 0644);
	FYAI_TCHECK(fd >= 0);
	close(fd);
	FYAI_TCHECK(!mkdirat(opts.source_fd, "empty", 0700));
	FYAI_TCHECK(!symlinkat("file-00", opts.source_fd, "link"));
	FYAI_TCHECK(!mkdirat(opts.source_fd, ".fyai", 0700));
	FYAI_TCHECK(!mkdirat(opts.source_fd, "empty/.fyai", 0700));
	gb = project_builder();
	FYAI_TCHECK(gb);
	opts.baseline_fd = lower[0];
	first = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(first));
	opts.baseline_fd = lower[1];
	opts.workers = 4;
	opts.reuse_baseline = true;
	opts.previous_baseline_fd = lower[0];
	opts.snapshot = first;
	second = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(second) && fy_equal(first, second));
	FYAI_TCHECK(!fstatat(lower[0], "file-00", &st, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(!fstatat(lower[1], "file-00", &linked_stat, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(st.st_ino == linked_stat.st_ino && st.st_dev == linked_stat.st_dev);
	FYAI_TCHECK(!fstatat(lower[0], "file-02", &linked_stat, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(st.st_ino != linked_stat.st_ino);
	opts.reuse_baseline = false;
	FYAI_TCHECK(!mkdirat(root, "upper", 0700));
	opts.upper_fd = openat(root, "upper", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(opts.upper_fd >= 0);
	opts.incremental = true;
	opts.snapshot = first;
	opts.baseline_fd = -1;
	opts.verify = false;
	objects = opts.objects_fd;
	/* Reused lower files require no CAS descriptor or content ingestion. */
	opts.objects_fd = -1;
	incremental = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(incremental) && fy_equal(first, incremental));
	opts.objects_fd = objects;
	close(opts.upper_fd);
	for (i = 0; i < 2; i++) {
		fd = openat(lower[i], "file-00", O_RDONLY | O_NOFOLLOW);
		FYAI_TCHECK(fd >= 0 && !fstat(fd, &st) && st.st_size == (off_t)size &&
			    st.st_nlink >= 1);
		mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		FYAI_TCHECK(mapped != MAP_FAILED && !memcmp(mapped, data, size));
		munmap(mapped, size);
		close(fd);
		FYAI_TCHECK(!fstatat(lower[i], "empty", &st, AT_SYMLINK_NOFOLLOW) &&
			    S_ISDIR(st.st_mode));
		FYAI_TCHECK(!fstatat(lower[i], "link", &st, AT_SYMLINK_NOFOLLOW) &&
			    S_ISLNK(st.st_mode));
	}
	/*
	 * Distinct metadata inodes share CAS data without copying file bytes.
	 */
	directory = fdopendir(dup(objects));
	FYAI_TCHECK(directory);
	while ((de = readdir(directory))) {
		FYAI_TCHECK(!fstatat(objects, de->d_name, &blob_stat, AT_SYMLINK_NOFOLLOW));
		if (S_ISREG(blob_stat.st_mode) && blob_stat.st_size == (off_t)size)
			break;
	}
	FYAI_TCHECK(de);
	for (i = 0; i < 8; i++) {
		snprintf(name, sizeof(name), "file-%02zu", i);
		FYAI_TCHECK(!fstatat(lower[0], name, &st, AT_SYMLINK_NOFOLLOW));
		if (st.st_ino == blob_stat.st_ino && st.st_dev == blob_stat.st_dev)
			shared = true;
	}
	FYAI_TCHECK(shared);
	FYAI_TCHECK(!mkdirat(root, "metadata", 0700) && !mkdirat(root, "data", 0700));
	linked_lower = openat(root, "metadata", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	data_directory = openat(root, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(linked_lower >= 0 && data_directory >= 0);
	opts.incremental = false;
	opts.baseline_fd = linked_lower;
	opts.data_fd = data_directory;
	opts.metacopy = true;
	opts.verify = true;
	linked = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(linked) && fy_equal(first, linked));
	FYAI_TCHECK(!fstatat(data_directory, de->d_name, &st, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(st.st_ino == blob_stat.st_ino && st.st_dev == blob_stat.st_dev);
	FYAI_TCHECK(st.st_mode == blob_stat.st_mode &&
		    st.st_mtim.tv_sec == blob_stat.st_mtim.tv_sec &&
		    st.st_mtim.tv_nsec == blob_stat.st_mtim.tv_nsec);
	closedir(directory);
	FYAI_TCHECK(!fstatat(linked_lower, "file-00", &linked_stat, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(linked_stat.st_ino != blob_stat.st_ino && linked_stat.st_size == (off_t)size &&
		    (linked_stat.st_mode & 07777) == 0600);
	FYAI_TCHECK(!fstatat(linked_lower, "file-01", &st, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(st.st_ino != linked_stat.st_ino && (st.st_mode & 07777) == 0755);
	fd = openat(linked_lower, "file-00", O_RDONLY | O_NOFOLLOW);
	FYAI_TCHECK(fd >= 0 && fgetxattr(fd, "user.overlay.metacopy", NULL, 0) == 0);
	errno = 0;
	FYAI_TCHECK(lseek(fd, 0, SEEK_DATA) == -1 && errno == ENXIO);
	close(fd);
	/* Timestamp-only edits retain the content root and create one sparse
	 * override. */
	FYAI_TCHECK(!utimensat(opts.source_fd, "file-00", times, 0));
	opts.metacopy = false;
	opts.baseline_fd = -1;
	opts.snapshot = first;
	refreshed = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(refreshed));
	FYAI_TCHECK(
		fy_equal(fy_get(first, "root", fy_invalid), fy_get(refreshed, "root", fy_invalid)));
	attrs = fy_get(refreshed, "attributes", fy_invalid);
	FYAI_TCHECK(fy_is_mapping(fy_get(attrs, "66696c652d3030", fy_invalid)));
	FYAI_TCHECK(!fyai_project_snapshot_equal(first, refreshed));
	FYAI_TCHECK(fyai_project_snapshot_equal(refreshed, refreshed));
	FYAI_TCHECK(!unlinkat(opts.source_fd, "file-00", 0));
	FYAI_TCHECK(!linkat(opts.source_fd, "file-02", opts.source_fd, "alias", 0));
	/* Unsupported project hard links are still rejected by this phase. */
	second = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_invalid(second) && errno == ENOTSUP);
	FYAI_TCHECK(!unlinkat(opts.source_fd, "alias", 0));
	second = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(second));
	FYAI_TCHECK(fy_is_invalid(
		fy_get(fy_get(second, "attributes", fy_invalid), "66696c652d3030", fy_invalid)));
	close(linked_lower);
	close(data_directory);
	free(data);
	fy_generic_builder_destroy(gb);
	close(opts.source_fd);
	close(opts.objects_fd);
	close(lower[0]);
	close(lower[1]);
	project_remove_tree(root);
	close(root);
	FYAI_TCHECK(!rmdir(path));
	return 0;
}

#else
int project_borrowed_git(void)
{
	return 0;
}
int project_capture_parallel(void)
{
	return 0;
}
#endif
