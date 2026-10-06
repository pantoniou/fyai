/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAS_H
#define FYAI_CAS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

/* Raw BLAKE3-256 size, and the size of its lowercase hex name with a NUL. */
#define FYAI_CAS_HASH_SIZE 32
#define FYAI_CAS_DIGEST_SIZE (FYAI_CAS_HASH_SIZE * 2 + 1)

struct fy_blake3_hasher;
struct stat;

/* Write count bytes as lowercase hex and a NUL; out holds count * 2 + 1 bytes. */
void fyai_cas_hex(char *out, const unsigned char *bytes, size_t count);

/*
 * Decode the 64 lowercase hex digits of a digest name. Anything else sets
 * errno to EINVAL and returns -1; out is unchanged on failure.
 */
int fyai_cas_digest_parse(unsigned char out[FYAI_CAS_HASH_SIZE], const char *hex);

struct fyai_cas_blob;

/* Size of the name of an object, with its NUL: borrowed/, two digits, a slash, the rest. */
#define FYAI_CAS_NAME_SIZE (9 + FYAI_CAS_DIGEST_SIZE + 1)

/* Size of the overlay redirect string of a blob, with its NUL. */
#define FYAI_CAS_REDIRECT_SIZE (FYAI_CAS_NAME_SIZE + 1)

/* Write the lowercase hex name of the blob digest and a NUL. */
void fyai_cas_blob_hex(char out[FYAI_CAS_DIGEST_SIZE], const struct fyai_cas_blob *blob);

/*
 * Write the object name relative to the objects directory: the hex digest
 * split after its first byte, AA/BBCC..., prefixed with borrowed/ for a
 * borrowed object. A directory holds at most 256 entries of the level above
 * it, so no directory grows with the number of objects. size must be at least
 * FYAI_CAS_NAME_SIZE.
 */
int fyai_cas_name(char *path, size_t size, const struct fyai_cas_blob *blob);

/*
 * Create the directories that hold the object name of the blob below
 * directory_fd, and those that exist are kept. A caller that links or creates
 * the name makes them first.
 */
int fyai_cas_mkdirs(int directory_fd, const struct fyai_cas_blob *blob);

/*
 * Write the overlay redirect "/<name>" of a blob: the object name below the
 * data layer, which has the layout of the objects directory.
 */
void fyai_cas_redirect(char out[FYAI_CAS_REDIRECT_SIZE], const struct fyai_cas_blob *blob);

/* How bytes move from a source to a CAS file. */
enum fyai_cas_method {
	/* A worker is probing the filesystem pair; others wait. */
	FYAI_CAS_METHOD_PROBING = -1,
	/* Not probed yet. */
	FYAI_CAS_METHOD_NONE = 0,
	FYAI_CAS_METHOD_REFLINK,
	FYAI_CAS_METHOD_COPY_RANGE,
	FYAI_CAS_METHOD_SENDFILE,
};

/* One source/destination filesystem pair; shared by all capture workers. */
struct fyai_cas_copy_state {
	/* Caller completes a filesystem barrier before durable publication. */
	bool defer_file_sync;
	/* Caller selects the persistence barrier for capture. */
	bool defer_directory_sync;
	_Atomic enum fyai_cas_method backend;
	atomic_uint_fast64_t copied_bytes;
	atomic_uint_fast64_t reflinked_bytes;
};

struct fyai_cas_blob {
	/* Raw BLAKE3-256 digest; the object name is its lowercase hex form. */
	unsigned char digest[FYAI_CAS_HASH_SIZE];
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
 * publication. FYAI_CAS_HASHED_BY_PATH names the file by its path instead of a
 * descriptor: the caller owns the file, and no other process writes it.
 */
#define FYAI_CAS_HASHED_BY_PATH (-2)
int fyai_cas_link_hashed(int directory_fd, int source_directory, const char *path,
			 const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			 int hashed_fd);
int fyai_cas_link_hashed_state(int directory_fd, int source_directory, const char *path,
			       const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			       int hashed_fd, struct fyai_cas_copy_state *state);
int fyai_cas_open(int directory_fd, const struct fyai_cas_blob *blob);

int fyai_cas_clone_method(int source_fd, int target_fd, uint64_t size,
			  struct fyai_cas_copy_state *state, enum fyai_cas_method *method);
int fyai_cas_clone_state(int source_fd, int target_fd, uint64_t size,
			 struct fyai_cas_copy_state *state);
int fyai_cas_put_hasher_state(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			      struct fy_blake3_hasher *hasher, struct fyai_cas_copy_state *state);

/*
 * Like fyai_cas_put_hasher_state() for a regular file at offset 0 whose size
 * the caller read. The call neither stats nor seeks the source, and the offset
 * of the source afterwards is unspecified.
 */
int fyai_cas_put_sized_state(int directory_fd, int source_fd, uint64_t size,
			     struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			     struct fyai_cas_copy_state *state);

/* Hash the first size bytes of a regular file without stat or seek. */
int fyai_cas_hash_file_sized(int fd, uint64_t size, struct fyai_cas_blob *blob,
			     struct fy_blake3_hasher *hasher);

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

/* Whether an inode has the device, inode, mode and owner recorded for a blob. */
bool fyai_cas_borrowed_matches(const struct stat *st, const struct fyai_cas_blob *blob);

/* Verify a no-follow regular object against its complete identity. */
int fyai_cas_verify(int directory_fd, const struct fyai_cas_blob *blob);

#endif
