/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROJECT_H
#define FYAI_PROJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_cas.h"

/*
 * Snapshots are untrusted input. Walks of a recorded root stop at this nesting
 * depth, and at this many manifests: the root and at most 100000 entries.
 */
#define FYAI_PROJECT_MAX_DEPTH 128
#define FYAI_PROJECT_MAX_NODES 100001

enum fyai_project_kind {
	FYAI_PROJECT_FILE = 1,
	FYAI_PROJECT_DIRECTORY,
	FYAI_PROJECT_SYMLINK,
};

struct fyai_project_metadata {
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	int64_t mtime_sec;
	uint32_t mtime_nsec;
};

struct fyai_project_entry {
	const unsigned char *name;
	size_t name_length;
	enum fyai_project_kind kind;
	/* Raw BLAKE3 identity of the child manifest. */
	unsigned char digest[FYAI_CAS_HASH_SIZE];
};

/*
 * Build a directory manifest in the caller's builder and its BLAKE3 identity.
 * Children are copied and sorted by unsigned filename bytes. Names use hex in
 * the manifest so arbitrary Unix filename bytes remain distinct. No blob or
 * filesystem observation fields belong to a directory manifest.
 * project_root reserves .fyai. Entries are already captured child identities;
 * this call neither scans the host nor publishes references to durable storage.
 * On failure return fy_invalid and set errno; digest is valid only on success.
 */
fy_generic fyai_project_directory(struct fy_generic_builder *gb,
				  const struct fyai_project_metadata *metadata,
				  const struct fyai_project_entry *entries, size_t count,
				  bool project_root, char digest[FYAI_CAS_DIGEST_SIZE]);

struct fy_blake3_hasher;

/*
 * The BLAKE3 identity of a manifest without building one. A hasher that the
 * caller lends is reset and used; with NULL the call makes its own. The
 * identity does not depend on the timestamp of the metadata.
 */
int fyai_project_file_digest(struct fy_blake3_hasher *hasher,
			     const struct fyai_project_metadata *metadata,
			     const struct fyai_cas_blob *blob,
			     unsigned char digest[FYAI_CAS_HASH_SIZE]);
int fyai_project_symlink_digest(struct fy_blake3_hasher *hasher,
				const struct fyai_project_metadata *metadata,
				const unsigned char *target, size_t length,
				unsigned char digest[FYAI_CAS_HASH_SIZE]);

/*
 * Check the names of directory entries, and sort them by their bytes: the
 * order of the identity and of the stored directory. Return 0, or -1 with errno
 * set to EINVAL for a name that is not valid and EEXIST for a repeated name.
 */
int fyai_project_entries_prepare(struct fyai_project_entry *entries, size_t count,
				 bool project_root);

/* The identity of a directory; the entries are prepared. */
int fyai_project_directory_digest(struct fy_blake3_hasher *hasher,
				  const struct fyai_project_metadata *metadata,
				  const struct fyai_project_entry *entries, size_t count,
				  unsigned char digest[FYAI_CAS_HASH_SIZE]);

/* Returned manifests are owned by gb; payload bytes and metadata are copied. */
fy_generic fyai_project_file(struct fy_generic_builder *gb,
			     const struct fyai_project_metadata *metadata,
			     const struct fyai_cas_blob *blob, char digest[FYAI_CAS_DIGEST_SIZE]);
fy_generic fyai_project_symlink(struct fy_generic_builder *gb,
				const struct fyai_project_metadata *metadata,
				const unsigned char *target, size_t length,
				char digest[FYAI_CAS_DIGEST_SIZE]);

/*
 * A manifest path is the hex form of each component. Components are joined by
 * the hex form of '/' (0x2f), so any byte sequence is a plain text key.
 */
#define FYAI_PROJECT_HEX_SLASH "2f"
#define FYAI_PROJECT_HEX_SLASH_LEN (sizeof(FYAI_PROJECT_HEX_SLASH) - 1)

/*
 * The objects and attributes tables of a snapshot keep their keys in byte
 * order, so that a lookup is a binary search over the stored pairs. fy_get()
 * scans a mapping, which makes a walk over a large table quadratic.
 */

/* Value of key in a table of sorted keys, or NULL. The pointer names the table. */
const fy_generic *fyai_project_find(fy_generic table, const char *key);

/* The value of key in a table of sorted keys, or fy_invalid. */
fy_generic fyai_project_get(fy_generic table, const char *key);

/*
 * Build a table from count key and value pairs. The pairs are sorted in
 * place by key; the table belongs to gb.
 */
fy_generic fyai_project_table_create(struct fy_generic_builder *gb, size_t count,
				     fy_generic *pairs);

/* Borrowed manifest at a hex-encoded relative path; invalid if absent. */
fy_generic fyai_project_lookup(fy_generic snapshot, const char *path_hex);

/* Borrowed object or timestamp override; storage belongs to snapshot. */
fy_generic fyai_project_attributes(fy_generic snapshot, const char *path_hex, fy_generic object);

/*
 * Verify every borrowed object before mounting or reusing a recorded root.
 * Check borrowed inode metadata; full also hashes the object bytes.
 */
int fyai_project_check_borrowed(int objects_fd, fy_generic snapshot, bool full, char *error,
				size_t error_size);
int fyai_project_verify_borrowed(int objects_fd, fy_generic snapshot, char *error,
				 size_t error_size);

/*
 * Compare content roots and effective timestamps, independent of sparse
 * encoding.
 */
bool fyai_project_snapshot_equal(fy_generic a, fy_generic b);

/*
 * Return sorted content and metadata changes in gb, excluding .git and .fyai.
 */
fy_generic fyai_project_diff(struct fy_generic_builder *gb, fy_generic left, fy_generic right);

#endif
