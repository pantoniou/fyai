/*
 * fyai_manifest.c - binary project manifests
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
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libfyaml/libfyaml-blake3.h>
#include <libfyaml/libfyaml-endian.h>
#include <openssl/rand.h>

#include "fyai_manifest.h"
#include "fyai_project.h"
#include "fyai_scratch.h"
#include "utils.h"

/*
 * File layout: the header, the object index, the object records, the attribute
 * index and the attribute records. The file is little-endian. Every field has
 * its natural alignment, and every record starts at a multiple of 8 bytes, so
 * a record is read through a struct: a big-endian host converts each field
 * with le*toh() and a little-endian host does not.
 */
#define FYMF_MAGIC UINT32_C(0x464d5946)
#define PACKED __attribute__((packed))

struct fymf_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t flags;
	uint32_t reserved;
	uint64_t object_count;
	uint64_t attribute_count;
	uint64_t object_total;
	uint64_t index_offset;
	uint64_t objects_offset;
	uint64_t objects_size;
	uint64_t attribute_index_offset;
	uint64_t attributes_offset;
	uint64_t attributes_size;
	uint64_t removed_offset;
	uint64_t removed_count;
	uint64_t removed_attribute_index_offset;
	uint64_t removed_attribute_count;
	uint64_t removed_attributes_offset;
	uint64_t removed_attributes_size;
	uint64_t file_size;
	unsigned char root[FYAI_CAS_HASH_SIZE];
	unsigned char base[FYAI_CAS_HASH_SIZE];
} PACKED;

#define FYMF_FLAG_DELTA 0x1

/* Index entry: the digest and the offset of the record in the object region. */
struct fyai_manifest_index {
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	uint64_t offset;
} PACKED;

/*
 * Object record: this head, then by kind
 *
 *	file		struct fymf_file, and with the borrowed flag struct fymf_borrowed
 *	symlink		struct fymf_symlink and the target bytes
 *	directory	struct fymf_directory and the entries
 *
 * padded to a multiple of 8 bytes.
 */
struct fymf_record {
	uint8_t kind;
	uint8_t flags;
	uint16_t reserved;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	int64_t mtime_sec;
	uint32_t mtime_nsec;
	uint32_t reserved2;
} PACKED;

struct fymf_file {
	uint64_t size;
	unsigned char digest[FYAI_CAS_HASH_SIZE];
} PACKED;

struct fymf_borrowed {
	uint64_t device;
	uint64_t inode;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t reserved;
} PACKED;

struct fymf_symlink {
	uint32_t length;
	uint32_t reserved;
} PACKED;

struct fymf_directory {
	uint32_t count;
	uint32_t reserved;
} PACKED;

/* Entry: this head and the name bytes, padded to a multiple of 8 bytes. */
struct fymf_entry {
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	uint32_t name_length;
	uint32_t kind;
} PACKED;

/* Attribute record: this head and the path bytes, padded to a multiple of 8. */
struct fymf_attribute {
	uint32_t length;
	uint32_t nsec;
	int64_t sec;
} PACKED;

#define FYMF_HEADER_SIZE ((uint16_t)sizeof(struct fymf_header))
#define RECORD_FLAG_BORROWED 0x01
#define ALIGN8(n) (((n) + 7) & ~(size_t)7)

_Static_assert(sizeof(struct fymf_header) == 208, "header size");
_Static_assert(offsetof(struct fymf_header, object_count) == 16, "header alignment");
_Static_assert(offsetof(struct fymf_header, root) == 144, "header alignment");
_Static_assert(offsetof(struct fymf_record, mode) == 4, "record alignment");
_Static_assert(offsetof(struct fymf_record, mtime_sec) == 16, "record alignment");
_Static_assert(offsetof(struct fymf_record, mtime_nsec) == 24, "record alignment");
_Static_assert(offsetof(struct fymf_file, digest) == 8, "file alignment");
_Static_assert(offsetof(struct fymf_borrowed, mode) == 16, "borrowed alignment");
_Static_assert(offsetof(struct fymf_entry, name_length) == 32, "entry alignment");
_Static_assert(offsetof(struct fymf_attribute, sec) == 8, "attribute alignment");
_Static_assert(sizeof(struct fyai_manifest_index) == 40, "index size");
_Static_assert(sizeof(struct fymf_record) == 32, "record size");
_Static_assert(sizeof(struct fymf_file) == 40, "file size");
_Static_assert(sizeof(struct fymf_borrowed) == 32, "borrowed size");
_Static_assert(sizeof(struct fymf_entry) == 40, "entry size");
_Static_assert(sizeof(struct fymf_attribute) == 16, "attribute size");

/* Order of names and paths: unsigned bytes, a prefix first. */
static int bytes_compare(const unsigned char *a, size_t a_length, const unsigned char *b,
			 size_t b_length)
{
	int rc = memcmp(a, b, a_length < b_length ? a_length : b_length);

	if (rc)
		return rc;
	return (a_length > b_length) - (a_length < b_length);
}

size_t fyai_manifest_record_size(const struct fyai_mobject *object,
				 const struct fyai_project_entry *entries)
{
	size_t size = sizeof(struct fymf_record), i;

	switch (object->kind) {
	case FYAI_PROJECT_FILE:
		size += sizeof(struct fymf_file);
		if (object->blob.borrowed)
			size += sizeof(struct fymf_borrowed);
		break;
	case FYAI_PROJECT_SYMLINK:
		size += sizeof(struct fymf_symlink) + ALIGN8(object->target_length);
		break;
	case FYAI_PROJECT_DIRECTORY:
		size += sizeof(struct fymf_directory);
		for (i = 0; i < object->entry_count; i++)
			size += sizeof(struct fymf_entry) + ALIGN8(entries[i].name_length);
		break;
	default:
		return 0;
	}
	return size;
}

void fyai_manifest_record_put(unsigned char *p, const struct fyai_mobject *object,
			      const struct fyai_project_entry *entries)
{
	struct fymf_record *record = (struct fymf_record *)p;
	struct fymf_file *file;
	struct fymf_borrowed *borrowed;
	struct fymf_symlink *link;
	struct fymf_directory *directory;
	struct fymf_entry *entry;
	size_t i;

	memset(p, 0, fyai_manifest_record_size(object, entries));
	record->kind = (uint8_t)object->kind;
	record->flags = object->kind == FYAI_PROJECT_FILE && object->blob.borrowed ?
				RECORD_FLAG_BORROWED : 0;
	record->mode = htole32(object->meta.mode);
	record->uid = htole32(object->meta.uid);
	record->gid = htole32(object->meta.gid);
	record->mtime_sec = (int64_t)htole64((uint64_t)object->meta.mtime_sec);
	record->mtime_nsec = htole32(object->meta.mtime_nsec);
	p += sizeof(*record);
	switch (object->kind) {
	case FYAI_PROJECT_FILE:
		file = (struct fymf_file *)p;
		file->size = htole64(object->blob.size);
		memcpy(file->digest, object->blob.digest, FYAI_CAS_HASH_SIZE);
		if (object->blob.borrowed) {
			borrowed = (struct fymf_borrowed *)(p + sizeof(*file));
			borrowed->device = htole64(object->blob.source_device);
			borrowed->inode = htole64(object->blob.source_inode);
			borrowed->mode = htole32(object->blob.source_mode);
			borrowed->uid = htole32(object->blob.source_uid);
			borrowed->gid = htole32(object->blob.source_gid);
		}
		break;
	case FYAI_PROJECT_SYMLINK:
		link = (struct fymf_symlink *)p;
		link->length = htole32((uint32_t)object->target_length);
		memcpy(p + sizeof(*link), object->target, object->target_length);
		break;
	case FYAI_PROJECT_DIRECTORY:
		directory = (struct fymf_directory *)p;
		directory->count = htole32(object->entry_count);
		p += sizeof(*directory);
		for (i = 0; i < object->entry_count; i++) {
			entry = (struct fymf_entry *)p;
			memcpy(entry->digest, entries[i].digest, FYAI_CAS_HASH_SIZE);
			entry->name_length = htole32((uint32_t)entries[i].name_length);
			entry->kind = htole32((uint32_t)entries[i].kind);
			memcpy(p + sizeof(*entry), entries[i].name, entries[i].name_length);
			p += sizeof(*entry) + ALIGN8(entries[i].name_length);
		}
		break;
	default:
		break;
	}
}

/* Decode the record at p, which is aligned and ends before end. */
static int record_get(const unsigned char *p, const unsigned char *end, struct fyai_mobject *object,
		      size_t *length)
{
	const struct fymf_record *record = (const struct fymf_record *)p;
	const struct fymf_file *file;
	const struct fymf_borrowed *borrowed;
	const struct fymf_symlink *link;
	const struct fymf_directory *directory;
	const struct fymf_entry *entry;
	const unsigned char *start = p, *entries;
	uint32_t count, i;
	size_t name_length;

	if ((size_t)(end - p) < sizeof(*record))
		goto damaged;
	memset(object, 0, sizeof(*object));
	object->kind = record->kind;
	object->meta.mode = le32toh(record->mode);
	object->meta.uid = le32toh(record->uid);
	object->meta.gid = le32toh(record->gid);
	object->meta.mtime_sec = (int64_t)le64toh((uint64_t)record->mtime_sec);
	object->meta.mtime_nsec = le32toh(record->mtime_nsec);
	if (object->meta.mtime_nsec >= 1000000000 || object->meta.mode > 07777)
		goto damaged;
	if ((record->flags & ~RECORD_FLAG_BORROWED) ||
	    (record->flags && record->kind != FYAI_PROJECT_FILE))
		goto damaged;
	p += sizeof(*record);
	switch (object->kind) {
	case FYAI_PROJECT_FILE:
		if ((size_t)(end - p) < sizeof(*file))
			goto damaged;
		file = (const struct fymf_file *)p;
		object->blob.size = le64toh(file->size);
		memcpy(object->blob.digest, file->digest, FYAI_CAS_HASH_SIZE);
		p += sizeof(*file);
		if (record->flags & RECORD_FLAG_BORROWED) {
			if ((size_t)(end - p) < sizeof(*borrowed))
				goto damaged;
			borrowed = (const struct fymf_borrowed *)p;
			object->blob.borrowed = true;
			object->blob.source_device = le64toh(borrowed->device);
			object->blob.source_inode = le64toh(borrowed->inode);
			object->blob.source_mode = le32toh(borrowed->mode);
			object->blob.source_uid = le32toh(borrowed->uid);
			object->blob.source_gid = le32toh(borrowed->gid);
			p += sizeof(*borrowed);
		}
		break;
	case FYAI_PROJECT_SYMLINK:
		if ((size_t)(end - p) < sizeof(*link))
			goto damaged;
		link = (const struct fymf_symlink *)p;
		object->target_length = le32toh(link->length);
		p += sizeof(*link);
		if ((size_t)(end - p) < ALIGN8(object->target_length))
			goto damaged;
		object->target = p;
		p += ALIGN8(object->target_length);
		break;
	case FYAI_PROJECT_DIRECTORY:
		if ((size_t)(end - p) < sizeof(*directory))
			goto damaged;
		directory = (const struct fymf_directory *)p;
		count = le32toh(directory->count);
		p += sizeof(*directory);
		entries = p;
		for (i = 0; i < count; i++) {
			if ((size_t)(end - p) < sizeof(*entry))
				goto damaged;
			entry = (const struct fymf_entry *)p;
			name_length = le32toh(entry->name_length);
			if ((size_t)(end - p) - sizeof(*entry) < ALIGN8(name_length))
				goto damaged;
			p += sizeof(*entry) + ALIGN8(name_length);
		}
		object->entries = entries;
		object->entries_length = (size_t)(p - entries);
		object->entry_count = count;
		break;
	default:
		goto damaged;
	}
	*length = (size_t)(p - start);
	return 0;
damaged:
	errno = EBADMSG;
	return -1;
}

int fyai_mdir_open(const struct fyai_mobject *object, struct fyai_mdir *directory)
{
	if (object->kind != FYAI_PROJECT_DIRECTORY) {
		errno = EBADMSG;
		return -1;
	}
	directory->position = object->entries;
	directory->end = object->entries + object->entries_length;
	directory->remaining = object->entry_count;
	return 0;
}

bool fyai_mdir_next(struct fyai_mdir *directory, struct fyai_project_entry *entry)
{
	const struct fymf_entry *stored;
	size_t name_length;

	if (!directory->remaining || (size_t)(directory->end - directory->position) < sizeof(*stored))
		return false;
	stored = (const struct fymf_entry *)directory->position;
	name_length = le32toh(stored->name_length);
	if ((size_t)(directory->end - directory->position) - sizeof(*stored) < ALIGN8(name_length))
		return false;
	entry->kind = le32toh(stored->kind);
	memcpy(entry->digest, stored->digest, FYAI_CAS_HASH_SIZE);
	entry->name_length = name_length;
	entry->name = directory->position + sizeof(*stored);
	directory->position += sizeof(*stored) + ALIGN8(name_length);
	directory->remaining--;
	return true;
}

/* The reader */

static bool region_valid(uint64_t offset, uint64_t size, uint64_t file_size)
{
	return offset <= file_size && size <= file_size - offset && !(offset & 7);
}

/*
 * Check the header and the bounds of the regions. A manifest is used as it is
 * mapped: there is no pass over the index or the records. An access checks the
 * offset that it follows, and a record is checked as it is decoded.
 */
static int manifest_adopt(struct fyai_manifest *manifest, const unsigned char *data, size_t size)
{
	const struct fymf_header *header = (const struct fymf_header *)data;
	struct fyai_manifest m = { 0 };

	if (size < sizeof(*header) || ((uintptr_t)data & 7)) {
		errno = EBADMSG;
		return -1;
	}
	if (le32toh(header->magic) != FYMF_MAGIC || le16toh(header->version) != FYAI_MANIFEST_VERSION ||
	    le16toh(header->header_size) != FYMF_HEADER_SIZE || le64toh(header->file_size) != size ||
	    (le32toh(header->flags) & ~FYMF_FLAG_DELTA))
		goto damaged;
	m.data = data;
	m.size = size;
	m.delta = le32toh(header->flags) & FYMF_FLAG_DELTA;
	m.object_count = le64toh(header->object_count);
	m.attribute_count = le64toh(header->attribute_count);
	m.object_total = le64toh(header->object_total);
	m.index_offset = le64toh(header->index_offset);
	m.objects_offset = le64toh(header->objects_offset);
	m.objects_size = le64toh(header->objects_size);
	m.attribute_index_offset = le64toh(header->attribute_index_offset);
	m.attributes_offset = le64toh(header->attributes_offset);
	m.attributes_size = le64toh(header->attributes_size);
	m.removed_offset = le64toh(header->removed_offset);
	m.removed_count = le64toh(header->removed_count);
	m.removed_attribute_index_offset = le64toh(header->removed_attribute_index_offset);
	m.removed_attribute_count = le64toh(header->removed_attribute_count);
	m.removed_attributes_offset = le64toh(header->removed_attributes_offset);
	m.removed_attributes_size = le64toh(header->removed_attributes_size);
	memcpy(m.root, header->root, sizeof(m.root));
	memcpy(m.base_digest, header->base, sizeof(m.base_digest));
	if (m.object_count > FYAI_PROJECT_MAX_NODES || m.attribute_count > FYAI_PROJECT_MAX_NODES ||
	    m.removed_count > FYAI_PROJECT_MAX_NODES ||
	    m.removed_attribute_count > FYAI_PROJECT_MAX_NODES || m.object_total > FYAI_PROJECT_MAX_NODES)
		goto damaged;
	if (!m.delta && (m.removed_count || m.removed_attribute_count))
		goto damaged;
	if (!region_valid(m.index_offset, m.object_count * sizeof(struct fyai_manifest_index), size) ||
	    !region_valid(m.objects_offset, m.objects_size, size) ||
	    !region_valid(m.attribute_index_offset, m.attribute_count * sizeof(uint64_t), size) ||
	    !region_valid(m.attributes_offset, m.attributes_size, size) ||
	    !region_valid(m.removed_offset, m.removed_count * FYAI_CAS_HASH_SIZE, size) ||
	    !region_valid(m.removed_attribute_index_offset,
			  m.removed_attribute_count * sizeof(uint64_t), size) ||
	    !region_valid(m.removed_attributes_offset, m.removed_attributes_size, size))
		goto damaged;
	*manifest = m;
	return 0;
damaged:
	errno = EBADMSG;
	return -1;
}

int fyai_manifest_open_at(struct fyai_manifest *manifest, int directory_fd, const char *name)
{
	struct fyai_manifest *base;
	char base_name[FYAI_MANIFEST_NAME_SIZE];
	struct stat st;
	void *map;
	int fd, rc, saved;

	fd = openat(directory_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = fstat(fd, &st);
	if (rc || !S_ISREG(st.st_mode) || st.st_size < (off_t)FYMF_HEADER_SIZE) {
		saved = rc ? errno : EBADMSG;
		close(fd);
		errno = saved;
		return -1;
	}
	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	saved = errno;
	close(fd);
	if (map == MAP_FAILED) {
		errno = saved;
		return -1;
	}
	rc = manifest_adopt(manifest, map, (size_t)st.st_size);
	if (rc) {
		saved = errno;
		munmap(map, (size_t)st.st_size);
		errno = saved;
		return -1;
	}
	manifest->mapped = true;
	if (manifest->delta) {
		base = calloc(1, sizeof(*base));
		if (!base) {
			fyai_manifest_close(manifest);
			errno = ENOMEM;
			return -1;
		}
		fyai_cas_hex(base_name, manifest->base_digest, FYAI_CAS_HASH_SIZE);
		rc = fyai_manifest_open_at(base, directory_fd, base_name);
		if (rc || base->delta) {
			saved = rc ? errno : EBADMSG;
			fyai_manifest_close(base);
			free(base);
			fyai_manifest_close(manifest);
			errno = saved;
			return -1;
		}
		manifest->base = base;
	}
	return 0;
}

void fyai_manifest_close(struct fyai_manifest *manifest)
{
	if (manifest->base) {
		fyai_manifest_close(manifest->base);
		free(manifest->base);
	}
	if (manifest->mapped && manifest->data)
		munmap((void *)manifest->data, manifest->size);
	else if (manifest->owned)
		free((void *)manifest->data);
	memset(manifest, 0, sizeof(*manifest));
}

const unsigned char *fyai_manifest_root(const struct fyai_manifest *manifest)
{
	return manifest->root;
}

static const struct fyai_manifest_index *index_at(const struct fyai_manifest *manifest,
						  uint64_t position)
{
	return (const struct fyai_manifest_index *)(manifest->data + manifest->index_offset) +
	       position;
}

/* The digests that a delta removes from its base, in byte order. */
static bool digest_removed(const struct fyai_manifest *manifest,
			   const unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	const unsigned char *removed = manifest->data + manifest->removed_offset;
	uint64_t low = 0, high = manifest->removed_count, middle;
	int rc;

	while (low < high) {
		middle = low + (high - low) / 2;
		rc = memcmp(digest, removed + middle * FYAI_CAS_HASH_SIZE, FYAI_CAS_HASH_SIZE);
		if (rc < 0)
			high = middle;
		else if (rc > 0)
			low = middle + 1;
		else
			return true;
	}
	return false;
}

/* A record in the own index of a manifest; a delta does not look in its base. */
static const unsigned char *find_own_record(const struct fyai_manifest *manifest,
					    const unsigned char digest[FYAI_CAS_HASH_SIZE],
					    const unsigned char **end)
{
	uint64_t low = 0, high = manifest->object_count, middle, offset;
	const struct fyai_manifest_index *entry;
	int rc;

	while (low < high) {
		middle = low + (high - low) / 2;
		entry = index_at(manifest, middle);
		rc = memcmp(digest, entry->digest, FYAI_CAS_HASH_SIZE);
		if (rc < 0) {
			high = middle;
		} else if (rc > 0) {
			low = middle + 1;
		} else {
			offset = le64toh(entry->offset);
			if (offset >= manifest->objects_size || (offset & 7))
				return NULL;
			*end = manifest->data + manifest->objects_offset + manifest->objects_size;
			return manifest->data + manifest->objects_offset + offset;
		}
	}
	return NULL;
}

static const unsigned char *find_record(const struct fyai_manifest *manifest,
					const unsigned char digest[FYAI_CAS_HASH_SIZE],
					const unsigned char **end)
{
	const unsigned char *record = find_own_record(manifest, digest, end);

	/* A delta falls back to its base, except for the objects that it removed. */
	if (!record && manifest->base && !digest_removed(manifest, digest))
		return find_record(manifest->base, digest, end);
	return record;
}

bool fyai_manifest_find(const struct fyai_manifest *manifest,
			const unsigned char digest[FYAI_CAS_HASH_SIZE], struct fyai_mobject *object)
{
	const unsigned char *record, *end;
	size_t length;

	record = find_record(manifest, digest, &end);
	if (!record)
		return false;
	return !record_get(record, end, object, &length);
}

const unsigned char *fyai_manifest_record(const struct fyai_manifest *manifest,
					  const unsigned char digest[FYAI_CAS_HASH_SIZE],
					  size_t *length)
{
	const unsigned char *record, *end;
	struct fyai_mobject object;

	record = find_record(manifest, digest, &end);
	if (!record || record_get(record, end, &object, length))
		return NULL;
	return record;
}

bool fyai_manifest_object_at(const struct fyai_manifest *manifest, size_t position,
			     unsigned char digest[FYAI_CAS_HASH_SIZE], struct fyai_mobject *object)
{
	const struct fyai_manifest_index *entry;
	const unsigned char *record, *end;
	size_t length;

	uint64_t offset;

	if (position >= manifest->object_count)
		return false;
	entry = index_at(manifest, position);
	offset = le64toh(entry->offset);
	if (offset >= manifest->objects_size || (offset & 7))
		return false;
	memcpy(digest, entry->digest, FYAI_CAS_HASH_SIZE);
	record = manifest->data + manifest->objects_offset + offset;
	end = manifest->data + manifest->objects_offset + manifest->objects_size;
	return !record_get(record, end, object, &length);
}

bool fyai_manifest_lookup(const struct fyai_manifest *manifest, const unsigned char *path,
			  size_t length, struct fyai_mobject *object,
			  unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	struct fyai_mobject current;
	struct fyai_mdir directory;
	struct fyai_project_entry entry;
	unsigned char cursor[FYAI_CAS_HASH_SIZE];
	const unsigned char *end, *part = path, *stop = path + length;
	size_t name_length;
	unsigned int depth = 0;
	bool found;

	memcpy(cursor, manifest->root, sizeof(cursor));
	if (!fyai_manifest_find(manifest, cursor, &current))
		return false;
	while (part < stop) {
		if (++depth > FYAI_PROJECT_MAX_DEPTH || fyai_mdir_open(&current, &directory))
			return false;
		end = memchr(part, '/', (size_t)(stop - part));
		if (!end)
			end = stop;
		name_length = (size_t)(end - part);
		found = false;
		while (fyai_mdir_next(&directory, &entry)) {
			if (!bytes_compare(entry.name, entry.name_length, part, name_length)) {
				found = true;
				break;
			}
		}
		if (!found)
			return false;
		memcpy(cursor, entry.digest, sizeof(cursor));
		if (!fyai_manifest_find(manifest, cursor, &current))
			return false;
		part = end < stop ? end + 1 : end;
	}
	if (object)
		*object = current;
	if (digest)
		memcpy(digest, cursor, FYAI_CAS_HASH_SIZE);
	return true;
}

/* A table of paths: an index of offsets into the records, in path order. */
struct path_table {
	const unsigned char *data;
	uint64_t index_offset, count, records_offset, records_size;
};

static struct path_table attribute_table(const struct fyai_manifest *manifest)
{
	return (struct path_table){ manifest->data, manifest->attribute_index_offset,
				    manifest->attribute_count, manifest->attributes_offset,
				    manifest->attributes_size };
}

static struct path_table removed_attribute_table(const struct fyai_manifest *manifest)
{
	return (struct path_table){ manifest->data, manifest->removed_attribute_index_offset,
				    manifest->removed_attribute_count,
				    manifest->removed_attributes_offset,
				    manifest->removed_attributes_size };
}

static const struct fymf_attribute *table_record(const struct path_table *table, uint64_t position)
{
	const uint64_t *index = (const uint64_t *)(table->data + table->index_offset);
	const unsigned char *record, *end;
	const struct fymf_attribute *attribute;
	uint64_t offset = le64toh(index[position]);

	if (offset >= table->records_size || (offset & 7))
		return NULL;
	record = table->data + table->records_offset + offset;
	end = table->data + table->records_offset + table->records_size;
	if ((size_t)(end - record) < sizeof(*attribute))
		return NULL;
	attribute = (const struct fymf_attribute *)record;
	if ((size_t)(end - record) - sizeof(*attribute) < ALIGN8(le32toh(attribute->length)))
		return NULL;
	return attribute;
}

static const struct fymf_attribute *table_find(const struct path_table *table,
					       const unsigned char *path, size_t length)
{
	uint64_t low = 0, high = table->count, middle;
	const struct fymf_attribute *attribute;
	int rc;

	while (low < high) {
		middle = low + (high - low) / 2;
		attribute = table_record(table, middle);
		if (!attribute)
			return NULL;
		rc = bytes_compare(path, length, (const unsigned char *)attribute + sizeof(*attribute),
				   le32toh(attribute->length));
		if (rc < 0)
			high = middle;
		else if (rc > 0)
			low = middle + 1;
		else
			return attribute;
	}
	return NULL;
}

bool fyai_manifest_attribute_at(const struct fyai_manifest *manifest, size_t position,
				const unsigned char **path, size_t *length, int64_t *sec,
				uint32_t *nsec)
{
	struct path_table table = attribute_table(manifest);
	const struct fymf_attribute *attribute;

	if (position >= manifest->attribute_count)
		return false;
	attribute = table_record(&table, position);
	if (!attribute)
		return false;
	*length = le32toh(attribute->length);
	*sec = (int64_t)le64toh((uint64_t)attribute->sec);
	*nsec = le32toh(attribute->nsec);
	*path = (const unsigned char *)attribute + sizeof(*attribute);
	return true;
}

bool fyai_manifest_attribute(const struct fyai_manifest *manifest, const unsigned char *path,
			     size_t length, int64_t *sec, uint32_t *nsec)
{
	struct path_table table = attribute_table(manifest), removed;
	const struct fymf_attribute *attribute;

	attribute = table_find(&table, path, length);
	if (attribute) {
		*sec = (int64_t)le64toh((uint64_t)attribute->sec);
		*nsec = le32toh(attribute->nsec);
		return true;
	}
	if (!manifest->base)
		return false;
	removed = removed_attribute_table(manifest);
	if (table_find(&removed, path, length))
		return false;
	return fyai_manifest_attribute(manifest->base, path, length, sec, nsec);
}

/* Publication of a file */

int fyai_manifest_write(const struct fyai_manifest *manifest, int directory_fd, bool durable,
			char name[FYAI_MANIFEST_NAME_SIZE])
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	const uint8_t *digest;
	unsigned char random[16];
	char temporary[sizeof(".tmp-") + sizeof(random) * 2];
	struct stat existing;
	size_t done = 0;
	ssize_t amount;
	int fd, rc, saved;

	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		return -1;
	}
	digest = fy_blake3_hash(hasher, manifest->data, manifest->size);
	if (!digest) {
		fy_blake3_hasher_destroy(hasher);
		errno = EIO;
		return -1;
	}
	fyai_cas_hex(name, digest, FY_BLAKE3_OUT_LEN);
	fy_blake3_hasher_destroy(hasher);
	/* The name is the digest of the bytes: a file of that name and size is the manifest. */
	if (!fstatat(directory_fd, name, &existing, AT_SYMLINK_NOFOLLOW) && S_ISREG(existing.st_mode) &&
	    (size_t)existing.st_size == manifest->size)
		return 0;
	/*
	 * Write to a temporary file of a random name and rename it to the digest
	 * name: the name is unique per writer, and a reader never sees a partial
	 * manifest.
	 */
	if (RAND_bytes(random, sizeof(random)) != 1) {
		errno = EIO;
		return -1;
	}
	memcpy(temporary, ".tmp-", 5);
	fyai_cas_hex(temporary + 5, random, sizeof(random));
	fd = openat(directory_fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		    0600);
	if (fd < 0)
		return -1;
	rc = 0;
	while (done < manifest->size) {
		amount = write(fd, manifest->data + done, manifest->size - done);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0) {
			rc = -1;
			break;
		}
		done += (size_t)amount;
	}
	if (!rc)
		rc = fchmod(fd, 0444);
	if (!rc && durable)
		rc = fsync(fd);
	saved = errno;
	if (close(fd) < 0 && !rc) {
		rc = -1;
		saved = errno;
	}
	if (!rc)
		rc = renameat(directory_fd, temporary, directory_fd, name);
	if (rc) {
		saved = rc && saved ? saved : errno;
		unlinkat(directory_fd, temporary, 0);
		errno = saved;
		return -1;
	}
	if (durable && fsync(directory_fd))
		return -1;
	return 0;
}

/* The builder */

#define BUILDER_MIN_RECORDS 4096
#define BUILDER_MIN_INDEX 256

int fyai_manifest_builder_init(struct fyai_manifest_builder *builder, struct fyai_scratch *scratch)
{
	memset(builder, 0, sizeof(*builder));
	builder->scratch = scratch;
	builder->slot_mask = 1023;
	builder->slots = fyai_scratch_calloc(scratch, builder->slot_mask + 1, sizeof(*builder->slots));
	return builder->slots ? 0 : -1;
}

static size_t digest_slot(const unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	uint64_t value;

	memcpy(&value, digest, sizeof(value));
	return (size_t)value;
}

static bool builder_find(const struct fyai_manifest_builder *builder,
			 const unsigned char digest[FYAI_CAS_HASH_SIZE], size_t *slot_out)
{
	size_t slot = digest_slot(digest) & builder->slot_mask;

	while (builder->slots[slot]) {
		if (!memcmp(builder->index[builder->slots[slot] - 1].digest, digest,
			    FYAI_CAS_HASH_SIZE)) {
			*slot_out = slot;
			return true;
		}
		slot = (slot + 1) & builder->slot_mask;
	}
	*slot_out = slot;
	return false;
}

bool fyai_manifest_builder_has(const struct fyai_manifest_builder *builder,
			       const unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	size_t slot;

	return builder_find(builder, digest, &slot);
}

static int builder_grow_slots(struct fyai_manifest_builder *builder)
{
	uint32_t *slots;
	size_t mask = (builder->slot_mask + 1) * 2 - 1, i, slot;

	slots = fyai_scratch_calloc(builder->scratch, mask + 1, sizeof(*slots));
	if (!slots)
		return -1;
	for (i = 0; i < builder->count; i++) {
		slot = digest_slot(builder->index[i].digest) & mask;
		while (slots[slot])
			slot = (slot + 1) & mask;
		slots[slot] = (uint32_t)(i + 1);
	}
	builder->slots = slots;
	builder->slot_mask = mask;
	return 0;
}

static unsigned char *builder_reserve(struct fyai_manifest_builder *builder, size_t length)
{
	size_t capacity;
	unsigned char *records;

	if (builder->records_size + length > builder->records_capacity) {
		capacity = builder->records_capacity ? builder->records_capacity * 2 :
						       BUILDER_MIN_RECORDS;
		while (capacity < builder->records_size + length)
			capacity *= 2;
		records = fyai_scratch_grow(builder->scratch, builder->records, builder->records_size,
					    capacity, 1);
		if (!records)
			return NULL;
		builder->records = records;
		builder->records_capacity = capacity;
	}
	return builder->records + builder->records_size;
}

/* Reserve length bytes for a record and enter the digest in the index. */
static unsigned char *builder_begin(struct fyai_manifest_builder *builder,
				    const unsigned char digest[FYAI_CAS_HASH_SIZE], size_t length,
				    bool *present)
{
	struct fyai_manifest_index *index;
	unsigned char *record;
	size_t slot, capacity;

	if (builder_find(builder, digest, &slot)) {
		*present = true;
		return NULL;
	}
	*present = false;
	if (builder->count >= FYAI_PROJECT_MAX_NODES) {
		errno = E2BIG;
		return NULL;
	}
	if ((builder->count + 1) * 2 > builder->slot_mask + 1) {
		if (builder_grow_slots(builder) || builder_find(builder, digest, &slot))
			return NULL;
	}
	if (builder->count == builder->capacity) {
		capacity = builder->capacity ? builder->capacity * 2 : BUILDER_MIN_INDEX;
		index = fyai_scratch_grow(builder->scratch, builder->index, builder->count, capacity,
					  sizeof(*index));
		if (!index)
			return NULL;
		builder->index = index;
		builder->capacity = capacity;
	}
	record = builder_reserve(builder, length);
	if (!record)
		return NULL;
	memcpy(builder->index[builder->count].digest, digest, FYAI_CAS_HASH_SIZE);
	builder->index[builder->count].offset = builder->records_size;
	builder->count++;
	builder->slots[slot] = (uint32_t)builder->count;
	builder->records_size += length;
	return record;
}

int fyai_manifest_builder_add(struct fyai_manifest_builder *builder,
			      const unsigned char digest[FYAI_CAS_HASH_SIZE],
			      const struct fyai_mobject *object,
			      const struct fyai_project_entry *entries)
{
	unsigned char *record;
	size_t length;
	bool present;

	if (object->kind == FYAI_PROJECT_DIRECTORY && object->entry_count && !entries) {
		errno = EINVAL;
		return -1;
	}
	length = fyai_manifest_record_size(object, entries);
	if (!length) {
		errno = EINVAL;
		return -1;
	}
	record = builder_begin(builder, digest, length, &present);
	if (present)
		return 0;
	if (!record)
		return -1;
	fyai_manifest_record_put(record, object, entries);
	return 0;
}

int fyai_manifest_builder_add_record(struct fyai_manifest_builder *builder,
				     const unsigned char digest[FYAI_CAS_HASH_SIZE],
				     const unsigned char *record, size_t length)
{
	unsigned char *destination;
	bool present;

	destination = builder_begin(builder, digest, length, &present);
	if (present)
		return 0;
	if (!destination)
		return -1;
	memcpy(destination, record, length);
	return 0;
}

int fyai_manifest_builder_attribute(struct fyai_manifest_builder *builder,
				    const unsigned char *path, size_t length, int64_t sec,
				    uint32_t nsec)
{
	struct fyai_manifest_attribute *attributes;
	size_t capacity;

	if (builder->attribute_count == builder->attribute_capacity) {
		capacity = builder->attribute_capacity ? builder->attribute_capacity * 2 : 64;
		attributes = fyai_scratch_grow(builder->scratch, builder->attributes,
					       builder->attribute_count, capacity,
					       sizeof(*attributes));
		if (!attributes)
			return -1;
		builder->attributes = attributes;
		builder->attribute_capacity = capacity;
	}
	builder->attributes[builder->attribute_count++] = (struct fyai_manifest_attribute){
		.path = path, .length = length, .sec = sec, .nsec = nsec
	};
	return 0;
}

static int index_compare(const void *a, const void *b)
{
	return memcmp(a, b, FYAI_CAS_HASH_SIZE);
}

static int attribute_compare(const void *a, const void *b)
{
	const struct fyai_manifest_attribute *left = a, *right = b;

	return bytes_compare(left->path, left->length, right->path, right->length);
}

struct image_layout {
	bool delta;
	uint64_t object_count, attribute_count, object_total, objects_size;
	uint64_t attribute_index_offset, attributes_offset, attributes_size;
	uint64_t removed_offset, removed_count;
	uint64_t removed_attribute_index_offset, removed_attribute_count;
	uint64_t removed_attributes_offset, removed_attributes_size;
	uint64_t file_size;
	const unsigned char *root, *base;
};

/* Fill the header of an image. */
static void header_put(unsigned char *image, const struct image_layout *l)
{
	struct fymf_header *header = (struct fymf_header *)image;

	memset(header, 0, sizeof(*header));
	header->magic = htole32(FYMF_MAGIC);
	header->version = htole16(FYAI_MANIFEST_VERSION);
	header->header_size = htole16(FYMF_HEADER_SIZE);
	header->flags = htole32(l->delta ? FYMF_FLAG_DELTA : 0);
	header->object_count = htole64(l->object_count);
	header->attribute_count = htole64(l->attribute_count);
	header->object_total = htole64(l->object_total);
	header->index_offset = htole64(FYMF_HEADER_SIZE);
	header->objects_offset = htole64(FYMF_HEADER_SIZE +
					 l->object_count * sizeof(struct fyai_manifest_index));
	header->objects_size = htole64(l->objects_size);
	header->attribute_index_offset = htole64(l->attribute_index_offset);
	header->attributes_offset = htole64(l->attributes_offset);
	header->attributes_size = htole64(l->attributes_size);
	header->removed_offset = htole64(l->removed_offset);
	header->removed_count = htole64(l->removed_count);
	header->removed_attribute_index_offset = htole64(l->removed_attribute_index_offset);
	header->removed_attribute_count = htole64(l->removed_attribute_count);
	header->removed_attributes_offset = htole64(l->removed_attributes_offset);
	header->removed_attributes_size = htole64(l->removed_attributes_size);
	header->file_size = htole64(l->file_size);
	memcpy(header->root, l->root, FYAI_CAS_HASH_SIZE);
	if (l->base)
		memcpy(header->base, l->base, FYAI_CAS_HASH_SIZE);
}

static int removed_compare(const void *a, const void *b)
{
	return memcmp(a, b, FYAI_CAS_HASH_SIZE);
}

int fyai_manifest_builder_remove(struct fyai_manifest_builder *builder,
				 const unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	unsigned char (*removed)[FYAI_CAS_HASH_SIZE];
	size_t capacity;

	if (builder->removed_count == builder->removed_capacity) {
		capacity = builder->removed_capacity ? builder->removed_capacity * 2 : 64;
		removed = fyai_scratch_grow(builder->scratch, builder->removed, builder->removed_count,
					    capacity, sizeof(*removed));
		if (!removed)
			return -1;
		builder->removed = removed;
		builder->removed_capacity = capacity;
	}
	memcpy(builder->removed[builder->removed_count++], digest, FYAI_CAS_HASH_SIZE);
	return 0;
}

int fyai_manifest_builder_remove_attribute(struct fyai_manifest_builder *builder,
					   const unsigned char *path, size_t length)
{
	struct fyai_manifest_attribute *removed;
	size_t capacity;

	if (builder->removed_attribute_count == builder->removed_attribute_capacity) {
		capacity = builder->removed_attribute_capacity ?
				   builder->removed_attribute_capacity * 2 : 64;
		removed = fyai_scratch_grow(builder->scratch, builder->removed_attributes,
					    builder->removed_attribute_count, capacity,
					    sizeof(*removed));
		if (!removed)
			return -1;
		builder->removed_attributes = removed;
		builder->removed_attribute_capacity = capacity;
	}
	builder->removed_attributes[builder->removed_attribute_count++] =
		(struct fyai_manifest_attribute){ .path = path, .length = length };
	return 0;
}

/* The size of the records of a table of paths. */
static size_t table_size(const struct fyai_manifest_attribute *paths, size_t count)
{
	size_t i, size = 0;

	for (i = 0; i < count; i++)
		size += sizeof(struct fymf_attribute) + ALIGN8(paths[i].length);
	return size;
}

/* Write the index and the records of a table of paths at the given offsets. */
static void table_put(unsigned char *image, uint64_t index_offset, uint64_t records_offset,
		      const struct fyai_manifest_attribute *paths, size_t count)
{
	uint64_t *index = (uint64_t *)(image + index_offset);
	struct fymf_attribute *record = (struct fymf_attribute *)(image + records_offset);
	size_t i;

	for (i = 0; i < count; i++) {
		index[i] = htole64((uint64_t)((unsigned char *)record - (image + records_offset)));
		memset(record, 0, sizeof(*record) + ALIGN8(paths[i].length));
		record->length = htole32((uint32_t)paths[i].length);
		record->nsec = htole32(paths[i].nsec);
		record->sec = (int64_t)htole64((uint64_t)paths[i].sec);
		memcpy((unsigned char *)record + sizeof(*record), paths[i].path, paths[i].length);
		record = (struct fymf_attribute *)((unsigned char *)record + sizeof(*record) +
						   ALIGN8(paths[i].length));
	}
}

/* Sort and drop the repeats; return the count that remains. */
static size_t paths_unique(struct fyai_manifest_attribute *paths, size_t count)
{
	size_t i, kept = 0;

	if (!count)
		return 0;
	qsort(paths, count, sizeof(*paths), attribute_compare);
	for (i = 0; i < count; i++) {
		if (kept && !attribute_compare(&paths[kept - 1], &paths[i]))
			continue;
		paths[kept++] = paths[i];
	}
	return kept;
}

static int builder_finish(struct fyai_manifest_builder *builder,
			  const unsigned char root[FYAI_CAS_HASH_SIZE],
			  const unsigned char *base_digest, uint64_t object_total,
			  struct fyai_manifest *manifest)
{
	struct image_layout layout = { .delta = base_digest != NULL, .root = root,
				       .base = base_digest };
	struct fyai_manifest staged, final;
	struct fyai_manifest_index *sorted_index, *stored;
	struct fyai_manifest_attribute *kept;
	unsigned char *image, *grown;
	size_t i, objects_offset, objects_end, kept_count = 0, removed_count, removed_paths;
	size_t total;

	if (!builder->count || (!layout.delta && !fyai_manifest_builder_has(builder, root))) {
		errno = EINVAL;
		return -1;
	}
	sorted_index = fyai_scratch_alloc(builder->scratch, builder->count * sizeof(*sorted_index));
	if (!sorted_index)
		return -1;
	memcpy(sorted_index, builder->index, builder->count * sizeof(*sorted_index));
	qsort(sorted_index, builder->count, sizeof(*sorted_index), index_compare);
	/* The image: header, index and records; the other tables follow later. */
	objects_offset = FYMF_HEADER_SIZE + builder->count * sizeof(*sorted_index);
	objects_end = objects_offset + builder->records_size;
	image = malloc(objects_end);
	if (!image) {
		errno = ENOMEM;
		return -1;
	}
	layout.object_count = builder->count;
	layout.object_total = object_total;
	layout.objects_size = builder->records_size;
	layout.attribute_index_offset = layout.attributes_offset = layout.removed_offset =
		layout.removed_attribute_index_offset = layout.removed_attributes_offset =
			layout.file_size = objects_end;
	header_put(image, &layout);
	stored = (struct fyai_manifest_index *)(image + FYMF_HEADER_SIZE);
	for (i = 0; i < builder->count; i++) {
		memcpy(stored[i].digest, sorted_index[i].digest, FYAI_CAS_HASH_SIZE);
		stored[i].offset = htole64(sorted_index[i].offset);
	}
	memcpy(image + objects_offset, builder->records, builder->records_size);
	/*
	 * A full manifest keeps the attributes of paths that exist in the tree; a
	 * delta keeps its attributes, which the caller made exact.
	 */
	kept = builder->attributes;
	if (!layout.delta) {
		kept = fyai_scratch_alloc(builder->scratch,
					  (builder->attribute_count + 1) * sizeof(*kept));
		if (!kept || manifest_adopt(&staged, image, objects_end)) {
			free(image);
			return -1;
		}
		if (builder->attribute_count)
			qsort(builder->attributes, builder->attribute_count,
			      sizeof(*builder->attributes), attribute_compare);
		for (i = 0; i < builder->attribute_count; i++) {
			if (!fyai_manifest_lookup(&staged, builder->attributes[i].path,
						  builder->attributes[i].length, NULL, NULL))
				continue;
			if (kept_count && !attribute_compare(&kept[kept_count - 1],
							     &builder->attributes[i]))
				continue;
			kept[kept_count++] = builder->attributes[i];
		}
	} else {
		kept_count = paths_unique(builder->attributes, builder->attribute_count);
	}
	removed_count = 0;
	if (layout.delta && builder->removed_count) {
		qsort(builder->removed, builder->removed_count, sizeof(*builder->removed),
		      removed_compare);
		for (i = 0; i < builder->removed_count; i++) {
			if (removed_count && !memcmp(builder->removed[removed_count - 1],
						     builder->removed[i], FYAI_CAS_HASH_SIZE))
				continue;
			memcpy(builder->removed[removed_count++], builder->removed[i],
			       FYAI_CAS_HASH_SIZE);
		}
	}
	removed_paths = layout.delta ?
				paths_unique(builder->removed_attributes,
					     builder->removed_attribute_count) : 0;
	layout.attribute_count = kept_count;
	layout.attribute_index_offset = objects_end;
	layout.attributes_offset = layout.attribute_index_offset + kept_count * sizeof(uint64_t);
	layout.attributes_size = table_size(kept, kept_count);
	layout.removed_offset = layout.attributes_offset + layout.attributes_size;
	layout.removed_count = removed_count;
	layout.removed_attribute_index_offset = layout.removed_offset +
						removed_count * FYAI_CAS_HASH_SIZE;
	layout.removed_attribute_count = removed_paths;
	layout.removed_attributes_offset = layout.removed_attribute_index_offset +
					   removed_paths * sizeof(uint64_t);
	layout.removed_attributes_size = table_size(builder->removed_attributes, removed_paths);
	total = layout.removed_attributes_offset + layout.removed_attributes_size;
	layout.file_size = total;
	grown = realloc(image, total);
	if (!grown) {
		free(image);
		errno = ENOMEM;
		return -1;
	}
	image = grown;
	table_put(image, layout.attribute_index_offset, layout.attributes_offset, kept, kept_count);
	if (removed_count)
		memcpy(image + layout.removed_offset, builder->removed,
		       removed_count * FYAI_CAS_HASH_SIZE);
	table_put(image, layout.removed_attribute_index_offset, layout.removed_attributes_offset,
		  builder->removed_attributes, removed_paths);
	header_put(image, &layout);
	if (manifest_adopt(&final, image, total)) {
		free(image);
		return -1;
	}
	final.owned = true;
	*manifest = final;
	return 0;
}

int fyai_manifest_builder_finish(struct fyai_manifest_builder *builder,
				 const unsigned char root[FYAI_CAS_HASH_SIZE],
				 struct fyai_manifest *manifest)
{
	return builder_finish(builder, root, NULL, builder->count, manifest);
}

int fyai_manifest_builder_finish_delta(struct fyai_manifest_builder *builder,
				       const unsigned char root[FYAI_CAS_HASH_SIZE],
				       const unsigned char base_digest[FYAI_CAS_HASH_SIZE],
				       uint64_t object_total, struct fyai_manifest *manifest)
{
	return builder_finish(builder, root, base_digest, object_total, manifest);
}

/* The record at a position of the own index, and its length. */
static const unsigned char *index_record(const struct fyai_manifest *manifest, uint64_t position,
					 size_t *length)
{
	const struct fyai_manifest_index *entry = index_at(manifest, position);
	uint64_t offset = le64toh(entry->offset);
	struct fyai_mobject object;
	const unsigned char *record;

	if (offset >= manifest->objects_size || (offset & 7))
		return NULL;
	record = manifest->data + manifest->objects_offset + offset;
	if (record_get(record, manifest->data + manifest->objects_offset + manifest->objects_size,
		       &object, length))
		return NULL;
	return record;
}

/* The record of an object in a manifest, as stored, with its length. */
static const unsigned char *record_of(const struct fyai_manifest *manifest,
				      const unsigned char digest[FYAI_CAS_HASH_SIZE],
				      size_t *length)
{
	return fyai_manifest_record(manifest, digest, length);
}

int fyai_manifest_make_delta(const struct fyai_manifest *full, const struct fyai_manifest *base,
			     const char base_name[FYAI_MANIFEST_NAME_SIZE],
			     struct fyai_manifest *delta)
{
	struct fyai_scratch scratch = { 0 };
	struct fyai_manifest_builder builder;
	struct fyai_mobject object;
	unsigned char base_digest[FYAI_CAS_HASH_SIZE];
	const unsigned char *record, *other, *path;
	size_t i, j, length, other_length, path_length, count = 0;
	int64_t sec, other_sec;
	uint32_t nsec, other_nsec;
	int rc = -1, saved, order;

	if (full->delta || base->delta || fyai_cas_digest_parse(base_digest, base_name)) {
		errno = EINVAL;
		return -1;
	}
	if (fyai_scratch_open(&scratch))
		return -1;
	if (fyai_manifest_builder_init(&builder, &scratch))
		goto out;
	/*
	 * One walk over the two indexes, which are in digest order: a record that
	 * the base lacks or holds in another form is the delta's own, and a digest
	 * that only the base has is removed.
	 */
	i = j = 0;
	while (i < full->object_count || j < base->object_count) {
		order = i == full->object_count ? 1 : j == base->object_count ? -1 :
			memcmp(index_at(full, i)->digest, index_at(base, j)->digest,
			       FYAI_CAS_HASH_SIZE);
		if (order > 0) {
			if (fyai_manifest_builder_remove(&builder, index_at(base, j)->digest))
				goto out;
			j++;
			continue;
		}
		record = index_record(full, i, &length);
		if (!record) {
			errno = EBADMSG;
			goto out;
		}
		other = order ? NULL : index_record(base, j, &other_length);
		if (!other || other_length != length || memcmp(record, other, length)) {
			if (fyai_manifest_builder_add_record(&builder, index_at(full, i)->digest,
							     record, length))
				goto out;
			count++;
		}
		i++;
		if (!order)
			j++;
	}
	/* The path timestamps: the ones that differ, and the ones that went away. */
	for (i = 0; i < full->attribute_count; i++) {
		if (!fyai_manifest_attribute_at(full, i, &path, &path_length, &sec, &nsec)) {
			errno = EBADMSG;
			goto out;
		}
		if (fyai_manifest_attribute(base, path, path_length, &other_sec, &other_nsec) &&
		    other_sec == sec && other_nsec == nsec)
			continue;
		if (fyai_manifest_builder_attribute(&builder, path, path_length, sec, nsec))
			goto out;
	}
	for (i = 0; i < base->attribute_count; i++) {
		if (!fyai_manifest_attribute_at(base, i, &path, &path_length, &sec, &nsec)) {
			errno = EBADMSG;
			goto out;
		}
		if (!fyai_manifest_attribute(full, path, path_length, &other_sec, &other_nsec) &&
		    fyai_manifest_builder_remove_attribute(&builder, path, path_length))
			goto out;
	}
	/* A delta needs an object of its own; an unchanged tree keeps the root. */
	if (!count) {
		memset(&object, 0, sizeof(object));
		if (!fyai_manifest_find(full, full->root, &object)) {
			errno = EBADMSG;
			goto out;
		}
		record = record_of(full, full->root, &length);
		if (!record || fyai_manifest_builder_add_record(&builder, full->root, record, length))
			goto out;
	}
	rc = fyai_manifest_builder_finish_delta(&builder, full->root, base_digest,
						full->object_total, delta);
out:
	saved = errno;
	fyai_scratch_close(&scratch);
	errno = saved;
	return rc;
}

/* Conversion to and from the snapshot generic */

static fy_generic object_to_generic(struct fy_generic_builder *gb, const struct fyai_mobject *object,
				    struct fyai_project_entry *entries, char *digest)
{
	struct fyai_mdir directory;
	size_t i = 0;

	if (object->kind == FYAI_PROJECT_FILE)
		return fyai_project_file(gb, &object->meta, &object->blob, digest);
	if (object->kind == FYAI_PROJECT_SYMLINK)
		return fyai_project_symlink(gb, &object->meta, object->target,
					    object->target_length, digest);
	if (fyai_mdir_open(object, &directory))
		return fy_invalid;
	while (i < object->entry_count && fyai_mdir_next(&directory, &entries[i]))
		i++;
	if (i != object->entry_count) {
		errno = EBADMSG;
		return fy_invalid;
	}
	return fyai_project_directory(gb, &object->meta, entries, i, false, digest);
}

/*
 * The objects of a tree are the own objects of the manifest and, for a delta,
 * the objects of its base that it neither removed nor replaced.
 */
static bool object_visible(const struct fyai_manifest *scan, const struct fyai_manifest *top,
			   const unsigned char digest[FYAI_CAS_HASH_SIZE])
{
	const unsigned char *end;

	return scan == top || !(digest_removed(top, digest) || find_own_record(top, digest, &end));
}

/* The same for the path timestamps. */
static bool attribute_visible(const struct fyai_manifest *scan, const struct fyai_manifest *top,
			      const unsigned char *path, size_t length)
{
	struct path_table own = attribute_table(top), removed = removed_attribute_table(top);

	return scan == top || !(table_find(&removed, path, length) || table_find(&own, path, length));
}

fy_generic fyai_manifest_to_snapshot(struct fy_generic_builder *gb,
				     const struct fyai_manifest *manifest)
{
	const struct fyai_manifest *scans[2] = { manifest, manifest->base };
	struct fyai_mobject object;
	struct fyai_project_entry *entries = NULL;
	fy_generic *pairs = NULL, *attrs = NULL, objects, attributes, result = fy_invalid;
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	const unsigned char *path;
	char hex[FYAI_CAS_DIGEST_SIZE], root[FYAI_CAS_DIGEST_SIZE], *encoded = NULL;
	size_t i, k, length, widest = 0, encoded_size = 0, object_count = 0, attribute_count = 0;
	int64_t sec;
	uint32_t nsec;

	/* First pass: how many objects and path timestamps, and the widest directory. */
	for (k = 0; k < 2 && scans[k]; k++) {
		for (i = 0; i < scans[k]->object_count; i++) {
			if (!fyai_manifest_object_at(scans[k], i, digest, &object)) {
				errno = EBADMSG;
				return fy_invalid;
			}
			if (!object_visible(scans[k], manifest, digest))
				continue;
			object_count++;
			if (object.entry_count > widest)
				widest = object.entry_count;
		}
		for (i = 0; i < scans[k]->attribute_count; i++) {
			if (!fyai_manifest_attribute_at(scans[k], i, &path, &length, &sec, &nsec)) {
				errno = EBADMSG;
				return fy_invalid;
			}
			if (attribute_visible(scans[k], manifest, path, length))
				attribute_count++;
		}
	}
	pairs = malloc((object_count * 2 + 1) * sizeof(*pairs));
	attrs = malloc((attribute_count * 2 + 1) * sizeof(*attrs));
	entries = malloc((widest + 1) * sizeof(*entries));
	if (!pairs || !attrs || !entries) {
		errno = ENOMEM;
		goto out;
	}
	object_count = attribute_count = 0;
	for (k = 0; k < 2 && scans[k]; k++) {
		for (i = 0; i < scans[k]->object_count; i++) {
			if (!fyai_manifest_object_at(scans[k], i, digest, &object)) {
				errno = EBADMSG;
				goto out;
			}
			if (!object_visible(scans[k], manifest, digest))
				continue;
			pairs[object_count * 2 + 1] = object_to_generic(gb, &object, entries, hex);
			if (!fy_is_valid(pairs[object_count * 2 + 1]))
				goto out;
			fyai_cas_hex(hex, digest, sizeof(digest));
			pairs[object_count * 2] = fy_value(gb, hex);
			object_count++;
		}
	}
	objects = fyai_project_table_create(gb, object_count, pairs);
	for (k = 0; k < 2 && scans[k]; k++) {
		for (i = 0; i < scans[k]->attribute_count; i++) {
			if (!fyai_manifest_attribute_at(scans[k], i, &path, &length, &sec, &nsec)) {
				errno = EBADMSG;
				goto out;
			}
			if (!attribute_visible(scans[k], manifest, path, length))
				continue;
			if (length * 2 + 1 > encoded_size) {
				char *grown = realloc(encoded, length * 2 + 1);

				if (!grown) {
					errno = ENOMEM;
					goto out;
				}
				encoded = grown;
				encoded_size = length * 2 + 1;
			}
			fyai_cas_hex(encoded, path, length);
			attrs[attribute_count * 2] = fy_value(gb, encoded);
			attrs[attribute_count * 2 + 1] = fy_mapping(gb, "mtime_sec", (long long)sec,
								    "mtime_nsec", (long long)nsec);
			attribute_count++;
		}
	}
	attributes = fyai_project_table_create(gb, attribute_count, attrs);
	fyai_cas_hex(root, manifest->root, sizeof(manifest->root));
	if (!fy_is_mapping(objects) || !fy_is_mapping(attributes)) {
		errno = ENOMEM;
		goto out;
	}
	result = fy_mapping(gb, "version", 2LL, "algorithm", "blake3", "root", fy_value(gb, root),
			    "objects", objects, "attributes", attributes);
out:
	free(pairs);
	free(attrs);
	free(entries);
	free(encoded);
	return result;
}

static int generic_to_object(fy_generic value, struct fyai_mobject *object,
			     struct fyai_project_entry **entries, struct fyai_scratch *scratch)
{
	fy_generic blob, borrowed, list, item;
	const char *kind, *text;
	unsigned char *name;
	size_t count, i = 0, length;

	memset(object, 0, sizeof(*object));
	kind = fy_get(value, "kind", "");
	if (!strcmp(kind, "file"))
		object->kind = FYAI_PROJECT_FILE;
	else if (!strcmp(kind, "directory"))
		object->kind = FYAI_PROJECT_DIRECTORY;
	else if (!strcmp(kind, "symlink"))
		object->kind = FYAI_PROJECT_SYMLINK;
	else
		goto invalid;
	object->meta.mode = (uint32_t)fy_get(value, "mode", 0LL);
	object->meta.uid = (uint32_t)fy_get(value, "uid", 0LL);
	object->meta.gid = (uint32_t)fy_get(value, "gid", 0LL);
	object->meta.mtime_sec = fy_get(value, "mtime_sec", 0LL);
	object->meta.mtime_nsec = (uint32_t)fy_get(value, "mtime_nsec", 0LL);
	if (object->kind == FYAI_PROJECT_FILE) {
		blob = fy_get(value, "blob", fy_invalid);
		text = fy_get(blob, "digest", "");
		if (!fy_is_mapping(blob) || fyai_cas_digest_parse(object->blob.digest, text))
			goto invalid;
		object->blob.size = (uint64_t)fy_get(blob, "size", 0LL);
		object->blob.borrowed = fy_equal(fy_get(blob, "storage", ""), "borrowed");
		borrowed = fy_get(value, "borrowed", fy_invalid);
		if (object->blob.borrowed) {
			if (!fy_is_mapping(borrowed))
				goto invalid;
			object->blob.source_device = (uint64_t)fy_get(borrowed, "device", 0LL);
			object->blob.source_inode = (uint64_t)fy_get(borrowed, "inode", 0LL);
			object->blob.source_mode = (uint32_t)fy_get(borrowed, "mode", 0LL);
			object->blob.source_uid = (uint32_t)fy_get(borrowed, "uid", 0LL);
			object->blob.source_gid = (uint32_t)fy_get(borrowed, "gid", 0LL);
		}
	} else if (object->kind == FYAI_PROJECT_SYMLINK) {
		text = fy_get(value, "target_hex", "");
		name = fyai_scratch_alloc(scratch, strlen(text) / 2 + 1);
		if (!name || hex_decode(text, name, &length))
			goto invalid;
		object->target = name;
		object->target_length = length;
	} else {
		list = fy_get(value, "entries", fy_invalid);
		count = fy_len(list);
		*entries = fyai_scratch_calloc(scratch, count + 1, sizeof(**entries));
		if (!*entries)
			return -1;
		fy_foreach(item, list) {
			text = fy_get(item, "name_hex", "");
			name = fyai_scratch_alloc(scratch, strlen(text) / 2 + 1);
			if (!name || hex_decode(text, name, &length))
				goto invalid;
			(*entries)[i].name = name;
			(*entries)[i].name_length = length;
			kind = fy_get(item, "kind", "");
			(*entries)[i].kind = !strcmp(kind, "file") ? FYAI_PROJECT_FILE :
					     !strcmp(kind, "symlink") ? FYAI_PROJECT_SYMLINK :
					     !strcmp(kind, "directory") ? FYAI_PROJECT_DIRECTORY : 0;
			if (!(*entries)[i].kind ||
			    fyai_cas_digest_parse((*entries)[i].digest, fy_get(item, "digest", "")))
				goto invalid;
			i++;
		}
		object->entry_count = (uint32_t)i;
	}
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

int fyai_manifest_from_snapshot(struct fyai_manifest *manifest, fy_generic snapshot)
{
	struct fyai_scratch scratch = { 0 };
	struct fyai_manifest_builder builder;
	struct fyai_mobject object;
	struct fyai_project_entry *entries;
	unsigned char digest[FYAI_CAS_HASH_SIZE], root[FYAI_CAS_HASH_SIZE], *path;
	fy_generic objects, attributes, key, value;
	const char *name;
	size_t length;
	int rc = -1, saved;

	if (!fy_equal(fy_get(snapshot, "version", fy_invalid), 2LL) ||
	    fyai_cas_digest_parse(root, fy_get(snapshot, "root", ""))) {
		errno = EINVAL;
		return -1;
	}
	if (fyai_scratch_open(&scratch))
		return -1;
	if (fyai_manifest_builder_init(&builder, &scratch))
		goto out;
	objects = fy_get(snapshot, "objects", fy_invalid);
	fy_foreach_key_value(name, value, objects) {
		entries = NULL;
		if (fyai_cas_digest_parse(digest, name) ||
		    generic_to_object(value, &object, &entries, &scratch) ||
		    fyai_manifest_builder_add(&builder, digest, &object, entries))
			goto out;
	}
	attributes = fy_get(snapshot, "attributes", fy_invalid);
	fy_foreach_key_value(key, value, attributes) {
		name = fy_castp(&key, "");
		path = fyai_scratch_alloc(&scratch, strlen(name) / 2 + 1);
		if (!path || hex_decode(name, path, &length) ||
		    fyai_manifest_builder_attribute(&builder, path, length,
						    fy_get(value, "mtime_sec", 0LL),
						    (uint32_t)fy_get(value, "mtime_nsec", 0LL)))
			goto out;
	}
	rc = fyai_manifest_builder_finish(&builder, root, manifest);
out:
	saved = errno;
	fyai_scratch_close(&scratch);
	errno = saved;
	return rc;
}

/* Borrowed objects */

/*
 * Check the borrowed objects of scan. With a filter, an object that the filter
 * removed or replaced is skipped: it is a base object that a delta overrides.
 */
static int borrowed_scan(const struct fyai_manifest *scan, const struct fyai_manifest *filter,
			 int objects_fd, bool full, struct fy_blake3_hasher **hasher, char *error,
			 size_t error_size)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fyai_mobject object;
	unsigned char digest[FYAI_CAS_HASH_SIZE];
	const unsigned char *end;
	char hex[FYAI_CAS_DIGEST_SIZE];
	struct stat st;
	size_t i;
	int rc = 0, saved, fd;

	for (i = 0; i < scan->object_count; i++) {
		if (!fyai_manifest_object_at(scan, i, digest, &object)) {
			errno = EBADMSG;
			return -1;
		}
		if (object.kind != FYAI_PROJECT_FILE || !object.blob.borrowed)
			continue;
		if (filter && (digest_removed(filter, digest) || find_own_record(filter, digest, &end)))
			continue;
		if (full && !*hasher) {
			*hasher = fy_blake3_hasher_create(&cfg);
			if (!*hasher) {
				errno = ENOMEM;
				return -1;
			}
		}
		if (full) {
			rc = fyai_cas_verify_hasher(objects_fd, &object.blob, *hasher);
		} else {
			fd = fyai_cas_open(objects_fd, &object.blob);
			rc = fd < 0 ? -1 : fstat(fd, &st);
			if (!rc && (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != object.blob.size ||
				    !fyai_cas_borrowed_matches(&st, &object.blob))) {
				errno = EIO;
				rc = -1;
			}
			saved = errno;
			if (fd >= 0)
				close(fd);
			errno = saved;
		}
		if (rc) {
			fyai_cas_blob_hex(hex, &object.blob);
			if (error && error_size)
				snprintf(error, error_size,
					 "borrowed Git object %s; recreate with --copy-git-objects",
					 hex);
			return -1;
		}
	}
	return 0;
}

int fyai_manifest_check_borrowed(const struct fyai_manifest *manifest, int objects_fd, bool full,
				 char *error, size_t error_size)
{
	struct fy_blake3_hasher *hasher = NULL;
	int rc, saved;

	rc = borrowed_scan(manifest, NULL, objects_fd, full, &hasher, error, error_size);
	if (!rc && manifest->base)
		rc = borrowed_scan(manifest->base, manifest, objects_fd, full, &hasher, error,
				   error_size);
	saved = errno;
	if (hasher)
		fy_blake3_hasher_destroy(hasher);
	errno = saved;
	return rc;
}

/* Comparison */

/* The timestamp that a path has in a manifest: its attribute, else its object's. */
static void effective_time(const struct fyai_manifest *manifest, const unsigned char *path,
			   size_t length, const struct fyai_mobject *object, int64_t *sec,
			   uint32_t *nsec)
{
	if (!fyai_manifest_attribute(manifest, path, length, sec, nsec)) {
		*sec = object->meta.mtime_sec;
		*nsec = object->meta.mtime_nsec;
	}
}

struct walk_path {
	unsigned char bytes[PATH_MAX * 2];
	size_t length;
	size_t count;
};

static bool walk_push(struct walk_path *path, const unsigned char *name, size_t name_length,
		      size_t *mark)
{
	size_t need = (path->length ? 1 : 0) + name_length;

	if (path->length + need > sizeof(path->bytes))
		return false;
	*mark = path->length;
	if (path->length)
		path->bytes[path->length++] = '/';
	memcpy(path->bytes + path->length, name, name_length);
	path->length += name_length;
	return true;
}

static bool equal_walk(const struct fyai_manifest *a, const struct fyai_manifest *b,
		       const unsigned char digest[FYAI_CAS_HASH_SIZE], struct walk_path *path,
		       unsigned int depth)
{
	struct fyai_mobject left, right;
	struct fyai_mdir left_dir, right_dir;
	struct fyai_project_entry left_entry, right_entry;
	int64_t left_sec, right_sec;
	uint32_t left_nsec, right_nsec;
	size_t mark;
	bool equal;

	if (depth > FYAI_PROJECT_MAX_DEPTH || ++path->count > FYAI_PROJECT_MAX_NODES)
		return false;
	if (!fyai_manifest_find(a, digest, &left) || !fyai_manifest_find(b, digest, &right))
		return false;
	effective_time(a, path->bytes, path->length, &left, &left_sec, &left_nsec);
	effective_time(b, path->bytes, path->length, &right, &right_sec, &right_nsec);
	if (left_sec != right_sec || left_nsec != right_nsec)
		return false;
	if (left.kind != FYAI_PROJECT_DIRECTORY)
		return true;
	/* The same digest names the same entries: they come in the same order. */
	if (right.kind != FYAI_PROJECT_DIRECTORY || left.entry_count != right.entry_count ||
	    fyai_mdir_open(&left, &left_dir) || fyai_mdir_open(&right, &right_dir))
		return false;
	while (fyai_mdir_next(&left_dir, &left_entry)) {
		if (!fyai_mdir_next(&right_dir, &right_entry) ||
		    left_entry.name_length != right_entry.name_length ||
		    memcmp(left_entry.name, right_entry.name, left_entry.name_length) ||
		    memcmp(left_entry.digest, right_entry.digest, FYAI_CAS_HASH_SIZE))
			return false;
		if (!walk_push(path, left_entry.name, left_entry.name_length, &mark))
			return false;
		equal = equal_walk(a, b, left_entry.digest, path, depth + 1);
		path->length = mark;
		if (!equal)
			return false;
	}
	return true;
}

bool fyai_manifest_equal(const struct fyai_manifest *a, const struct fyai_manifest *b)
{
	struct walk_path *path;
	bool equal;

	if (memcmp(a->root, b->root, FYAI_CAS_HASH_SIZE))
		return false;
	path = calloc(1, sizeof(*path));
	if (!path)
		return false;
	equal = equal_walk(a, b, a->root, path, 0);
	free(path);
	return equal;
}

/* The state of one path in a diff: the kind and metadata with the effective time. */
struct diff_state {
	struct fyai_mobject object;
	int64_t sec;
	uint32_t nsec;
	bool present;
};

static bool state_equal(const struct diff_state *a, const struct diff_state *b)
{
	if (a->object.kind != b->object.kind || a->object.meta.mode != b->object.meta.mode ||
	    a->object.meta.uid != b->object.meta.uid || a->object.meta.gid != b->object.meta.gid ||
	    a->sec != b->sec || a->nsec != b->nsec)
		return false;
	if (a->object.kind == FYAI_PROJECT_FILE)
		return a->object.blob.size == b->object.blob.size &&
		       !memcmp(a->object.blob.digest, b->object.blob.digest,
			       sizeof(a->object.blob.digest));
	if (a->object.kind == FYAI_PROJECT_SYMLINK)
		return a->object.target_length == b->object.target_length &&
		       !memcmp(a->object.target, b->object.target, a->object.target_length);
	return true;
}

static fy_generic state_generic(struct fy_generic_builder *gb, const struct diff_state *state)
{
	char hex[FYAI_CAS_DIGEST_SIZE], *target;
	const struct fyai_mobject *o = &state->object;
	fy_generic value;

	value = fy_mapping(gb, "kind", o->kind == FYAI_PROJECT_FILE ? "file" :
				   o->kind == FYAI_PROJECT_SYMLINK ? "symlink" : "directory",
			   "mode", (long long)o->meta.mode, "uid", (long long)o->meta.uid, "gid",
			   (long long)o->meta.gid, "mtime_sec", (long long)state->sec,
			   "mtime_nsec", (long long)state->nsec);
	if (o->kind == FYAI_PROJECT_FILE) {
		fyai_cas_hex(hex, o->blob.digest, sizeof(o->blob.digest));
		value = fy_assoc(gb, value, "digest", fy_value(gb, hex));
		value = fy_assoc(gb, value, "size", (long long)o->blob.size);
	} else if (o->kind == FYAI_PROJECT_SYMLINK) {
		target = malloc(o->target_length * 2 + 1);
		if (!target)
			return fy_invalid;
		fyai_cas_hex(target, o->target, o->target_length);
		value = fy_assoc(gb, value, "target_hex", fy_value(gb, target));
		free(target);
	}
	return value;
}

struct diff_row {
	unsigned char *path;
	size_t length;
	struct diff_state before, after;
};

struct diff_walk {
	const struct fyai_manifest *side[2];
	struct walk_path path;
	struct diff_row *rows;
	size_t count, capacity;
};

static bool state_load(const struct fyai_manifest *manifest, const unsigned char digest[FYAI_CAS_HASH_SIZE],
		       const struct walk_path *path, struct diff_state *state)
{
	memset(state, 0, sizeof(*state));
	if (!digest)
		return true;
	if (!fyai_manifest_find(manifest, digest, &state->object))
		return false;
	effective_time(manifest, path->bytes, path->length, &state->object, &state->sec,
		       &state->nsec);
	state->present = true;
	return true;
}

static bool diff_hidden(const struct fyai_project_entry *entry)
{
	return (entry->name_length == 4 && !memcmp(entry->name, ".git", 4)) ||
	       (entry->name_length == 5 && !memcmp(entry->name, ".fyai", 5));
}

static int diff_step(struct diff_walk *walk, const unsigned char *left_digest,
		     const unsigned char *right_digest, unsigned int depth)
{
	struct diff_state before, after;
	struct diff_row *rows, *row;
	struct fyai_mdir directories[2];
	struct fyai_project_entry entries[2];
	bool have[2] = { false, false };
	size_t mark, common, k;
	int order;

	if (depth > FYAI_PROJECT_MAX_DEPTH || walk->path.count++ > 2 * FYAI_PROJECT_MAX_NODES) {
		errno = EINVAL;
		return -1;
	}
	if (!state_load(walk->side[0], left_digest, &walk->path, &before) ||
	    !state_load(walk->side[1], right_digest, &walk->path, &after)) {
		errno = EBADMSG;
		return -1;
	}
	if (!before.present || !after.present || !state_equal(&before, &after)) {
		if (walk->count == walk->capacity) {
			walk->capacity = walk->capacity ? walk->capacity * 2 : 64;
			rows = realloc(walk->rows, walk->capacity * sizeof(*rows));
			if (!rows) {
				errno = ENOMEM;
				return -1;
			}
			walk->rows = rows;
		}
		row = &walk->rows[walk->count++];
		row->path = malloc(walk->path.length + 1);
		if (!row->path) {
			walk->count--;
			errno = ENOMEM;
			return -1;
		}
		memcpy(row->path, walk->path.bytes, walk->path.length);
		row->length = walk->path.length;
		row->before = before;
		row->after = after;
	}
	/* Walk the children of both directories together, in name order. */
	for (k = 0; k < 2; k++) {
		const struct diff_state *state = k ? &after : &before;

		if (state->present && state->object.kind == FYAI_PROJECT_DIRECTORY) {
			if (fyai_mdir_open(&state->object, &directories[k])) {
				errno = EBADMSG;
				return -1;
			}
			have[k] = fyai_mdir_next(&directories[k], &entries[k]);
			while (have[k] && diff_hidden(&entries[k]))
				have[k] = fyai_mdir_next(&directories[k], &entries[k]);
		}
	}
	while (have[0] || have[1]) {
		if (!have[0]) {
			order = 1;
		} else if (!have[1]) {
			order = -1;
		} else {
			common = entries[0].name_length < entries[1].name_length ?
					 entries[0].name_length : entries[1].name_length;
			order = memcmp(entries[0].name, entries[1].name, common);
			if (!order)
				order = (entries[0].name_length > entries[1].name_length) -
					(entries[0].name_length < entries[1].name_length);
		}
		k = order > 0 ? 1 : 0;
		if (!walk_push(&walk->path, entries[k].name, entries[k].name_length, &mark)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (diff_step(walk, order <= 0 ? entries[0].digest : NULL,
			      order >= 0 ? entries[1].digest : NULL, depth + 1))
			return -1;
		walk->path.length = mark;
		if (order <= 0) {
			have[0] = fyai_mdir_next(&directories[0], &entries[0]);
			while (have[0] && diff_hidden(&entries[0]))
				have[0] = fyai_mdir_next(&directories[0], &entries[0]);
		}
		if (order >= 0) {
			have[1] = fyai_mdir_next(&directories[1], &entries[1]);
			while (have[1] && diff_hidden(&entries[1]))
				have[1] = fyai_mdir_next(&directories[1], &entries[1]);
		}
	}
	return 0;
}

static int diff_row_order(const void *a, const void *b)
{
	const struct diff_row *left = a, *right = b;

	return bytes_compare(left->path, left->length, right->path, right->length);
}

fy_generic fyai_manifest_diff(struct fy_generic_builder *gb, const struct fyai_manifest *left,
			      const struct fyai_manifest *right)
{
	struct diff_walk *walk;
	fy_generic *items = NULL, result = fy_invalid;
	struct diff_row *row;
	char *path = NULL, *hex = NULL;
	size_t i, longest = 0;
	int saved;

	walk = calloc(1, sizeof(*walk));
	if (!walk) {
		errno = ENOMEM;
		return fy_invalid;
	}
	walk->side[0] = left;
	walk->side[1] = right;
	if (diff_step(walk, left->root, right->root, 0))
		goto out;
	if (walk->count)
		qsort(walk->rows, walk->count, sizeof(*walk->rows), diff_row_order);
	for (i = 0; i < walk->count; i++)
		if (walk->rows[i].length > longest)
			longest = walk->rows[i].length;
	items = malloc((walk->count + 1) * sizeof(*items));
	path = malloc(longest + 2);
	hex = malloc(longest * 2 + 1);
	if (!items || !path || !hex) {
		errno = ENOMEM;
		goto out;
	}
	for (i = 0; i < walk->count; i++) {
		row = &walk->rows[i];
		memcpy(path, row->path, row->length);
		path[row->length] = '\0';
		fyai_cas_hex(hex, row->path, row->length);
		items[i] = fy_mapping(gb, "path", fy_value(gb, row->length ? path : "."), "path_hex",
				      fy_value(gb, hex), "status",
				      !row->before.present ? "added" :
				      !row->after.present ? "deleted" : "modified",
				      "before", row->before.present ? state_generic(gb, &row->before) :
								      fy_null,
				      "after", row->after.present ? state_generic(gb, &row->after) :
								    fy_null);
		if (!fy_is_valid(items[i])) {
			errno = ENOMEM;
			goto out;
		}
	}
	result = fy_gb_sequence_create(gb, walk->count, items);
out:
	saved = errno;
	if (walk) {
		for (i = 0; i < walk->count; i++)
			free(walk->rows[i].path);
		free(walk->rows);
		free(walk);
	}
	free(items);
	free(path);
	free(hex);
	errno = saved;
	return result;
}
