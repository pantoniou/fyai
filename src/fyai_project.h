/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROJECT_H
#define FYAI_PROJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_cas.h"

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
	char digest[FYAI_CAS_DIGEST_SIZE];
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

#endif
