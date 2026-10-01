/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
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

	length = (*left)->name_length < (*right)->name_length ?
		 (*left)->name_length : (*right)->name_length;
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

/* Canonical integers are eight-byte big-endian words, including signed seconds. */
static void project_hash_number(struct fy_blake3_hasher *hasher, uint64_t value)
{
	unsigned char bytes[8];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++)
		bytes[sizeof(bytes) - i - 1] = (unsigned char)(value >> (i * 8));
	fy_blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

fy_generic fyai_project_directory(struct fy_generic_builder *gb,
		const struct fyai_project_metadata *metadata,
		const struct fyai_project_entry *entries, size_t count,
		bool project_root, char digest[FYAI_CAS_DIGEST_SIZE])
{
	static const char domain[] = "fyai/project/directory/blake3/v1";
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher = NULL;
	const struct fyai_project_entry **sorted = NULL, *entry;
	const uint8_t *hash;
	fy_generic children, item, result = fy_invalid;
	char *name = NULL;
	size_t i;
	int saved;

	if (!gb || !metadata || !digest || (count && !entries) ||
	    metadata->mode > 07777 || metadata->mtime_nsec >= 1000000000 ||
	    count > SIZE_MAX / sizeof(*sorted)) {
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
	project_hash_number(hasher, (uint64_t)metadata->mtime_sec);
	project_hash_number(hasher, metadata->mtime_nsec);
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
		item = fy_mapping(gb, "name_hex", fy_value(gb, name),
				  "kind", fy_value(gb, project_kind_name(entry->kind)),
				  "digest", fy_value(gb, entry->digest));
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
		fy_blake3_hasher_update(hasher, entry->digest, FYAI_CAS_DIGEST_SIZE - 1);
	}
	result = fy_mapping(gb, "version", 1LL, "kind", "directory",
		"algorithm", "blake3", "mode", (long long)metadata->mode,
		"uid", (long long)metadata->uid, "gid", (long long)metadata->gid,
		"mtime_sec", (long long)metadata->mtime_sec,
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
