/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fyai_project.h"
#include "fyai_project_capture.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(project, directory_order, project_directory_order)
FYAI_TEST_ENTRY(project, capture_parallel, project_capture_parallel)
FYAI_TEST_ENTRY(project, directory_ancestors, project_directory_ancestors)
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
	FYAI_TCHECK(!strcmp(root1,
		"863a5e73f50868cecdcde4ec3d39def7704caae5d4f6d59e86d80ad0fa00c9ca"));
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

int project_capture_parallel(void)
{
	struct fy_generic_builder *gb;
	struct fyai_project_capture_opts opts = { .verify = true, .workers = 1 };
	fy_generic first, second;
	char path[] = "/tmp/fyai-project-parallel-XXXXXX", name[32], error[PATH_MAX];
	unsigned char *data, *mapped;
	size_t i, j, size = 2U * 1024U * 1024U + 19;
	struct stat st;
	int root, fd, rc, lower[2];

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
		/* Repeated bytes exercise concurrent publication of one CAS identity. */
		FYAI_TCHECK(write(fd, data, i < 8 ? size : 4096) == (ssize_t)(i < 8 ? size : 4096));
		close(fd);
	}
	FYAI_TCHECK(!mkdirat(opts.source_fd, "empty", 0700));
	FYAI_TCHECK(!symlinkat("file-00", opts.source_fd, "link"));
	FYAI_TCHECK(!mkdirat(opts.source_fd, ".fyai", 0700));
	gb = project_builder();
	FYAI_TCHECK(gb);
	opts.baseline_fd = lower[0];
	first = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(first));
	opts.baseline_fd = lower[1];
	opts.workers = 4;
	second = fyai_project_capture(gb, &opts, error, sizeof(error));
	FYAI_TCHECK(fy_is_mapping(second) && fy_equal(first, second));
	for (i = 0; i < 2; i++) {
		fd = openat(lower[i], "file-00", O_RDONLY | O_NOFOLLOW);
		FYAI_TCHECK(fd >= 0 && !fstat(fd, &st) && st.st_size == (off_t)size && st.st_nlink == 1);
		mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		FYAI_TCHECK(mapped != MAP_FAILED && !memcmp(mapped, data, size));
		munmap(mapped, size);
		close(fd);
		FYAI_TCHECK(!fstatat(lower[i], "empty", &st, AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode));
		FYAI_TCHECK(!fstatat(lower[i], "link", &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode));
	}
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
