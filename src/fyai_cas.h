/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAS_H
#define FYAI_CAS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define FYAI_CAS_DIGEST_SIZE 65

struct fy_blake3_hasher;

/* One source/destination filesystem pair; shared by all capture workers. */
struct fyai_cas_copy_state {
	/* Caller completes a filesystem barrier before durable publication. */
	bool defer_file_sync;
	/* Caller selects the persistence barrier for capture. */
	bool defer_directory_sync;
	atomic_int backend;
	atomic_uint_fast64_t copied_bytes;
	atomic_uint_fast64_t reflinked_bytes;
};

struct fyai_cas_blob {
	char digest[FYAI_CAS_DIGEST_SIZE];
	uint64_t size;
	bool borrowed;
	uint32_t source_mode, source_uid, source_gid;
	uint64_t source_device, source_inode;
};

/*
 * Publish a regular file from its current offset into an owned, private
 * directory. The caller owns both descriptors and validates source stability
 * during capture. Hashing maps the private copy; source truncation fails
 * without a mapped fault. Names are lowercase BLAKE3-256 digests. Existing
 * objects are verified before reuse. Shared baseline inodes retain project
 * metadata; owned temporaries use mode 0444. On failure errno is set; an
 * installed object can remain after a durability error. The output is valid
 * only on success. This call grants no agent access to storage.
 */
int fyai_cas_put(int directory_fd, int source_fd, struct fyai_cas_blob *blob);

/* The borrowed hasher belongs to one worker and is reset before each hash. */
int fyai_cas_put_hasher(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			struct fy_blake3_hasher *hasher);

/*
 * Hash an immutable inode through mmap; the caller excludes concurrent writes.
 */
int fyai_cas_hash_file(int fd, struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher);

/*
 * Publish a closed immutable inode; borrowed objects use a separate namespace.
 */
int fyai_cas_link(int directory_fd, int source_directory, const char *path,
		  const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher);
/* Reuse a just-computed digest only when hashed_fd and the CAS path share an
 * inode. The caller excludes writes and validates source stability after
 * publication.
 */
int fyai_cas_link_hashed(int directory_fd, int source_directory, const char *path,
			 const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			 int hashed_fd);
int fyai_cas_link_hashed_state(int directory_fd, int source_directory, const char *path,
			       const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			       int hashed_fd, struct fyai_cas_copy_state *state);
int fyai_cas_open(int directory_fd, const struct fyai_cas_blob *blob);

int fyai_cas_clone_method(int source_fd, int target_fd, uint64_t size,
			  struct fyai_cas_copy_state *state, int *method);
int fyai_cas_clone_state(int source_fd, int target_fd, uint64_t size,
			 struct fyai_cas_copy_state *state);
int fyai_cas_put_hasher_state(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			      struct fy_blake3_hasher *hasher, struct fyai_cas_copy_state *state);

/* Clone full files when supported, else copy the exact byte range. */
int fyai_cas_clone(int source_fd, int target_fd, uint64_t size);

/* Copy exactly size bytes at the current offsets; unsupported ranges use
 * sendfile. */
int fyai_cas_copy(int source_fd, int target_fd, uint64_t size);

/* Verification borrows one hasher; use num_threads=-1 for independent serial
 * checks. */
int fyai_cas_verify_hasher(int directory_fd, const struct fyai_cas_blob *blob,
			   struct fy_blake3_hasher *hasher);
int fyai_cas_verify_file(int fd, const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher);

/* Verify a no-follow regular object against its complete identity. */
int fyai_cas_verify(int directory_fd, const struct fyai_cas_blob *blob);

#endif
