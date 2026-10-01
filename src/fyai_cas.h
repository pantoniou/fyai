/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAS_H
#define FYAI_CAS_H

#include <stdint.h>

#define FYAI_CAS_DIGEST_SIZE 65

struct fyai_cas_blob {
	char digest[FYAI_CAS_DIGEST_SIZE];
	uint64_t size;
};

/*
 * Publish a regular file from its current offset into an owned, private directory.
 * The caller owns both descriptors and excludes source writers during capture.
 * mmap requires that the source cannot be truncated during this call.
 * Names are lowercase BLAKE3-256 digests. Existing objects are verified before reuse.
 * On failure errno is set; an installed object can remain after a durability error.
 * The output is valid only on success. This call grants no agent access to storage.
 */
int fyai_cas_put(int directory_fd, int source_fd, struct fyai_cas_blob *blob);

/* Verify a no-follow regular object against its complete identity. */
int fyai_cas_verify(int directory_fd, const struct fyai_cas_blob *blob);

#endif
