/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAS_H
#define FYAI_CAS_H

#include <stdint.h>

#define FYAI_CAS_DIGEST_SIZE 65

struct fy_blake3_hasher;

struct fyai_cas_blob {
	char digest[FYAI_CAS_DIGEST_SIZE];
	uint64_t size;
};

/*
 * Publish a regular file from its current offset into an owned, private directory.
 * The caller owns both descriptors and validates source stability during capture.
 * Hashing maps the private copy; source truncation fails without a mapped fault.
 * Names are lowercase BLAKE3-256 digests. Existing objects are verified before reuse.
 * On failure errno is set; an installed object can remain after a durability error.
 * The output is valid only on success. This call grants no agent access to storage.
 */
int fyai_cas_put(int directory_fd, int source_fd, struct fyai_cas_blob *blob);

/* The borrowed hasher belongs to one worker and is reset before each hash. */
int fyai_cas_put_hasher(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
		      struct fy_blake3_hasher *hasher);

/* Copy exactly size bytes at the current offsets; unsupported ranges use sendfile. */
int fyai_cas_copy(int source_fd, int target_fd, uint64_t size);

/* Verification borrows one hasher; use num_threads=-1 for independent serial checks. */
int fyai_cas_verify_hasher(int directory_fd, const struct fyai_cas_blob *blob,
			 struct fy_blake3_hasher *hasher);
int fyai_cas_verify_file(int fd, const struct fyai_cas_blob *blob,
		       struct fy_blake3_hasher *hasher);

/* Verify a no-follow regular object against its complete identity. */
int fyai_cas_verify(int directory_fd, const struct fyai_cas_blob *blob);

#endif
