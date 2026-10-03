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
	const struct fyai_project_entry *const *left = a, *const *right = b;
	size_t length;
	int rc;

	length = (*left)->name_length < (*right)->name_length ? (*left)->name_length :
								(*right)->name_length;
	rc = memcmp((*left)->name, (*right)->name, length);
	if (rc)
		return rc;
	return ((*left)->name_length > (*right)->name_length) -
	       ((*left)->name_length < (*right)->name_length);
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
	for (i = 0; i < FYAI_CAS_DIGEST_SIZE - 1; i++)
		if (!((entry->digest[i] >= '0' && entry->digest[i] <= '9') ||
		      (entry->digest[i] >= 'a' && entry->digest[i] <= 'f')))
			return false;
	return !entry->digest[i];
}

static void project_hex(char *out, const unsigned char *bytes, size_t count)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < count; i++) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 15];
	}
	out[count * 2] = '\0';
}

/* Canonical integers are eight-byte big-endian words, independent of host
 * representation. */
static void project_hash_number(struct fy_blake3_hasher *hasher, uint64_t value)
{
	unsigned char bytes[8];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++)
		bytes[sizeof(bytes) - i - 1] = (unsigned char)(value >> (i * 8));
	fy_blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

static void project_hash_digest(struct fy_blake3_hasher *hasher, const char *digest)
{
	unsigned char bytes[32];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] =
			(unsigned char)(((digest[i * 2] <= '9' ? digest[i * 2] - '0' :
								 digest[i * 2] - 'a' + 10)
					 << 4) |
					(digest[i * 2 + 1] <= '9' ? digest[i * 2 + 1] - '0' :
								    digest[i * 2 + 1] - 'a' + 10));
	fy_blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

fy_generic fyai_project_directory(struct fy_generic_builder *gb,
				  const struct fyai_project_metadata *metadata,
				  const struct fyai_project_entry *entries, size_t count,
				  bool project_root, char digest[FYAI_CAS_DIGEST_SIZE])
{
	static const char domain[] = "fyai/project/directory/blake3/v2";
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher = NULL;
	const struct fyai_project_entry **sorted = NULL, *entry;
	const uint8_t *hash;
	fy_generic children, item, result = fy_invalid;
	char *name = NULL;
	size_t i;
	int saved;

	if (!gb || !metadata || !digest || (count && !entries) || metadata->mode > 07777 ||
	    metadata->mtime_nsec >= 1000000000 || count > SIZE_MAX / sizeof(*sorted)) {
		errno = EINVAL;
		return fy_invalid;
	}
	if (count) {
		sorted = malloc(count * sizeof(*sorted));
		if (!sorted)
			return fy_invalid;
		for (i = 0; i < count; i++) {
			if (!project_entry_valid(&entries[i], project_root)) {
				errno = EINVAL;
				goto out;
			}
			sorted[i] = &entries[i];
		}
		qsort(sorted, count, sizeof(*sorted), project_entry_compare);
		for (i = 1; i < count; i++) {
			if (!project_entry_compare(&sorted[i - 1], &sorted[i])) {
				errno = EEXIST;
				goto out;
			}
		}
	}
	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		goto out;
	}
	fy_blake3_hasher_update(hasher, domain, sizeof(domain));
	project_hash_number(hasher, metadata->mode);
	project_hash_number(hasher, metadata->uid);
	project_hash_number(hasher, metadata->gid);
	project_hash_number(hasher, count);
	children = fy_sequence(gb);
	for (i = 0; i < count; i++) {
		entry = sorted[i];
		name = malloc(entry->name_length * 2 + 1);
		if (!name) {
			errno = ENOMEM;
			goto out;
		}
		project_hex(name, entry->name, entry->name_length);
		item = fy_mapping(gb, "name_hex", fy_value(gb, name), "kind",
				  fy_value(gb, project_kind_name(entry->kind)), "digest",
				  fy_value(gb, entry->digest));
		free(name);
		name = NULL;
		children = fy_append(gb, children, item);
		if (!fy_is_valid(item) || !fy_is_valid(children)) {
			errno = ENOMEM;
			goto out;
		}
		project_hash_number(hasher, entry->name_length);
		fy_blake3_hasher_update(hasher, entry->name, entry->name_length);
		project_hash_number(hasher, entry->kind);
		project_hash_digest(hasher, entry->digest);
	}
	result = fy_mapping(gb, "version", 2LL, "kind", "directory", "algorithm", "blake3", "mode",
			    (long long)metadata->mode, "uid", (long long)metadata->uid, "gid",
			    (long long)metadata->gid, "mtime_sec", (long long)metadata->mtime_sec,
			    "mtime_nsec", (long long)metadata->mtime_nsec, "entries", children);
	if (!fy_is_valid(result)) {
		errno = ENOMEM;
		goto out;
	}
	hash = fy_blake3_hasher_finalize(hasher);
	if (!hash) {
		errno = EIO;
		result = fy_invalid;
		goto out;
	}
	project_hex(digest, hash, FY_BLAKE3_OUT_LEN);
out:
	saved = errno;
	free(name);
	free(sorted);
	if (hasher)
		fy_blake3_hasher_destroy(hasher);
	errno = saved;
	return result;
}

static fy_generic project_leaf(struct fy_generic_builder *gb,
			       const struct fyai_project_metadata *metadata, const char *kind,
			       const void *payload, size_t length, uint64_t size,
			       char digest[FYAI_CAS_DIGEST_SIZE])
{
	static const char domain[] = "fyai/project/leaf/blake3/v2";
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	const uint8_t *hash;
	fy_generic result;

	if (!gb || !metadata || !digest || (!payload && length) || metadata->mode > 07777 ||
	    metadata->mtime_nsec >= 1000000000) {
		errno = EINVAL;
		return fy_invalid;
	}
	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		return fy_invalid;
	}
	fy_blake3_hasher_update(hasher, domain, sizeof(domain));
	fy_blake3_hasher_update(hasher, kind, strlen(kind) + 1);
	project_hash_number(hasher, metadata->mode);
	project_hash_number(hasher, metadata->uid);
	project_hash_number(hasher, metadata->gid);
	project_hash_number(hasher, size);
	if (!strcmp(kind, "file"))
		project_hash_digest(hasher, payload);
	else
		fy_blake3_hasher_update(hasher, payload, length);
	hash = fy_blake3_hasher_finalize(hasher);
	if (!hash) {
		fy_blake3_hasher_destroy(hasher);
		errno = EIO;
		return fy_invalid;
	}
	project_hex(digest, hash, FY_BLAKE3_OUT_LEN);
	fy_blake3_hasher_destroy(hasher);
	result = fy_mapping(gb, "version", 2LL, "algorithm", "blake3", "kind", kind, "mode",
			    (long long)metadata->mode, "uid", (long long)metadata->uid, "gid",
			    (long long)metadata->gid, "mtime_sec", (long long)metadata->mtime_sec,
			    "mtime_nsec", (long long)metadata->mtime_nsec);
	return result;
}

fy_generic fyai_project_file(struct fy_generic_builder *gb,
			     const struct fyai_project_metadata *metadata,
			     const struct fyai_cas_blob *blob, char digest[FYAI_CAS_DIGEST_SIZE])
{
	struct fyai_project_entry check = { .name = (const unsigned char *)"file",
					    .name_length = 4,
					    .kind = FYAI_PROJECT_FILE };
	fy_generic result;

	if (!blob || blob->size > INT64_MAX) {
		errno = EINVAL;
		return fy_invalid;
	}
	memcpy(check.digest, blob->digest, sizeof(check.digest));
	if (!project_entry_valid(&check, false)) {
		errno = EINVAL;
		return fy_invalid;
	}
	result = project_leaf(gb, metadata, "file", blob->digest, 64, blob->size, digest);
	if (!fy_is_valid(result))
		return fy_invalid;
	result = fy_assoc(gb, result, "blob",
			  fy_mapping(gb, "algorithm", "blake3", "digest",
				     fy_value(gb, blob->digest), "size", (long long)blob->size,
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
	project_hex(encoded, target, length);
	result = project_leaf(gb, metadata, "symlink", target, length, length, digest);
	if (fy_is_valid(result))
		result = fy_assoc(gb, result, "target_hex", fy_value(gb, encoded));
	free(encoded);
	return result;
}

fy_generic fyai_project_lookup(fy_generic snapshot, const char *path_hex)
{
	fy_generic objects, object, entries, entry;
	const char *part, *end, *name, *digest;
	size_t length;
	unsigned int depth = 0;

	objects = fy_get(snapshot, "objects", fy_invalid);
	digest = fy_get(snapshot, "root", "");
	object = fy_get(objects, digest, fy_invalid);
	part = path_hex;
	while (*part && fy_is_mapping(object)) {
		if (++depth > 128)
			return fy_invalid;
		end = part;
		while (*end && strncmp(end, "2f", 2)) {
			if (!end[1])
				return fy_invalid;
			end += 2;
		}
		length = end - part;
		entries = fy_get(object, "entries", fy_invalid);
		object = fy_invalid;
		fy_foreach(entry, entries) {
			name = fy_get(entry, "name_hex", "");
			if (strlen(name) == length && !memcmp(name, part, length)) {
				digest = fy_get(entry, "digest", "");
				object = fy_get(objects, digest, fy_invalid);
				break;
			}
		}
		part = *end ? end + 2 : end;
	}
	return object;
}

fy_generic fyai_project_attributes(fy_generic snapshot, const char *path_hex, fy_generic object)
{
	fy_generic attributes, value;

	attributes = fy_get(snapshot, "attributes", fy_invalid);
	value = fy_get(attributes, path_hex, fy_invalid);
	return fy_is_mapping(value) ? value : object;
}

static bool project_equal_path(fy_generic a, fy_generic b, const char *digest, const char *path,
			       unsigned int depth, size_t *count)
{
	fy_generic left, right, la, ra, entries, entry, value;
	const char *name, *child;
	char *next;
	size_t length;
	bool equal;

	if (depth > 128 || ++*count > 100001)
		return false;
	left = fy_get(fy_get(a, "objects", fy_invalid), digest, fy_invalid);
	right = fy_get(fy_get(b, "objects", fy_invalid), digest, fy_invalid);
	if (!fy_is_mapping(left) || !fy_is_mapping(right))
		return false;
	la = fyai_project_attributes(a, path, left);
	ra = fyai_project_attributes(b, path, right);
	if (!fy_equal(fy_get(la, "mtime_sec", fy_invalid), fy_get(ra, "mtime_sec", fy_invalid)) ||
	    !fy_equal(fy_get(la, "mtime_nsec", fy_invalid), fy_get(ra, "mtime_nsec", fy_invalid)))
		return false;
	entries = fy_get(left, "entries", fy_invalid);
	fy_foreach(entry, entries) {
		value = fy_get(entry, "name_hex", fy_invalid);
		name = fy_castp(&value, "");
		length = strlen(path) + strlen(name) + 3;
		next = malloc(length);
		if (!next)
			return false;
		strcpy(next, path);
		if (*path)
			strcat(next, "2f");
		strcat(next, name);
		child = fy_get(entry, "digest", "");
		equal = project_equal_path(a, b, child, next, depth + 1, count);
		free(next);
		if (!equal)
			return false;
	}
	return true;
}

bool fyai_project_snapshot_equal(fy_generic a, fy_generic b)
{
	fy_generic root;
	size_t count = 0;

	root = fy_get(a, "root", fy_invalid);
	if (!fy_is_string(root) || !fy_equal(root, fy_get(b, "root", fy_invalid)) ||
	    !fy_equal(fy_get(a, "version", fy_invalid), 2LL) ||
	    !fy_equal(fy_get(b, "version", fy_invalid), 2LL))
		return false;
	return project_equal_path(a, b, fy_castp(&root, ""), "", 0, &count);
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
		memcpy(blob.digest, digest, sizeof(blob.digest));
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
				    (uint64_t)st.st_dev != blob.source_device ||
				    (uint64_t)st.st_ino != blob.source_inode ||
				    (st.st_mode & 07777) != blob.source_mode ||
				    st.st_uid != blob.source_uid || st.st_gid != blob.source_gid)) {
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
