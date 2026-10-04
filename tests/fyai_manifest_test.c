/*
 * fyai_manifest_test.c - unit tests for the binary project manifest
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_manifest.h"
#include "fyai_scratch.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

#ifdef __APPLE__
#define TEST_MTIM(st) ((st).st_mtimespec)
#else
#define TEST_MTIM(st) ((st).st_mtim)
#endif

FYAI_TEST_ENTRY(manifest, snapshot_roundtrip, manifest_snapshot_roundtrip)
FYAI_TEST_ENTRY(manifest, lookup, manifest_lookup)
FYAI_TEST_ENTRY(manifest, file_roundtrip, manifest_file_roundtrip)
FYAI_TEST_ENTRY(manifest, damaged_files, manifest_damaged_files)
FYAI_TEST_ENTRY(manifest, builder, manifest_builder)
FYAI_TEST_ENTRY(manifest, equal, manifest_equal)
FYAI_TEST_ENTRY(manifest, diff, manifest_diff)
FYAI_TEST_ENTRY(manifest, delta, manifest_delta)

static struct fy_generic_builder *manifest_gb(void)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };

	return fy_generic_builder_create(&cfg);
}

/*
 * A small tree: a file, a borrowed file, a symlink and a directory d that
 * holds a file c. The file c has a timestamp that differs from its object.
 * Names include a byte that is not ASCII.
 */
struct tree {
	struct fy_generic_builder *gb;
	fy_generic snapshot;
	unsigned char root[FYAI_CAS_HASH_SIZE];
};

static int tree_entry(struct fyai_project_entry *entry, const char *name, enum fyai_project_kind kind,
		      const char *digest)
{
	entry->name = (const unsigned char *)name;
	entry->name_length = strlen(name);
	entry->kind = kind;
	return fyai_cas_digest_parse(entry->digest, digest);
}

static int tree_build(struct tree *tree)
{
	struct fyai_project_metadata meta = { .mode = 0644, .uid = 1000, .gid = 1000,
					      .mtime_sec = 100, .mtime_nsec = 7 };
	struct fyai_cas_blob plain = { .size = 3 }, borrowed = { .size = 9, .borrowed = true,
								 .source_device = 5,
								 .source_inode = 77,
								 .source_mode = 0444,
								 .source_uid = 1,
								 .source_gid = 2 };
	struct fyai_project_entry top[4], inner[1];
	char file[FYAI_CAS_DIGEST_SIZE], other[FYAI_CAS_DIGEST_SIZE], link[FYAI_CAS_DIGEST_SIZE];
	char inner_digest[FYAI_CAS_DIGEST_SIZE], dir[FYAI_CAS_DIGEST_SIZE],
		root[FYAI_CAS_DIGEST_SIZE];
	fy_generic pairs[10], attrs[2], file_object, other_object, link_object, inner_object,
		dir_object, root_object;
	int rc;

	rc = fyai_cas_digest_parse(plain.digest, "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd35"
						 "9c6cd5bd9d85");
	rc |= fyai_cas_digest_parse(borrowed.digest, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7"
						     "cc9a93cae41f3262");
	if (rc)
		return -1;
	file_object = fyai_project_file(tree->gb, &meta, &plain, file);
	meta.mode = 0755;
	other_object = fyai_project_file(tree->gb, &meta, &borrowed, other);
	meta.mode = 0777;
	link_object = fyai_project_symlink(tree->gb, &meta, (const unsigned char *)"a\xc3\xa9", 3, link);
	meta.mode = 0700;
	if (tree_entry(&inner[0], "c", FYAI_PROJECT_FILE, file))
		return -1;
	inner_object = fyai_project_directory(tree->gb, &meta, inner, 1, false, inner_digest);
	if (tree_entry(&top[0], "a", FYAI_PROJECT_FILE, file) ||
	    tree_entry(&top[1], "b", FYAI_PROJECT_FILE, other) ||
	    tree_entry(&top[2], "l", FYAI_PROJECT_SYMLINK, link) ||
	    tree_entry(&top[3], "d", FYAI_PROJECT_DIRECTORY, inner_digest))
		return -1;
	dir_object = fyai_project_directory(tree->gb, &meta, top, 4, true, root);
	(void)dir;
	root_object = dir_object;
	if (!fy_is_valid(file_object) || !fy_is_valid(other_object) || !fy_is_valid(link_object) ||
	    !fy_is_valid(inner_object) || !fy_is_valid(root_object))
		return -1;
	pairs[0] = fy_value(tree->gb, file);
	pairs[1] = file_object;
	pairs[2] = fy_value(tree->gb, other);
	pairs[3] = other_object;
	pairs[4] = fy_value(tree->gb, link);
	pairs[5] = link_object;
	pairs[6] = fy_value(tree->gb, inner_digest);
	pairs[7] = inner_object;
	pairs[8] = fy_value(tree->gb, root);
	pairs[9] = root_object;
	/* The attribute key is the hex of the path d/c. */
	attrs[0] = fy_value(tree->gb, "642f63");
	attrs[1] = fy_mapping(tree->gb, "mtime_sec", 555LL, "mtime_nsec", 9LL);
	tree->snapshot = fy_mapping(tree->gb, "version", 2LL, "algorithm", "blake3", "root",
				    fy_value(tree->gb, root), "objects",
				    fyai_project_table_create(tree->gb, 5, pairs), "attributes",
				    fyai_project_table_create(tree->gb, 1, attrs));
	return fyai_cas_digest_parse(tree->root, root);
}

int manifest_snapshot_roundtrip(void)
{
	struct tree tree = { 0 };
	struct fyai_manifest manifest = { 0 };
	fy_generic again;

	tree.gb = manifest_gb();
	FYAI_TCHECK(tree.gb);
	FYAI_TCHECK(!tree_build(&tree));
	FYAI_TCHECK(fy_is_mapping(tree.snapshot));
	FYAI_TCHECK(!fyai_manifest_from_snapshot(&manifest, tree.snapshot));
	FYAI_TCHECK(manifest.object_count == 5 && manifest.attribute_count == 1);
	FYAI_TCHECK(!memcmp(fyai_manifest_root(&manifest), tree.root, sizeof(tree.root)));
	again = fyai_manifest_to_snapshot(tree.gb, &manifest);
	FYAI_TCHECK(fy_is_mapping(again) && fy_equal(again, tree.snapshot));
	fyai_manifest_close(&manifest);
	/* A snapshot of another version is refused. */
	errno = 0;
	FYAI_TCHECK(fyai_manifest_from_snapshot(&manifest, fy_mapping(tree.gb, "version", 1LL)) < 0 &&
		    errno == EINVAL);
	fy_generic_builder_destroy(tree.gb);
	return 0;
}

int manifest_lookup(void)
{
	struct tree tree = { 0 };
	struct fyai_manifest manifest = { 0 };
	struct fyai_mobject object, found;
	struct fyai_mdir directory;
	struct fyai_project_entry entry;
	unsigned char digest[FYAI_CAS_HASH_SIZE], seen[FYAI_CAS_HASH_SIZE];
	const unsigned char *path;
	size_t i, length, names = 0;
	int64_t sec;
	uint32_t nsec;

	tree.gb = manifest_gb();
	FYAI_TCHECK(tree.gb && !tree_build(&tree));
	FYAI_TCHECK(!fyai_manifest_from_snapshot(&manifest, tree.snapshot));
	/* The root, then each name in turn. */
	FYAI_TCHECK(fyai_manifest_lookup(&manifest, (const unsigned char *)"", 0, &object, digest));
	FYAI_TCHECK(object.kind == FYAI_PROJECT_DIRECTORY && object.entry_count == 4 &&
		    !memcmp(digest, tree.root, sizeof(digest)));
	FYAI_TCHECK(!fyai_mdir_open(&object, &directory));
	while (fyai_mdir_next(&directory, &entry)) {
		/* Entries are in the byte order of their names. */
		if (names++)
			FYAI_TCHECK(memcmp(seen, entry.name, 1) < 0);
		seen[0] = entry.name[0];
	}
	FYAI_TCHECK(names == 4);
	FYAI_TCHECK(fyai_manifest_lookup(&manifest, (const unsigned char *)"a", 1, &object, NULL) &&
		    object.kind == FYAI_PROJECT_FILE && object.blob.size == 3 && !object.blob.borrowed &&
		    object.meta.mode == 0644 && object.meta.mtime_sec == 100);
	FYAI_TCHECK(fyai_manifest_lookup(&manifest, (const unsigned char *)"b", 1, &object, NULL) &&
		    object.blob.borrowed && object.blob.source_inode == 77 &&
		    object.blob.source_device == 5 && object.blob.source_gid == 2);
	FYAI_TCHECK(fyai_manifest_lookup(&manifest, (const unsigned char *)"l", 1, &object, NULL) &&
		    object.kind == FYAI_PROJECT_SYMLINK && object.target_length == 3 &&
		    !memcmp(object.target, "a\xc3\xa9", 3));
	FYAI_TCHECK(fyai_manifest_lookup(&manifest, (const unsigned char *)"d/c", 3, &object, digest));
	FYAI_TCHECK(object.kind == FYAI_PROJECT_FILE &&
		    fyai_manifest_find(&manifest, digest, &found) && found.blob.size == 3);
	FYAI_TCHECK(!fyai_manifest_lookup(&manifest, (const unsigned char *)"x", 1, &object, NULL));
	FYAI_TCHECK(!fyai_manifest_lookup(&manifest, (const unsigned char *)"d/x", 3, &object, NULL));
	FYAI_TCHECK(!fyai_manifest_lookup(&manifest, (const unsigned char *)"a/c", 3, &object, NULL));
	FYAI_TCHECK(!fyai_manifest_lookup(&manifest, (const unsigned char *)"d//c", 4, &object, NULL));
	/* The attribute of d/c, and no attribute for a path without one. */
	FYAI_TCHECK(fyai_manifest_attribute(&manifest, (const unsigned char *)"d/c", 3, &sec, &nsec) &&
		    sec == 555 && nsec == 9);
	FYAI_TCHECK(!fyai_manifest_attribute(&manifest, (const unsigned char *)"a", 1, &sec, &nsec));
	FYAI_TCHECK(fyai_manifest_attribute_at(&manifest, 0, &path, &length, &sec, &nsec) &&
		    length == 3 && !memcmp(path, "d/c", 3) && !fyai_manifest_attribute_at(&manifest, 1, &path, &length, &sec, &nsec));
	/* Every object is found by its own digest, and the index is in order. */
	for (i = 0; i < manifest.object_count; i++) {
		FYAI_TCHECK(fyai_manifest_object_at(&manifest, i, digest, &object));
		FYAI_TCHECK(fyai_manifest_find(&manifest, digest, &found) && found.kind == object.kind);
		if (i)
			FYAI_TCHECK(memcmp(seen, digest, sizeof(seen)) < 0);
		memcpy(seen, digest, sizeof(seen));
	}
	memset(digest, 0xee, sizeof(digest));
	FYAI_TCHECK(!fyai_manifest_find(&manifest, digest, &found));
	FYAI_TCHECK(!fyai_manifest_object_at(&manifest, manifest.object_count, digest, &found));
	fyai_manifest_close(&manifest);
	fy_generic_builder_destroy(tree.gb);
	return 0;
}

static int manifest_temp_dir(char *path, int *fd)
{
	if (!mkdtemp(path))
		return -1;
	*fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	return *fd < 0 ? -1 : 0;
}

int manifest_file_roundtrip(void)
{
	char path[] = "/tmp/fyai-manifest-test-XXXXXX", name[FYAI_MANIFEST_NAME_SIZE],
	     again[FYAI_MANIFEST_NAME_SIZE];
	struct tree tree = { 0 };
	struct fyai_manifest manifest = { 0 }, opened = { 0 };
	struct stat st, first;
	fy_generic snapshot;
	int dir;

	tree.gb = manifest_gb();
	FYAI_TCHECK(tree.gb && !tree_build(&tree));
	FYAI_TCHECK(!fyai_manifest_from_snapshot(&manifest, tree.snapshot));
	FYAI_TCHECK(!manifest_temp_dir(path, &dir));
	FYAI_TCHECK(!fyai_manifest_write(&manifest, dir, false, name));
	FYAI_TCHECK(strlen(name) == 64);
	/* A second write names the same file and leaves nothing else. */
	FYAI_TCHECK(!fstatat(dir, name, &first, AT_SYMLINK_NOFOLLOW));
	FYAI_TCHECK(!fyai_manifest_write(&manifest, dir, true, again) && !strcmp(name, again));
	FYAI_TCHECK(!fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) && !(st.st_mode & 0222) &&
		    (size_t)st.st_size == manifest.size);
	/* The file that exists is kept, not written again. */
	FYAI_TCHECK(st.st_ino == first.st_ino && TEST_MTIM(st).tv_sec == TEST_MTIM(first).tv_sec &&
		    TEST_MTIM(st).tv_nsec == TEST_MTIM(first).tv_nsec);
	FYAI_TCHECK(!fyai_manifest_open_at(&opened, dir, name) && opened.mapped);
	FYAI_TCHECK(opened.object_count == manifest.object_count &&
		    !memcmp(opened.root, manifest.root, sizeof(opened.root)));
	snapshot = fyai_manifest_to_snapshot(tree.gb, &opened);
	FYAI_TCHECK(fy_equal(snapshot, tree.snapshot));
	fyai_manifest_close(&opened);
	fyai_manifest_close(&manifest);
	errno = 0;
	FYAI_TCHECK(fyai_manifest_open_at(&opened, dir, "absent") < 0 && errno == ENOENT);
	FYAI_TCHECK(!unlinkat(dir, name, 0));
	close(dir);
	FYAI_TCHECK(!rmdir(path));
	fy_generic_builder_destroy(tree.gb);
	return 0;
}

/* Write a changed copy of the image and report whether it opens. */
static int manifest_open_changed(const struct fyai_manifest *manifest, int dir, size_t cut,
				 size_t offset, unsigned char value, bool change)
{
	struct fyai_manifest opened;
	unsigned char *copy;
	size_t size = cut ? cut : manifest->size;
	int fd, rc, saved;

	copy = malloc(manifest->size);
	if (!copy)
		return -2;
	memcpy(copy, manifest->data, manifest->size);
	if (change)
		copy[offset] = value;
	fd = openat(dir, "damaged", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, copy, size) != (ssize_t)size) {
		free(copy);
		return -2;
	}
	close(fd);
	free(copy);
	rc = fyai_manifest_open_at(&opened, dir, "damaged");
	saved = errno;
	if (!rc)
		fyai_manifest_close(&opened);
	unlinkat(dir, "damaged", 0);
	errno = saved;
	return rc;
}

/* Change a byte of the image and look up every object: no crash, no bad read. */
static bool manifest_find_after_change(const struct fyai_manifest *manifest, int dir, size_t offset,
				       unsigned char value)
{
	struct fyai_manifest changed;
	struct fyai_mobject object;
	unsigned char *copy, digest[FYAI_CAS_HASH_SIZE];
	size_t i;
	int fd;

	copy = malloc(manifest->size);
	if (!copy)
		return false;
	memcpy(copy, manifest->data, manifest->size);
	copy[offset] = value;
	fd = openat(dir, "damaged", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, copy, manifest->size) != (ssize_t)manifest->size) {
		free(copy);
		return false;
	}
	close(fd);
	free(copy);
	if (fyai_manifest_open_at(&changed, dir, "damaged")) {
		unlinkat(dir, "damaged", 0);
		return false;
	}
	for (i = 0; i < changed.object_count; i++)
		(void)fyai_manifest_object_at(&changed, i, digest, &object);
	(void)fyai_manifest_find(&changed, manifest->root, &object);
	fyai_manifest_close(&changed);
	unlinkat(dir, "damaged", 0);
	return true;
}

int manifest_damaged_files(void)
{
	char path[] = "/tmp/fyai-manifest-test-XXXXXX";
	struct tree tree = { 0 };
	struct fyai_manifest manifest = { 0 };
	size_t i, index;
	int dir;

	tree.gb = manifest_gb();
	FYAI_TCHECK(tree.gb && !tree_build(&tree));
	FYAI_TCHECK(!fyai_manifest_from_snapshot(&manifest, tree.snapshot));
	FYAI_TCHECK(!manifest_temp_dir(path, &dir));
	FYAI_TCHECK(manifest_open_changed(&manifest, dir, 0, 0, 0, false) == 0);
	/* Bad magic, a version that is not supported, and a size that does not match. */
	errno = 0;
	FYAI_TCHECK(manifest_open_changed(&manifest, dir, 0, 0, 0x7a, true) < 0 && errno == EBADMSG);
	errno = 0;
	FYAI_TCHECK(manifest_open_changed(&manifest, dir, 0, 4, 9, true) < 0 && errno == EBADMSG);
	/* Every prefix of the file that cuts it is refused. */
	for (i = 0; i < manifest.size; i += 7) {
		if (!i)
			continue;
		errno = 0;
		FYAI_TCHECK(manifest_open_changed(&manifest, dir, i, 0, 0, false) < 0);
	}
	/*
	 * The file is used as mapped: an index that is out of order, or an offset
	 * that leaves the record region, opens but is never followed out of bounds.
	 */
	index = manifest.index_offset + 40;
	FYAI_TCHECK(manifest_open_changed(&manifest, dir, 0, index, 0, true) == 0);
	FYAI_TCHECK(manifest_open_changed(&manifest, dir, 0, manifest.index_offset + 32 + 7, 0x7f, true) == 0);
	FYAI_TCHECK(manifest_find_after_change(&manifest, dir, manifest.index_offset + 32 + 7, 0x7f));
	FYAI_TCHECK(!rmdir(path));
	close(dir);
	fyai_manifest_close(&manifest);
	fy_generic_builder_destroy(tree.gb);
	return 0;
}

int manifest_builder(void)
{
	struct fyai_scratch scratch = { 0 };
	struct fyai_manifest_builder builder;
	struct fyai_manifest manifest = { 0 };
	struct fyai_mobject object = { .kind = FYAI_PROJECT_FILE,
				       .meta = { .mode = 0644, .mtime_sec = 1 },
				       .blob = { .size = 4 } }, found;
	struct fyai_project_entry entry;
	unsigned char root[FYAI_CAS_HASH_SIZE], file[FYAI_CAS_HASH_SIZE], missing[FYAI_CAS_HASH_SIZE];
	int64_t sec;
	uint32_t nsec;

	memset(root, 1, sizeof(root));
	memset(file, 2, sizeof(file));
	memset(missing, 3, sizeof(missing));
	entry.name = (const unsigned char *)"f";
	entry.name_length = 1;
	entry.kind = FYAI_PROJECT_FILE;
	memcpy(entry.digest, file, sizeof(file));
	FYAI_TCHECK(!fyai_scratch_open(&scratch));
	FYAI_TCHECK(!fyai_manifest_builder_init(&builder, &scratch));
	/* A manifest without its root is refused. */
	errno = 0;
	FYAI_TCHECK(fyai_manifest_builder_finish(&builder, root, &manifest) < 0 && errno == EINVAL);
	FYAI_TCHECK(!fyai_manifest_builder_add(&builder, file, &object, NULL));
	FYAI_TCHECK(fyai_manifest_builder_has(&builder, file) && !fyai_manifest_builder_has(&builder, root));
	/* The first record of a digest stays. */
	object.meta.mtime_sec = 99;
	FYAI_TCHECK(!fyai_manifest_builder_add(&builder, file, &object, NULL));
	object = (struct fyai_mobject){ .kind = FYAI_PROJECT_DIRECTORY, .entry_count = 1,
					.meta = { .mode = 0755 } };
	FYAI_TCHECK(!fyai_manifest_builder_add(&builder, root, &object, &entry));
	/* A directory with entries needs them; an object of no kind is refused. */
	errno = 0;
	FYAI_TCHECK(fyai_manifest_builder_add(&builder, missing, &object, NULL) < 0 && errno == EINVAL);
	object.kind = 0;
	errno = 0;
	FYAI_TCHECK(fyai_manifest_builder_add(&builder, missing, &object, NULL) < 0 && errno == EINVAL);
	/* Attributes: one for a path in the tree, one that is not, and a repeat. */
	FYAI_TCHECK(!fyai_manifest_builder_attribute(&builder, (const unsigned char *)"f", 1, 5, 6));
	FYAI_TCHECK(!fyai_manifest_builder_attribute(&builder, (const unsigned char *)"gone", 4, 7, 8));
	FYAI_TCHECK(!fyai_manifest_builder_attribute(&builder, (const unsigned char *)"f", 1, 5, 6));
	FYAI_TCHECK(!fyai_manifest_builder_finish(&builder, root, &manifest));
	FYAI_TCHECK(manifest.object_count == 2 && manifest.attribute_count == 1);
	FYAI_TCHECK(fyai_manifest_find(&manifest, file, &found) && found.meta.mtime_sec == 1);
	FYAI_TCHECK(fyai_manifest_attribute(&manifest, (const unsigned char *)"f", 1, &sec, &nsec) &&
		    sec == 5 && nsec == 6);
	FYAI_TCHECK(!fyai_manifest_attribute(&manifest, (const unsigned char *)"gone", 4, &sec, &nsec));
	fyai_manifest_close(&manifest);
	fyai_scratch_close(&scratch);
	return 0;
}

/*
 * A root directory with a file a, a file b and, when extra is set, a file e and
 * the entry .git. Digests are arbitrary: the library only stores them.
 */
struct spec {
	unsigned char a, b;		/* the content byte of the digests of a and b */
	bool with_b, with_extra;
	int64_t a_mtime;		/* the timestamp that the object of a stores */
	bool a_attribute;		/* an attribute for a, with a_attribute_sec */
	int64_t a_attribute_sec;
};

static int spec_build(const struct spec *spec, struct fyai_scratch *scratch,
		      struct fyai_manifest *manifest)
{
	struct fyai_manifest_builder builder;
	struct fyai_project_entry entries[4];
	struct fyai_mobject object = { .kind = FYAI_PROJECT_FILE, .meta = { .mode = 0644 } };
	unsigned char root[FYAI_CAS_HASH_SIZE], file[FYAI_CAS_HASH_SIZE], extra[FYAI_CAS_HASH_SIZE];
	size_t count = 0;

	memset(root, 0x11, sizeof(root));
	memset(extra, 0x33, sizeof(extra));
	if (fyai_manifest_builder_init(&builder, scratch))
		return -1;
	/* Entries in name order: .git, a, b, e. */
	if (spec->with_extra) {
		entries[count++] = (struct fyai_project_entry){ .name = (const unsigned char *)".git",
								.name_length = 4,
								.kind = FYAI_PROJECT_FILE };
		memcpy(entries[count - 1].digest, extra, sizeof(extra));
	}
	memset(file, spec->a, sizeof(file));
	object.blob.size = spec->a;
	memcpy(object.blob.digest, file, sizeof(file));
	object.meta.mtime_sec = spec->a_mtime;
	if (fyai_manifest_builder_add(&builder, file, &object, NULL))
		return -1;
	entries[count++] = (struct fyai_project_entry){ .name = (const unsigned char *)"a",
							.name_length = 1, .kind = FYAI_PROJECT_FILE };
	memcpy(entries[count - 1].digest, file, sizeof(file));
	if (spec->with_b) {
		memset(file, spec->b, sizeof(file));
		object.blob.size = spec->b;
		memcpy(object.blob.digest, file, sizeof(file));
		object.meta.mtime_sec = 7;
		if (fyai_manifest_builder_add(&builder, file, &object, NULL))
			return -1;
		entries[count++] = (struct fyai_project_entry){ .name = (const unsigned char *)"b",
								.name_length = 1,
								.kind = FYAI_PROJECT_FILE };
		memcpy(entries[count - 1].digest, file, sizeof(file));
	}
	if (spec->with_extra) {
		object.blob.size = 99;
		memcpy(object.blob.digest, extra, sizeof(extra));
		if (fyai_manifest_builder_add(&builder, extra, &object, NULL))
			return -1;
		entries[count++] = (struct fyai_project_entry){ .name = (const unsigned char *)"e",
								.name_length = 1,
								.kind = FYAI_PROJECT_FILE };
		memcpy(entries[count - 1].digest, extra, sizeof(extra));
	}
	object = (struct fyai_mobject){ .kind = FYAI_PROJECT_DIRECTORY,
					.meta = { .mode = 0755, .mtime_sec = 5 },
					.entry_count = (uint32_t)count };
	if (fyai_manifest_builder_add(&builder, root, &object, entries))
		return -1;
	if (spec->a_attribute &&
	    fyai_manifest_builder_attribute(&builder, (const unsigned char *)"a", 1,
					    spec->a_attribute_sec, 0))
		return -1;
	return fyai_manifest_builder_finish(&builder, root, manifest);
}

int manifest_equal(void)
{
	struct fyai_scratch scratch = { 0 };
	struct fyai_manifest one = { 0 }, same = { 0 }, other = { 0 }, timed = { 0 }, stored = { 0 };
	struct spec base = { .a = 1, .b = 2, .with_b = true, .a_mtime = 10 };
	struct spec changed = base, retimed = base, moved = base;

	FYAI_TCHECK(!fyai_scratch_open(&scratch));
	FYAI_TCHECK(!spec_build(&base, &scratch, &one) && !spec_build(&base, &scratch, &same));
	FYAI_TCHECK(fyai_manifest_equal(&one, &one) && fyai_manifest_equal(&one, &same));
	/* Another digest at a path gives another root. */
	changed.a = 3;
	FYAI_TCHECK(!spec_build(&changed, &scratch, &other));
	FYAI_TCHECK(!fyai_manifest_equal(&one, &other));
	/* The same tree with another stored time differs, and an attribute that
	 * restores the effective time makes it equal again. */
	retimed.a_mtime = 11;
	FYAI_TCHECK(!spec_build(&retimed, &scratch, &timed));
	FYAI_TCHECK(!fyai_manifest_equal(&one, &timed));
	moved.a_mtime = 11;
	moved.a_attribute = true;
	moved.a_attribute_sec = 10;
	FYAI_TCHECK(!spec_build(&moved, &scratch, &stored));
	FYAI_TCHECK(fyai_manifest_equal(&one, &stored) && fyai_manifest_equal(&stored, &one));
	fyai_manifest_close(&one);
	fyai_manifest_close(&same);
	fyai_manifest_close(&other);
	fyai_manifest_close(&timed);
	fyai_manifest_close(&stored);
	fyai_scratch_close(&scratch);
	return 0;
}

int manifest_diff(void)
{
	struct fyai_scratch scratch = { 0 };
	struct fyai_manifest left = { 0 }, right = { 0 }, retimed = { 0 };
	struct fy_generic_builder *gb;
	struct spec before = { .a = 1, .b = 2, .with_b = true, .a_mtime = 10 };
	struct spec after = { .a = 3, .b = 2, .with_b = false, .with_extra = true, .a_mtime = 10 };
	struct spec timed = { .a = 1, .b = 2, .with_b = true, .a_mtime = 10, .a_attribute = true,
			      .a_attribute_sec = 50 };
	fy_generic rows, row;

	gb = manifest_gb();
	FYAI_TCHECK(gb && !fyai_scratch_open(&scratch));
	FYAI_TCHECK(!spec_build(&before, &scratch, &left) && !spec_build(&after, &scratch, &right));
	/* a changed, b is gone, e is new, and .git does not show. */
	rows = fyai_manifest_diff(gb, &left, &right);
	FYAI_TCHECK(fy_is_sequence(rows) && fy_len(rows) == 3);
	row = fy_get_at(rows, 0);
	FYAI_TCHECK(!strcmp(fy_get(row, "path", ""), "a") && !strcmp(fy_get(row, "status", ""), "modified") &&
		    !strcmp(fy_get(row, "path_hex", ""), "61") &&
		    fy_get(fy_get(row, "before", fy_invalid), "size", 0LL) == 1 &&
		    fy_get(fy_get(row, "after", fy_invalid), "size", 0LL) == 3);
	row = fy_get_at(rows, 1);
	FYAI_TCHECK(!strcmp(fy_get(row, "path", ""), "b") && !strcmp(fy_get(row, "status", ""), "deleted") &&
		    fy_is_null(fy_get(row, "after", fy_invalid)));
	row = fy_get_at(rows, 2);
	FYAI_TCHECK(!strcmp(fy_get(row, "path", ""), "e") && !strcmp(fy_get(row, "status", ""), "added") &&
		    fy_is_null(fy_get(row, "before", fy_invalid)));
	/* Nothing changed: no rows. A time that only an attribute changes is a row. */
	rows = fyai_manifest_diff(gb, &left, &left);
	FYAI_TCHECK(fy_is_sequence(rows) && !fy_len(rows));
	FYAI_TCHECK(!spec_build(&timed, &scratch, &retimed));
	rows = fyai_manifest_diff(gb, &left, &retimed);
	FYAI_TCHECK(fy_len(rows) == 1 && !strcmp(fy_get(fy_get_at(rows, 0), "path", ""), "a") &&
		    fy_get(fy_get(fy_get_at(rows, 0), "after", fy_invalid), "mtime_sec", 0LL) == 50);
	fyai_manifest_close(&left);
	fyai_manifest_close(&right);
	fyai_manifest_close(&retimed);
	fyai_scratch_close(&scratch);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* Publish a manifest into dir and open it again. */
static int delta_publish(struct fyai_manifest *manifest, int dir, char name[FYAI_MANIFEST_NAME_SIZE])
{
	return fyai_manifest_write(manifest, dir, false, name);
}

int manifest_delta(void)
{
	char path[] = "/tmp/fyai-manifest-delta-XXXXXX", base_name[FYAI_MANIFEST_NAME_SIZE],
	     delta_name[FYAI_MANIFEST_NAME_SIZE], full_name[FYAI_MANIFEST_NAME_SIZE],
	     same_name[FYAI_MANIFEST_NAME_SIZE];
	struct fyai_scratch scratch = { 0 };
	struct fyai_manifest base = { 0 }, full = { 0 }, delta = { 0 }, opened = { 0 }, again = { 0 };
	struct fyai_mobject object, found;
	struct fy_generic_builder *gb;
	/* The base has an attribute for a; the tree after the change has none. */
	struct spec before = { .a = 1, .b = 2, .with_b = true, .a_mtime = 10, .a_attribute = true,
			       .a_attribute_sec = 50 };
	struct spec after = { .a = 3, .b = 2, .with_b = false, .with_extra = true, .a_mtime = 10 };
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	struct stat full_stat, delta_stat;
	int64_t sec;
	uint32_t nsec;
	size_t i;
	int dir;

	gb = manifest_gb();
	FYAI_TCHECK(gb && !fyai_scratch_open(&scratch) && !manifest_temp_dir(path, &dir));
	FYAI_TCHECK(!spec_build(&before, &scratch, &base) && !spec_build(&after, &scratch, &full));
	FYAI_TCHECK(!delta_publish(&base, dir, base_name));
	FYAI_TCHECK(!fyai_manifest_make_delta(&full, &base, base_name, &delta));
	FYAI_TCHECK(delta.delta && delta.object_total == full.object_count &&
		    delta.object_count < full.object_count + 1);
	FYAI_TCHECK(!delta_publish(&delta, dir, delta_name));
	FYAI_TCHECK(!delta_publish(&full, dir, full_name));
	FYAI_TCHECK(!fstatat(dir, full_name, &full_stat, 0) && !fstatat(dir, delta_name, &delta_stat, 0));
	FYAI_TCHECK(!fyai_manifest_open_at(&opened, dir, delta_name));
	FYAI_TCHECK(opened.delta && opened.base && !opened.base->delta &&
		    !memcmp(opened.root, full.root, sizeof(full.root)));
	/* Every object of the tree is found through the delta. */
	for (i = 0; i < full.object_count; i++) {
		FYAI_TCHECK(fyai_manifest_object_at(&full, i, digest, &object));
		FYAI_TCHECK(fyai_manifest_find(&opened, digest, &found) && found.kind == object.kind &&
			    found.blob.size == object.blob.size);
	}
	/* An object that only the base has is gone; one that both have is kept. */
	for (i = 0; i < base.object_count; i++) {
		FYAI_TCHECK(fyai_manifest_object_at(&base, i, digest, &object));
		FYAI_TCHECK(fyai_manifest_find(&opened, digest, &found) ==
			    fyai_manifest_find(&full, digest, &found));
	}
	FYAI_TCHECK(fyai_manifest_lookup(&opened, (const unsigned char *)"e", 1, &object, NULL) &&
		    !fyai_manifest_lookup(&opened, (const unsigned char *)"b", 1, &object, NULL) &&
		    fyai_manifest_lookup(&opened, (const unsigned char *)"a", 1, &object, NULL) &&
		    object.blob.size == 3);
	/* The attribute of a was in the base only: the delta removed it. */
	FYAI_TCHECK(fyai_manifest_attribute(&base, (const unsigned char *)"a", 1, &sec, &nsec) &&
		    !fyai_manifest_attribute(&opened, (const unsigned char *)"a", 1, &sec, &nsec));
	/* The delta is the same tree as the full manifest, in the comparison and the diff. */
	FYAI_TCHECK(!fyai_manifest_open_at(&again, dir, full_name));
	FYAI_TCHECK(fyai_manifest_equal(&opened, &again) && fyai_manifest_equal(&again, &opened));
	FYAI_TCHECK(fy_equal(fyai_manifest_diff(gb, &base, &opened), fyai_manifest_diff(gb, &base, &again)));
	FYAI_TCHECK(fy_len(fyai_manifest_diff(gb, &opened, &again)) == 0);
	/* The snapshot of a delta is the snapshot of the whole tree. */
	FYAI_TCHECK(fy_equal(fyai_manifest_to_snapshot(gb, &opened), fyai_manifest_to_snapshot(gb, &again)));
	FYAI_TCHECK(fy_is_mapping(fyai_manifest_to_snapshot(gb, &opened)));
	/* ... and a full manifest made from it is the same tree again. */
	{
		struct fyai_manifest rebuilt = { 0 };

		FYAI_TCHECK(!fyai_manifest_from_snapshot(&rebuilt, fyai_manifest_to_snapshot(gb, &opened)));
		FYAI_TCHECK(!rebuilt.delta && fyai_manifest_equal(&rebuilt, &opened) &&
			    rebuilt.object_count == full.object_count);
		fyai_manifest_close(&rebuilt);
	}
	/* The records that the delta shares with its base are not copied. */
	FYAI_TCHECK(delta.object_count < base.object_count + full.object_count);
	(void)full_stat;
	(void)delta_stat;
	fyai_manifest_close(&again);
	fyai_manifest_close(&opened);
	fyai_manifest_close(&delta);
	/* A delta of a tree over itself holds the root only, and names the same tree. */
	FYAI_TCHECK(!fyai_manifest_make_delta(&base, &base, base_name, &delta));
	FYAI_TCHECK(delta.delta && delta.object_count == 1 && delta.attribute_count == 0);
	FYAI_TCHECK(!delta_publish(&delta, dir, same_name));
	FYAI_TCHECK(!fyai_manifest_open_at(&opened, dir, same_name));
	FYAI_TCHECK(fyai_manifest_equal(&opened, &base));
	FYAI_TCHECK(fy_equal(fyai_manifest_to_snapshot(gb, &opened), fyai_manifest_to_snapshot(gb, &base)));
	FYAI_TCHECK(fyai_manifest_attribute(&opened, (const unsigned char *)"a", 1, &sec, &nsec) &&
		    sec == 50);
	fyai_manifest_close(&opened);
	/* A delta over a delta is refused, and so is one whose base is missing. */
	errno = 0;
	FYAI_TCHECK(fyai_manifest_make_delta(&delta, &base, base_name, &opened) < 0 && errno == EINVAL);
	FYAI_TCHECK(!unlinkat(dir, base_name, 0));
	errno = 0;
	FYAI_TCHECK(fyai_manifest_open_at(&opened, dir, same_name) < 0 && errno == ENOENT);
	FYAI_TCHECK(!unlinkat(dir, same_name, 0) && !unlinkat(dir, delta_name, 0) &&
		    !unlinkat(dir, full_name, 0));
	fyai_manifest_close(&delta);
	fyai_manifest_close(&full);
	fyai_manifest_close(&base);
	close(dir);
	FYAI_TCHECK(!rmdir(path));
	fyai_scratch_close(&scratch);
	fy_generic_builder_destroy(gb);
	return 0;
}
