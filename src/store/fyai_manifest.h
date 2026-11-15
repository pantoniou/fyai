/* SPDX-License-Identifier: MIT */
#ifndef FYAI_MANIFEST_H
#define FYAI_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libfyaml/libfyaml-generic.h>

#include "fyai_cas.h"
#include "fyai_project.h"

struct fyai_scratch;

/*
 * A project manifest is one immutable file of binary records: the Merkle
 * objects of a captured project, an index of them by digest, and the table of
 * path timestamps that differ from the timestamp of their object. A lookup is
 * a binary search over the index and reads the record in place, so a
 * manifest of a large tree costs no allocation per node.
 *
 * Integers are little-endian and every field has its natural alignment, so a
 * record is read through a struct without a copy. The identity of an object is
 * the digest of fyai_project.h; this file does not define it.
 */

#define FYAI_MANIFEST_VERSION 2

/* A name of a manifest file is the hex of the digest of its bytes. */
#define FYAI_MANIFEST_NAME_SIZE FYAI_CAS_DIGEST_SIZE

/* One object, decoded from a record. Pointers name the record. */
struct fyai_mobject {
	enum fyai_project_kind kind;
	struct fyai_project_metadata meta;
	/* A file: the blob; the digest, size and the borrowed provenance. */
	struct fyai_cas_blob blob;
	/* A symlink: the target bytes. */
	const unsigned char *target;
	size_t target_length;
	/* A directory: the entries, in name order, in their record encoding. */
	const unsigned char *entries;
	size_t entries_length;
	uint32_t entry_count;
};

/* Iterator over the entries of a directory object. */
struct fyai_mdir {
	const unsigned char *position;
	const unsigned char *end;
	uint32_t remaining;
};

/*
 * A manifest in memory: mapped from a file, or an image that it owns.
 *
 * A full manifest holds every object of its tree. A delta holds the records
 * that differ from a full manifest that is its base, the digests of the base
 * objects that the tree no longer has, and the same for the path timestamps.
 * A lookup in a delta reads its own tables first and then the base, so a delta
 * of a few changes over a large tree is small. A base is always a full manifest.
 * The counts of the own tables are object_count and attribute_count;
 * object_total is the number of objects of the whole tree.
 */
struct fyai_manifest {
	const unsigned char *data;
	size_t size;
	bool mapped;
	bool owned;
	bool delta;
	/* The base of a delta, opened with it and closed with it. */
	struct fyai_manifest *base;
	uint64_t object_count;
	uint64_t attribute_count;
	uint64_t object_total;
	uint64_t index_offset;
	uint64_t objects_offset;
	uint64_t objects_size;
	uint64_t attribute_index_offset;
	uint64_t attributes_offset;
	uint64_t attributes_size;
	uint64_t removed_count;
	uint64_t removed_offset;
	uint64_t removed_attribute_index_offset;
	uint64_t removed_attribute_count;
	uint64_t removed_attributes_offset;
	uint64_t removed_attributes_size;
	unsigned char root[FYAI_CAS_HASH_SIZE];
	unsigned char base_digest[FYAI_CAS_HASH_SIZE];
};

/*
 * Map a manifest file and use it in place. A delta opens its base from the
 * same directory. Return 0, or -1 with errno set:
 * EBADMSG when the header is not that of a manifest or a region lies outside
 * the file. There is no pass over the records: an access checks the offset that
 * it follows, and a record is checked as it is decoded.
 */
int fyai_manifest_open_at(struct fyai_manifest *manifest, int directory_fd, const char *name);
void fyai_manifest_close(struct fyai_manifest *manifest);

const unsigned char *fyai_manifest_root(const struct fyai_manifest *manifest);

/* Find an object by digest. The result is valid while the manifest is open. */
bool fyai_manifest_find(const struct fyai_manifest *manifest,
			const unsigned char digest[FYAI_CAS_HASH_SIZE], struct fyai_mobject *object);

/*
 * The record of an object, as stored, or NULL when the digest is absent. The
 * length is set from the record.
 */
const unsigned char *fyai_manifest_record(const struct fyai_manifest *manifest,
					  const unsigned char digest[FYAI_CAS_HASH_SIZE],
					  size_t *length);

/* The object at an index position, with its digest. False when out of range. */
bool fyai_manifest_object_at(const struct fyai_manifest *manifest, size_t position,
			     unsigned char digest[FYAI_CAS_HASH_SIZE], struct fyai_mobject *object);

/* Return 0, or -1 with errno set to EBADMSG for a damaged directory. */
int fyai_mdir_open(const struct fyai_mobject *object, struct fyai_mdir *directory);

/* The next entry; the name points into the record. False at the end or on damage. */
bool fyai_mdir_next(struct fyai_mdir *directory, struct fyai_project_entry *entry);

/*
 * Find an object by path: component names joined by '/', and an empty path
 * for the root. The path has no leading slash.
 */
bool fyai_manifest_lookup(const struct fyai_manifest *manifest, const unsigned char *path,
			  size_t length, struct fyai_mobject *object,
			  unsigned char digest[FYAI_CAS_HASH_SIZE]);

/* The timestamp that overrides the one of the object at a path. */
bool fyai_manifest_attribute(const struct fyai_manifest *manifest, const unsigned char *path,
			     size_t length, int64_t *sec, uint32_t *nsec);

/* The attribute at an index position: the path, which names the manifest. */
bool fyai_manifest_attribute_at(const struct fyai_manifest *manifest, size_t position,
				const unsigned char **path, size_t *length, int64_t *sec,
				uint32_t *nsec);

/*
 * Write the manifest to a file in directory_fd, named by the hex of the digest
 * of its bytes, and return that name. The file is published by rename, so a
 * reader never sees a partial file, and an existing file of the name is
 * identical by construction. With durable, the file is flushed first.
 */
int fyai_manifest_write(const struct fyai_manifest *manifest, int directory_fd, bool durable,
			char name[FYAI_MANIFEST_NAME_SIZE]);

/*
 * Convert to and from the snapshot generic of version 2, for code that reads
 * a snapshot as a generic. The conversion costs a generic for every object. A
 * delta converts to the generic of its whole tree, with the base objects that it
 * keeps; only a full manifest is made from a snapshot.
 */
fy_generic fyai_manifest_to_snapshot(struct fy_generic_builder *gb,
				     const struct fyai_manifest *manifest);
int fyai_manifest_from_snapshot(struct fyai_manifest *manifest, fy_generic snapshot);

/*
 * Check every borrowed object of the manifest before the manifest is mounted or
 * reused: the inode metadata, and with full the bytes. A failure fills error
 * with the object and sets errno.
 */
int fyai_manifest_check_borrowed(const struct fyai_manifest *manifest, int objects_fd, bool full,
				 char *error, size_t error_size);

/*
 * The record of an object as a manifest stores it, to compare it with a stored
 * record or to add it. The size is 0 for an object of no valid kind; put writes
 * that many bytes at an address aligned to 8 bytes. For a directory, the
 * entries are the children in name order.
 */
size_t fyai_manifest_record_size(const struct fyai_mobject *object,
				 const struct fyai_project_entry *entries);
void fyai_manifest_record_put(unsigned char *record, const struct fyai_mobject *object,
			      const struct fyai_project_entry *entries);

/*
 * Whether two manifests record the same tree: the same root, and the same
 * effective timestamp at every path. A timestamp is the attribute of its path,
 * else the one of its object.
 */
bool fyai_manifest_equal(const struct fyai_manifest *a, const struct fyai_manifest *b);

/*
 * The changes from one manifest to another, as a sequence of rows in path
 * order: path, path_hex, status (added, deleted, modified), before and after.
 * The entries .git and .fyai are left out at every depth.
 */
fy_generic fyai_manifest_diff(struct fy_generic_builder *gb, const struct fyai_manifest *left,
			      const struct fyai_manifest *right);

/* Builder of a manifest image; its temporaries use a scratch arena. */
struct fyai_manifest_attribute {
	const unsigned char *path;
	size_t length;
	int64_t sec;
	uint32_t nsec;
};

struct fyai_manifest_builder {
	struct fyai_scratch *scratch;
	unsigned char *records;
	size_t records_size, records_capacity;
	struct fyai_manifest_index *index;
	size_t count, capacity;
	uint32_t *slots;
	size_t slot_mask;
	struct fyai_manifest_attribute *attributes;
	size_t attribute_count, attribute_capacity;
	/* A delta only: the digests and the paths that the base has and the tree has not. */
	unsigned char (*removed)[FYAI_CAS_HASH_SIZE];
	size_t removed_count, removed_capacity;
	struct fyai_manifest_attribute *removed_attributes;
	size_t removed_attribute_count, removed_attribute_capacity;
};

int fyai_manifest_builder_init(struct fyai_manifest_builder *builder, struct fyai_scratch *scratch);

/* True when the digest is already in the builder. */
bool fyai_manifest_builder_has(const struct fyai_manifest_builder *builder,
			       const unsigned char digest[FYAI_CAS_HASH_SIZE]);

/*
 * Add an object; an object already present is kept, and the call succeeds. For a
 * directory the entries are the children in name order. Return 0, or -1 with
 * errno set.
 */
int fyai_manifest_builder_add(struct fyai_manifest_builder *builder,
			      const unsigned char digest[FYAI_CAS_HASH_SIZE],
			      const struct fyai_mobject *object,
			      const struct fyai_project_entry *entries);

/* Add a record as another manifest stores it. */
int fyai_manifest_builder_add_record(struct fyai_manifest_builder *builder,
				     const unsigned char digest[FYAI_CAS_HASH_SIZE],
				     const unsigned char *record, size_t length);

/* A path timestamp; the path must outlive the builder. */
int fyai_manifest_builder_attribute(struct fyai_manifest_builder *builder,
				    const unsigned char *path, size_t length, int64_t sec,
				    uint32_t nsec);

/* For a delta: a digest of the base that the tree has not, and a path likewise. */
int fyai_manifest_builder_remove(struct fyai_manifest_builder *builder,
				 const unsigned char digest[FYAI_CAS_HASH_SIZE]);
int fyai_manifest_builder_remove_attribute(struct fyai_manifest_builder *builder,
					   const unsigned char *path, size_t length);

/*
 * Assemble a delta manifest over the base that the digest names: the digest of
 * the bytes of the base file. The root must be an object of the delta or of the
 * base, and object_total is the number of objects of the whole tree.
 */
int fyai_manifest_builder_finish_delta(struct fyai_manifest_builder *builder,
				       const unsigned char root[FYAI_CAS_HASH_SIZE],
				       const unsigned char base_digest[FYAI_CAS_HASH_SIZE],
				       uint64_t object_total, struct fyai_manifest *manifest);

/*
 * Make the delta of a full manifest over a full base. The base_name is the name
 * of the file of the base. Return 0, or -1 with errno set.
 */
int fyai_manifest_make_delta(const struct fyai_manifest *full, const struct fyai_manifest *base,
			     const char base_name[FYAI_MANIFEST_NAME_SIZE],
			     struct fyai_manifest *delta);

/*
 * Assemble the manifest: the result owns one malloc'ed image. An attribute whose
 * path is not in the tree of the root is dropped. Return 0, or -1 with errno.
 */
int fyai_manifest_builder_finish(struct fyai_manifest_builder *builder,
				 const unsigned char root[FYAI_CAS_HASH_SIZE],
				 struct fyai_manifest *manifest);

#endif
