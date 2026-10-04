/*
 * fyai_project.c - canonical Merkle manifests of a captured project
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libfyaml/libfyaml-blake3.h>

#include "fyai_project.h"

static const char *project_kind_name(enum fyai_project_kind kind)
{
	switch (kind) {
	case FYAI_PROJECT_FILE:
		return "file";
	case FYAI_PROJECT_DIRECTORY:
		return "directory";
	case FYAI_PROJECT_SYMLINK:
		return "symlink";
	default:
		return NULL;
	}
}

static int project_entry_compare(const void *a, const void *b)
{
	const struct fyai_project_entry *left = a, *right = b;
	size_t length;
	int rc;

	length = left->name_length < right->name_length ? left->name_length : right->name_length;
	rc = memcmp(left->name, right->name, length);
	if (rc)
		return rc;
	return (left->name_length > right->name_length) - (left->name_length < right->name_length);
}

static bool project_entry_valid(const struct fyai_project_entry *entry, bool root)
{
	size_t i;

	if (!entry->name || !entry->name_length || !project_kind_name(entry->kind))
		return false;
	if (entry->name_length > (SIZE_MAX - 1) / 2)
		return false;
	if ((entry->name_length == 1 && entry->name[0] == '.') ||
	    (entry->name_length == 2 && !memcmp(entry->name, "..", 2)) ||
	    (root && entry->name_length == 5 && !memcmp(entry->name, ".fyai", 5)))
		return false;
	for (i = 0; i < entry->name_length; i++)
		if (!entry->name[i] || entry->name[i] == '/')
			return false;
	return true;
}

/* Canonical integers are eight-byte little-endian words, independent of host
 * representation. */
static void project_hash_number(struct fy_blake3_hasher *hasher, uint64_t value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	value = __builtin_bswap64(value);
#endif
	fy_blake3_hasher_update(hasher, &value, sizeof(value));
}

/* A hasher that the caller lends is reset; otherwise this call makes one. */
static struct fy_blake3_hasher *project_hasher(struct fy_blake3_hasher *lent, bool *made)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher = lent;

	*made = false;
	if (!hasher) {
		hasher = fy_blake3_hasher_create(&cfg);
		if (!hasher) {
			errno = ENOMEM;
			return NULL;
		}
		*made = true;
	} else {
		fy_blake3_hasher_reset(hasher);
	}
	return hasher;
}

static int project_finish(struct fy_blake3_hasher *hasher, bool made,
			  unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	const uint8_t *hash = fy_blake3_hasher_finalize(hasher);
	int rc = 0;

	if (!hash) {
		errno = EIO;
		rc = -1;
	} else {
		memcpy(digest, hash, FYAI_CAS_HASH_SIZE);
	}
	if (made)
		fy_blake3_hasher_destroy(hasher);
	return rc;
}

int fyai_project_entries_prepare(struct fyai_project_entry *entries, size_t count,
				 bool project_root)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (!project_entry_valid(&entries[i], project_root)) {
			errno = EINVAL;
			return -1;
		}
	}
	qsort(entries, count, sizeof(*entries), project_entry_compare);
	/* The order is total, so a repeated name is adjacent. */
	for (i = 1; i < count; i++) {
		if (!project_entry_compare(&entries[i - 1], &entries[i])) {
			errno = EEXIST;
			return -1;
		}
	}
	return 0;
}

int fyai_project_directory_digest(struct fy_blake3_hasher *lent,
				  const struct fyai_project_metadata *metadata,
				  const struct fyai_project_entry *entries, size_t count,
				  unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	static const char domain[] = "fyai/project/directory/blake3/v2";
	struct fy_blake3_hasher *hasher;
	bool made;
	size_t i;

	if (!metadata || (count && !entries) || metadata->mode > 07777 ||
	    metadata->mtime_nsec >= 1000000000) {
		errno = EINVAL;
		return -1;
	}
	hasher = project_hasher(lent, &made);
	if (!hasher)
		return -1;
	fy_blake3_hasher_update(hasher, domain, sizeof(domain));
	project_hash_number(hasher, metadata->mode);
	project_hash_number(hasher, metadata->uid);
	project_hash_number(hasher, metadata->gid);
	project_hash_number(hasher, count);
	for (i = 0; i < count; i++) {
		project_hash_number(hasher, entries[i].name_length);
		fy_blake3_hasher_update(hasher, entries[i].name, entries[i].name_length);
		project_hash_number(hasher, entries[i].kind);
		fy_blake3_hasher_update(hasher, entries[i].digest, sizeof(entries[i].digest));
	}
	return project_finish(hasher, made, digest);
}

fy_generic fyai_project_directory(struct fy_generic_builder *gb,
				  const struct fyai_project_metadata *metadata,
				  const struct fyai_project_entry *entries, size_t count,
				  bool project_root, char digest[FYAI_CAS_DIGEST_SIZE])
{
	struct fyai_project_entry *sorted = NULL;
	fy_generic *items = NULL, children, result = fy_invalid;
	unsigned char raw[FYAI_CAS_HASH_SIZE];
	char *name = NULL, child_digest[FYAI_CAS_DIGEST_SIZE];
	size_t i, longest = 0;
	int saved;

	if (!gb || !metadata || !digest || (count && !entries) || metadata->mode > 07777 ||
	    metadata->mtime_nsec >= 1000000000 || count > SIZE_MAX / sizeof(*sorted) ||
	    count > SIZE_MAX / sizeof(*items)) {
		errno = EINVAL;
		return fy_invalid;
	}
	if (count) {
		sorted = malloc(count * sizeof(*sorted));
		items = malloc(count * sizeof(*items));
		if (!sorted || !items)
			goto out;
		memcpy(sorted, entries, count * sizeof(*sorted));
		if (fyai_project_entries_prepare(sorted, count, project_root))
			goto out;
		for (i = 0; i < count; i++)
			if (sorted[i].name_length > longest)
				longest = sorted[i].name_length;
		name = malloc(longest * 2 + 1);
		if (!name)
			goto out;
	}
	if (fyai_project_directory_digest(NULL, metadata, sorted, count, raw))
		goto out;
	for (i = 0; i < count; i++) {
		fyai_cas_hex(name, sorted[i].name, sorted[i].name_length);
		fyai_cas_hex(child_digest, sorted[i].digest, sizeof(sorted[i].digest));
		items[i] = fy_mapping(gb, "name_hex", fy_value(gb, name), "kind",
				      fy_value(gb, project_kind_name(sorted[i].kind)), "digest",
				      fy_value(gb, child_digest));
		if (!fy_is_valid(items[i])) {
			errno = ENOMEM;
			goto out;
		}
	}
	children = fy_gb_sequence_create(gb, count, items);
	if (!fy_is_valid(children)) {
		errno = ENOMEM;
		goto out;
	}
	result = fy_mapping(gb, "version", 2LL, "kind", "directory", "algorithm", "blake3", "mode",
			    (long long)metadata->mode, "uid", (long long)metadata->uid, "gid",
			    (long long)metadata->gid, "mtime_sec", (long long)metadata->mtime_sec,
			    "mtime_nsec", (long long)metadata->mtime_nsec, "entries", children);
	if (!fy_is_valid(result)) {
		errno = ENOMEM;
		goto out;
	}
	fyai_cas_hex(digest, raw, sizeof(raw));
out:
	saved = errno;
	free(name);
	free(items);
	free(sorted);
	errno = saved;
	return result;
}

/* The identity of a file or a symlink: the kind, the metadata, and the payload. */
static int project_leaf_digest(struct fy_blake3_hasher *lent,
			       const struct fyai_project_metadata *metadata, const char *kind,
			       const void *payload, size_t length, uint64_t size,
			       unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	static const char domain[] = "fyai/project/leaf/blake3/v2";
	struct fy_blake3_hasher *hasher;
	bool made;

	if (!metadata || (!payload && length) || metadata->mode > 07777 ||
	    metadata->mtime_nsec >= 1000000000) {
		errno = EINVAL;
		return -1;
	}
	hasher = project_hasher(lent, &made);
	if (!hasher)
		return -1;
	fy_blake3_hasher_update(hasher, domain, sizeof(domain));
	fy_blake3_hasher_update(hasher, kind, strlen(kind) + 1);
	project_hash_number(hasher, metadata->mode);
	project_hash_number(hasher, metadata->uid);
	project_hash_number(hasher, metadata->gid);
	project_hash_number(hasher, size);
	fy_blake3_hasher_update(hasher, payload, length);
	return project_finish(hasher, made, digest);
}

int fyai_project_file_digest(struct fy_blake3_hasher *hasher,
			     const struct fyai_project_metadata *metadata,
			     const struct fyai_cas_blob *blob,
			     unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	if (!blob || blob->size > INT64_MAX) {
		errno = EINVAL;
		return -1;
	}
	return project_leaf_digest(hasher, metadata, "file", blob->digest, sizeof(blob->digest),
				   blob->size, digest);
}

int fyai_project_symlink_digest(struct fy_blake3_hasher *hasher,
				const struct fyai_project_metadata *metadata,
				const unsigned char *target, size_t length,
				unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	if (!target || !length || length > (SIZE_MAX - 1) / 2 || memchr(target, 0, length)) {
		errno = EINVAL;
		return -1;
	}
	return project_leaf_digest(hasher, metadata, "symlink", target, length, length, digest);
}

static fy_generic project_leaf(struct fy_generic_builder *gb,
			       const struct fyai_project_metadata *metadata, const char *kind,
			       const void *payload, size_t length, uint64_t size,
			       char digest[FYAI_CAS_DIGEST_SIZE])
{
	unsigned char raw[FYAI_CAS_HASH_SIZE];

	if (!gb || !digest || project_leaf_digest(NULL, metadata, kind, payload, length, size, raw))
		return fy_invalid;
	fyai_cas_hex(digest, raw, sizeof(raw));
	return fy_mapping(gb, "version", 2LL, "algorithm", "blake3", "kind", kind, "mode",
			  (long long)metadata->mode, "uid", (long long)metadata->uid, "gid",
			  (long long)metadata->gid, "mtime_sec", (long long)metadata->mtime_sec,
			  "mtime_nsec", (long long)metadata->mtime_nsec);
}

fy_generic fyai_project_file(struct fy_generic_builder *gb,
			     const struct fyai_project_metadata *metadata,
			     const struct fyai_cas_blob *blob, char digest[FYAI_CAS_DIGEST_SIZE])
{
	char hex[FYAI_CAS_DIGEST_SIZE];
	fy_generic result;

	if (!blob || blob->size > INT64_MAX) {
		errno = EINVAL;
		return fy_invalid;
	}
	result = project_leaf(gb, metadata, "file", blob->digest, sizeof(blob->digest),
			      blob->size, digest);
	if (!fy_is_valid(result))
		return fy_invalid;
	fyai_cas_blob_hex(hex, blob);
	result = fy_assoc(gb, result, "blob",
			  fy_mapping(gb, "algorithm", "blake3", "digest",
				     fy_value(gb, hex), "size", (long long)blob->size,
				     "storage", blob->borrowed ? "borrowed" : "owned"));
	if (blob->borrowed)
		result = fy_assoc(gb, result, "borrowed",
				  fy_mapping(gb, "device", (long long)blob->source_device, "inode",
					     (long long)blob->source_inode, "mode",
					     (long long)blob->source_mode, "uid",
					     (long long)blob->source_uid, "gid",
					     (long long)blob->source_gid));
	return result;
}

fy_generic fyai_project_symlink(struct fy_generic_builder *gb,
				const struct fyai_project_metadata *metadata,
				const unsigned char *target, size_t length,
				char digest[FYAI_CAS_DIGEST_SIZE])
{
	fy_generic result;
	char *encoded;

	if (!target || !length || length > (SIZE_MAX - 1) / 2 || memchr(target, 0, length)) {
		errno = EINVAL;
		return fy_invalid;
	}
	encoded = malloc(length * 2 + 1);
	if (!encoded)
		return fy_invalid;
	fyai_cas_hex(encoded, target, length);
	result = project_leaf(gb, metadata, "symlink", target, length, length, digest);
	if (fy_is_valid(result))
		result = fy_assoc(gb, result, "target_hex", fy_value(gb, encoded));
	free(encoded);
	return result;
}

/* Key bytes of a pair item. A table keeps its keys as plain strings. */
static const char *project_key(const fy_generic *item, size_t *length)
{
	if (fy_generic_is_string(*item) && !fy_generic_is_indirect(*item))
		return fy_genericp_get_string_size_no_check(item, length);
	*length = 0;
	return NULL;
}

static int project_key_compare(const char *a, size_t a_length, const char *b, size_t b_length)
{
	int rc = memcmp(a, b, a_length < b_length ? a_length : b_length);

	if (rc)
		return rc;
	return (a_length > b_length) - (a_length < b_length);
}

const fy_generic *fyai_project_find(fy_generic table, const char *key)
{
	const fy_generic *items;
	const char *name;
	size_t count, low = 0, high, middle, length = strlen(key), have;
	int rc;

	items = fy_generic_mapping_get_items(table, &count);
	high = count / 2;
	while (low < high) {
		middle = low + (high - low) / 2;
		name = project_key(&items[middle * 2], &have);
		if (!name)
			return NULL;
		rc = project_key_compare(key, length, name, have);
		if (!rc)
			return &items[middle * 2 + 1];
		if (rc < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return NULL;
}

fy_generic fyai_project_get(fy_generic table, const char *key)
{
	const fy_generic *value = fyai_project_find(table, key);

	return value ? *value : fy_invalid;
}

static int project_pair_compare(const void *a, const void *b)
{
	const fy_generic *left = a, *right = b;
	const char *left_key, *right_key;
	size_t left_length, right_length;

	left_key = project_key(left, &left_length);
	right_key = project_key(right, &right_length);
	return project_key_compare(left_key ? left_key : "", left_length,
				   right_key ? right_key : "", right_length);
}

fy_generic fyai_project_table_create(struct fy_generic_builder *gb, size_t count,
				     fy_generic *pairs)
{
	qsort(pairs, count, 2 * sizeof(*pairs), project_pair_compare);
	return fy_gb_mapping_create(gb, count, pairs);
}

fy_generic fyai_project_lookup(fy_generic snapshot, const char *path_hex)
{
	fy_generic objects, object, entries, entry;
	const char *part, *end, *name, *digest;
	size_t length, low, high, middle;
	unsigned int depth = 0;
	int rc;

	objects = fy_get(snapshot, "objects", fy_invalid);
	digest = fy_get(snapshot, "root", "");
	object = fyai_project_get(objects, digest);
	part = path_hex;
	while (*part && fy_is_mapping(object)) {
		if (++depth > FYAI_PROJECT_MAX_DEPTH)
			return fy_invalid;
		end = part;
		while (*end && strncmp(end, FYAI_PROJECT_HEX_SLASH, FYAI_PROJECT_HEX_SLASH_LEN)) {
			if (!end[1])
				return fy_invalid;
			end += 2;
		}
		length = end - part;
		entries = fy_get(object, "entries", fy_invalid);
		object = fy_invalid;
		/* Entries are in the byte order of their hex names. */
		for (low = 0, high = fy_len(entries); low < high;) {
			middle = low + (high - low) / 2;
			entry = fy_get_at(entries, middle);
			name = fy_get(entry, "name_hex", "");
			rc = project_key_compare(part, length, name, strlen(name));
			if (rc < 0)
				high = middle;
			else if (rc > 0)
				low = middle + 1;
			else {
				digest = fy_get(entry, "digest", "");
				object = fyai_project_get(objects, digest);
				break;
			}
		}
		part = *end ? end + FYAI_PROJECT_HEX_SLASH_LEN : end;
	}
	return object;
}

fy_generic fyai_project_attributes(fy_generic snapshot, const char *path_hex, fy_generic object)
{
	fy_generic attributes, value;

	attributes = fy_get(snapshot, "attributes", fy_invalid);
	value = fyai_project_get(attributes, path_hex);
	return fy_is_mapping(value) ? value : object;
}

/* The hex path of the manifest being compared, grown in place for each level. */
struct project_walk {
	char *path;
	size_t length, capacity;
	size_t count;
};

/* Append a path component; return the length to restore, or SIZE_MAX. */
static size_t project_walk_push(struct project_walk *walk, const char *name)
{
	size_t mark = walk->length, need = strlen(name) + 3;
	char *grown;

	if (walk->length + need > walk->capacity) {
		grown = realloc(walk->path, (walk->capacity + need) * 2);
		if (!grown)
			return SIZE_MAX;
		walk->path = grown;
		walk->capacity = (walk->capacity + need) * 2;
	}
	if (walk->length) {
		memcpy(walk->path + walk->length, FYAI_PROJECT_HEX_SLASH, FYAI_PROJECT_HEX_SLASH_LEN);
		walk->length += FYAI_PROJECT_HEX_SLASH_LEN;
	}
	memcpy(walk->path + walk->length, name, strlen(name) + 1);
	walk->length += strlen(name);
	return mark;
}

static void project_walk_pop(struct project_walk *walk, size_t mark)
{
	walk->length = mark;
	walk->path[mark] = '\0';
}

static bool project_equal_path(fy_generic a, fy_generic b, const char *digest,
			       struct project_walk *walk, unsigned int depth)
{
	fy_generic left, right, la, ra, entries, entry, value;
	const char *name, *child;
	size_t mark;
	bool equal;

	if (depth > FYAI_PROJECT_MAX_DEPTH || ++walk->count > FYAI_PROJECT_MAX_NODES)
		return false;
	left = fyai_project_get(fy_get(a, "objects", fy_invalid), digest);
	right = fyai_project_get(fy_get(b, "objects", fy_invalid), digest);
	if (!fy_is_mapping(left) || !fy_is_mapping(right))
		return false;
	la = fyai_project_attributes(a, walk->path, left);
	ra = fyai_project_attributes(b, walk->path, right);
	if (!fy_equal(fy_get(la, "mtime_sec", fy_invalid), fy_get(ra, "mtime_sec", fy_invalid)) ||
	    !fy_equal(fy_get(la, "mtime_nsec", fy_invalid), fy_get(ra, "mtime_nsec", fy_invalid)))
		return false;
	entries = fy_get(left, "entries", fy_invalid);
	fy_foreach(entry, entries) {
		value = fy_get(entry, "name_hex", fy_invalid);
		name = fy_castp(&value, "");
		mark = project_walk_push(walk, name);
		if (mark == SIZE_MAX)
			return false;
		child = fy_get(entry, "digest", "");
		equal = project_equal_path(a, b, child, walk, depth + 1);
		project_walk_pop(walk, mark);
		if (!equal)
			return false;
	}
	return true;
}

bool fyai_project_snapshot_equal(fy_generic a, fy_generic b)
{
	struct project_walk walk = { .capacity = PATH_MAX };
	fy_generic root;
	bool equal;

	root = fy_get(a, "root", fy_invalid);
	if (!fy_is_string(root) || !fy_equal(root, fy_get(b, "root", fy_invalid)) ||
	    !fy_equal(fy_get(a, "version", fy_invalid), 2LL) ||
	    !fy_equal(fy_get(b, "version", fy_invalid), 2LL))
		return false;
	walk.path = calloc(1, walk.capacity);
	if (!walk.path)
		return false;
	equal = project_equal_path(a, b, fy_castp(&root, ""), &walk, 0);
	free(walk.path);
	return equal;
}

int fyai_project_check_borrowed(int objects_fd, fy_generic snapshot, bool full, char *error,
				size_t error_size)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher = NULL;
	struct fyai_cas_blob blob = { .borrowed = true };
	fy_generic object, objects, value, key;
	const char *digest;
	struct stat st;
	int rc = 0, saved, fd;

	if (!fy_equal(fy_get(snapshot, "version", fy_invalid), 2LL)) {
		errno = ENOTSUP;
		return -1;
	}
	objects = fy_get(snapshot, "objects", fy_invalid);
	fy_foreach_key_value(key, object, objects) {
		value = fy_get(object, "blob", fy_invalid);
		if (!fy_equal(fy_get(value, "storage", fy_invalid), "borrowed"))
			continue;
		if (full && !hasher) {
			hasher = fy_blake3_hasher_create(&cfg);
			if (!hasher) {
				errno = ENOMEM;
				return -1;
			}
		}
		digest = fy_get(value, "digest", "");
		if (strlen(digest) != 64 || fy_get(value, "size", -1LL) < 0) {
			errno = EINVAL;
			rc = -1;
			break;
		}
		rc = fyai_cas_digest_parse(blob.digest, digest);
		if (rc)
			break;
		blob.size = fy_get(value, "size", 0LL);
		value = fy_get(object, "borrowed", fy_invalid);
		if (!fy_is_mapping(value)) {
			errno = EINVAL;
			rc = -1;
			break;
		}
		blob.source_device = fy_get(value, "device", 0LL);
		blob.source_inode = fy_get(value, "inode", 0LL);
		blob.source_mode = fy_get(value, "mode", 0LL);
		blob.source_uid = fy_get(value, "uid", 0LL);
		blob.source_gid = fy_get(value, "gid", 0LL);
		if (full)
			rc = fyai_cas_verify_hasher(objects_fd, &blob, hasher);
		else {
			fd = fyai_cas_open(objects_fd, &blob);
			rc = fd < 0 ? -1 : fstat(fd, &st);
			if (!rc && (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != blob.size ||
				    !fyai_cas_borrowed_matches(&st, &blob))) {
				errno = EIO;
				rc = -1;
			}
			saved = errno;
			if (fd >= 0)
				close(fd);
			errno = saved;
		}
		if (rc) {
			if (error && error_size)
				snprintf(error, error_size,
					 "borrowed Git object %s; recreate "
					 "with --copy-git-objects",
					 digest);
			break;
		}
	}
	saved = errno;
	if (hasher)
		fy_blake3_hasher_destroy(hasher);
	errno = saved;
	return rc;
}

int fyai_project_verify_borrowed(int objects_fd, fy_generic snapshot, char *error,
				 size_t error_size)
{
	return fyai_project_check_borrowed(objects_fd, snapshot, true, error, error_size);
}
