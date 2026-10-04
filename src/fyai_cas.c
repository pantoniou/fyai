/*
 * fyai_cas.c - content-addressed blob store for captured project files
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include "fyai_cas.h"
#include "utils.h"

void fyai_cas_hex(char *out, const unsigned char *bytes, size_t count)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < count; i++) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 15];
	}
	out[count * 2] = '\0';
}

int fyai_cas_digest_parse(unsigned char out[FYAI_CAS_HASH_SIZE], const char *hex)
{
	unsigned char bytes[FYAI_CAS_HASH_SIZE];
	int high, low;
	size_t i;

	for (i = 0; i < FYAI_CAS_HASH_SIZE; i++) {
		high = hex_nibble(hex[i * 2]);
		low = high < 0 ? -1 : hex_nibble(hex[i * 2 + 1]);
		if (low < 0) {
			errno = EINVAL;
			return -1;
		}
		bytes[i] = (unsigned char)(high << 4 | low);
	}
	if (hex[FYAI_CAS_HASH_SIZE * 2]) {
		errno = EINVAL;
		return -1;
	}
	memcpy(out, bytes, sizeof(bytes));
	return 0;
}

void fyai_cas_blob_hex(char out[FYAI_CAS_DIGEST_SIZE], const struct fyai_cas_blob *blob)
{
	fyai_cas_hex(out, blob->digest, sizeof(blob->digest));
}

void fyai_cas_redirect(char out[FYAI_CAS_REDIRECT_SIZE], const struct fyai_cas_blob *blob)
{
	out[0] = '/';
	if (blob->borrowed)
		memcpy(out + 1, "b-", 2);
	fyai_cas_blob_hex(out + 1 + (blob->borrowed ? 2 : 0), blob);
}

int fyai_cas_name(char *path, size_t size, const struct fyai_cas_blob *blob)
{
	size_t prefix = blob->borrowed ? 9 : 0;

	if (size < prefix + FYAI_CAS_DIGEST_SIZE) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (prefix)
		memcpy(path, "borrowed/", prefix);
	fyai_cas_blob_hex(path + prefix, blob);
	return 0;
}

#ifdef __linux__
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sched.h>
#include <limits.h>
#include <unistd.h>

#include <libfyaml/libfyaml-blake3.h>
#include <openssl/rand.h>

static bool cas_copy_unsupported(int err)
{
	return err == EXDEV || err == EOPNOTSUPP || err == ENOSYS || err == EINVAL;
}

/* Each method moves at most one chunk at the current offsets. */
static ssize_t cas_copy_chunk_range(int source_fd, int target_fd, size_t count)
{
	return copy_file_range(source_fd, NULL, target_fd, NULL, count, 0);
}

static ssize_t cas_copy_chunk_sendfile(int source_fd, int target_fd, size_t count)
{
	return sendfile(target_fd, source_fd, NULL, count);
}

static ssize_t (*const cas_copy_chunk[])(int, int, size_t) = {
	[FYAI_CAS_METHOD_COPY_RANGE] = cas_copy_chunk_range,
	[FYAI_CAS_METHOD_SENDFILE] = cas_copy_chunk_sendfile,
};

/*
 * Copy exactly size bytes. The first chunk probes the methods in order of
 * preference; *method names the one that works and is reused afterwards.
 */
static int cas_copy_bytes(int source_fd, int target_fd, uint64_t size, enum fyai_cas_method *method,
			  uint_fast64_t *copied_bytes)
{
	enum fyai_cas_method current = *method;
	ssize_t copied;
	size_t count;

	if (current != FYAI_CAS_METHOD_COPY_RANGE && current != FYAI_CAS_METHOD_SENDFILE)
		current = FYAI_CAS_METHOD_COPY_RANGE;
	while (size) {
		count = size > (1U << 30) ? (1U << 30) : (size_t)size;
		copied = cas_copy_chunk[current](source_fd, target_fd, count);
		if (copied < 0 && errno == EINTR)
			continue;
		if (copied < 0 && current == FYAI_CAS_METHOD_COPY_RANGE &&
		    cas_copy_unsupported(errno)) {
			current = FYAI_CAS_METHOD_SENDFILE;
			continue;
		}
		if (copied <= 0) {
			if (!copied)
				errno = EAGAIN;
			return -1;
		}
		*method = current;
		if (copied_bytes)
			*copied_bytes += (uint64_t)copied;
		size -= (uint64_t)copied;
	}
	return 0;
}

int fyai_cas_copy(int source_fd, int target_fd, uint64_t size)
{
	enum fyai_cas_method method = FYAI_CAS_METHOD_NONE;

	return cas_copy_bytes(source_fd, target_fd, size, &method, NULL);
}

/*
 * Reflink a whole file. Returns 0 when cloned, 1 when the filesystem pair
 * does not support it, and -1 on error.
 */
static int cas_reflink(int source_fd, int target_fd, uint64_t size)
{
	struct stat st;
	int rc;

	rc = fstat(source_fd, &st);
	if (rc)
		return -1;
	if ((uint64_t)st.st_size != size || lseek(source_fd, 0, SEEK_CUR) != 0 ||
	    lseek(target_fd, 0, SEEK_CUR) != 0)
		return 1;
	do {
		rc = ioctl(target_fd, FICLONE, source_fd);
	} while (rc && errno == EINTR);
	if (rc)
		return cas_copy_unsupported(errno) || errno == ENOTTY ? 1 : -1;
	rc = lseek(source_fd, size, SEEK_SET) < 0 || lseek(target_fd, size, SEEK_SET) < 0;
	return rc ? -1 : 0;
}

int fyai_cas_clone_method(int source_fd, int target_fd, uint64_t size,
			  struct fyai_cas_copy_state *state, enum fyai_cas_method *method)
{
	struct fyai_cas_copy_state local = { .backend = FYAI_CAS_METHOD_NONE };
	enum fyai_cas_method current, used;
	uint_fast64_t copied = 0;
	int expected, rc;
	bool probe = false;

	if (method)
		*method = FYAI_CAS_METHOD_COPY_RANGE;
	if (!size)
		return 0;
	if (!state)
		state = &local;
	/* One worker probes; the others wait for its answer. */
	for (;;) {
		current = atomic_load_explicit(&state->backend, memory_order_acquire);
		if (current != FYAI_CAS_METHOD_NONE && current != FYAI_CAS_METHOD_PROBING)
			break;
		if (current == FYAI_CAS_METHOD_NONE) {
			expected = FYAI_CAS_METHOD_NONE;
			if (atomic_compare_exchange_strong_explicit(
				    &state->backend, &expected, FYAI_CAS_METHOD_PROBING,
				    memory_order_acq_rel, memory_order_acquire)) {
				probe = true;
				break;
			}
		}
		sched_yield();
	}
	if (probe || current == FYAI_CAS_METHOD_REFLINK) {
		rc = cas_reflink(source_fd, target_fd, size);
		if (rc < 0)
			goto err_out;
		if (!rc) {
			atomic_store_explicit(&state->backend, FYAI_CAS_METHOD_REFLINK,
					      memory_order_release);
			atomic_fetch_add_explicit(&state->reflinked_bytes, size,
						  memory_order_relaxed);
			if (method)
				*method = FYAI_CAS_METHOD_REFLINK;
			return 0;
		}
		current = FYAI_CAS_METHOD_COPY_RANGE;
	}
	used = current;
	rc = cas_copy_bytes(source_fd, target_fd, size, &used, &copied);
	if (rc)
		goto err_out;
	/* Only a probe, or a drop to sendfile, changes the shared method. */
	if (probe || used == FYAI_CAS_METHOD_SENDFILE)
		atomic_store_explicit(&state->backend, used, memory_order_release);
	atomic_fetch_add_explicit(&state->copied_bytes, copied, memory_order_relaxed);
	if (method)
		*method = used;
	return 0;
err_out:
	if (probe)
		atomic_store_explicit(&state->backend, FYAI_CAS_METHOD_NONE, memory_order_release);
	return -1;
}

int fyai_cas_clone_state(int source_fd, int target_fd, uint64_t size,
			 struct fyai_cas_copy_state *state)
{
	return fyai_cas_clone_method(source_fd, target_fd, size, state, NULL);
}

int fyai_cas_clone(int source_fd, int target_fd, uint64_t size)
{
	return fyai_cas_clone_state(source_fd, target_fd, size, NULL);
}

int fyai_cas_open(int directory_fd, const struct fyai_cas_blob *blob)
{
	char path[9 + FYAI_CAS_DIGEST_SIZE];
	int rc;

	rc = fyai_cas_name(path, sizeof(path), blob);
	if (rc)
		return -1;
	return openat(directory_fd, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
}

/* Up to this size, read the bytes into a stack buffer: a read costs less than a mapping. */
#define CAS_SMALL_FILE (64 * 1024)

/*
 * Hash the bytes of source_fd from offset start to offset length, and store
 * the digest and the size in blob.
 *
 * With target_fd, clone the bytes into the target first and hash that copy.
 * A host process that writes to the source cannot change the copy, so the
 * digest names the bytes that were stored. Without target_fd, hash the source,
 * which the caller keeps immutable. The offset of source_fd does not change.
 */
static int cas_hash_range(int source_fd, int target_fd, off_t start, off_t length,
			  struct fyai_cas_blob *blob, struct fy_blake3_hasher *md,
			  struct fyai_cas_copy_state *state)
{
	unsigned char small[CAS_SMALL_FILE];
	const uint8_t *digest;
	void *source = MAP_FAILED, *captured = MAP_FAILED;
	const unsigned char *data = NULL;
	size_t size, amount = 0;
	ssize_t count;
	off_t offset;
	int rc = -1, saved;

	size = start < length ? (size_t)(length - start) : 0;
	if (target_fd >= 0) {
		rc = fyai_cas_clone_state(source_fd, target_fd, size, state);
		if (rc)
			goto out;
		rc = -1;
	}
	if (size && size <= sizeof(small)) {
		offset = target_fd >= 0 ? 0 : start;
		while (amount < size) {
			count = pread(target_fd >= 0 ? target_fd : source_fd, small + amount,
				      size - amount, offset + (off_t)amount);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				if (!count)
					errno = EIO;
				goto out;
			}
			amount += (size_t)count;
		}
		data = small;
	} else if (size && target_fd >= 0) {
		captured = mmap(NULL, size, PROT_READ, MAP_PRIVATE, target_fd, 0);
		if (captured == MAP_FAILED)
			goto out;
		data = captured;
	} else if (size) {
		source = mmap(NULL, (size_t)length, PROT_READ, MAP_PRIVATE, source_fd, 0);
		if (source == MAP_FAILED)
			goto out;
		data = (const unsigned char *)source + (size_t)start;
	}
	fy_blake3_hasher_reset(md);
	digest = fy_blake3_hash(md, size ? data : (const unsigned char *)"", size);
	if (!digest) {
		errno = EIO;
		goto out;
	}
	memcpy(blob->digest, digest, sizeof(blob->digest));
	blob->size = size;
	rc = 0;
out:
	saved = errno;
	if (captured != MAP_FAILED)
		munmap(captured, size);
	if (source != MAP_FAILED)
		munmap(source, (size_t)length);
	errno = saved;
	return rc;
}

/* Hash from the current offset to the end, and leave the offset at the end. */
static int cas_copy_hash(int source_fd, int target_fd, struct fyai_cas_blob *blob,
			 struct fy_blake3_hasher *md, struct fyai_cas_copy_state *state)
{
	struct stat st;
	off_t start;
	int rc;

	rc = fstat(source_fd, &st);
	if (rc)
		return -1;
	if (!S_ISREG(st.st_mode) || st.st_size < 0) {
		errno = EINVAL;
		return -1;
	}
	if ((uintmax_t)st.st_size > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	start = lseek(source_fd, 0, SEEK_CUR);
	if (start < 0)
		return -1;
	rc = cas_hash_range(source_fd, target_fd, start, st.st_size, blob, md, state);
	if (!rc && start < st.st_size && lseek(source_fd, st.st_size, SEEK_SET) < 0)
		return -1;
	return rc;
}

int fyai_cas_hash_file_sized(int fd, uint64_t size, struct fyai_cas_blob *blob,
			     struct fy_blake3_hasher *hasher)
{
	if (size > (uint64_t)INT64_MAX || size > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	return cas_hash_range(fd, -1, 0, (off_t)size, blob, hasher, NULL);
}

int fyai_cas_hash_file(int fd, struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher)
{
	return cas_copy_hash(fd, -1, blob, hasher, NULL);
}

/*
 * An object needs no rehash when it is the inode of the hashed descriptor. An
 * owned object also needs none when it has the published form: a regular file of
 * the blob size, read-only. Publication makes an owned object read-only after its
 * content is complete, and the store is private. A borrowed object names a file
 * outside the store, so only the inode test applies to it.
 */
static int cas_verify_linked(int directory_fd, const char *name, const struct fyai_cas_blob *blob,
			     struct fy_blake3_hasher *hasher, int hashed_fd, int source_directory,
			     const char *path)
{
	struct stat source, object;
	int rc;

	rc = fstatat(directory_fd, name, &object, AT_SYMLINK_NOFOLLOW);
	if (rc)
		return -1;
	if (S_ISREG(object.st_mode) && (uint64_t)object.st_size == blob->size) {
		if (!blob->borrowed && (object.st_mode & 07777) == 0444)
			return 0;
		if (hashed_fd >= 0)
			rc = fstat(hashed_fd, &source);
		else if (hashed_fd == FYAI_CAS_HASHED_BY_PATH)
			rc = fstatat(source_directory, path, &source, AT_SYMLINK_NOFOLLOW);
		else
			rc = -1;
		if (!rc && source.st_dev == object.st_dev && source.st_ino == object.st_ino)
			return 0;
	}
	return fyai_cas_verify_hasher(directory_fd, blob, hasher);
}

int fyai_cas_link_hashed_state(int directory_fd, int source_directory, const char *path,
			       const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			       int hashed_fd, struct fyai_cas_copy_state *state)
{
	char name[9 + FYAI_CAS_DIGEST_SIZE];
	int rc, fd = -1, borrowed = -1, saved;

	rc = fyai_cas_name(name, sizeof(name), blob);
	if (rc)
		return -1;
	if (blob->borrowed) {
		rc = mkdirat(directory_fd, "borrowed", 0700);
		if (rc && errno != EEXIST)
			return -1;
	}
	rc = linkat(source_directory, path, directory_fd, name, 0);
	if (rc && errno != EEXIST)
		return -1;
	/* A fresh link to an owned object is the object just hashed. */
	if (rc || blob->borrowed) {
		rc = cas_verify_linked(directory_fd, name, blob, hasher, hashed_fd, source_directory,
				       path);
		if (rc)
			return -1;
	}
	rc = -1;
	if (blob->borrowed && (!state || !state->defer_file_sync)) {
		fd = fyai_cas_open(directory_fd, blob);
		if (fd < 0)
			goto err_out;
		if (fsync(fd))
			goto err_out;
	}
	if (state && state->defer_directory_sync) {
		rc = 0;
		goto err_out;
	}
	if (blob->borrowed) {
		borrowed = openat(directory_fd, "borrowed",
				  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (borrowed < 0)
			goto err_out;
		if (fsync(borrowed))
			goto err_out;
	}
	rc = fsync(directory_fd);
err_out:
	saved = errno;
	if (fd >= 0)
		close(fd);
	if (borrowed >= 0)
		close(borrowed);
	errno = saved;
	return rc;
}

int fyai_cas_link_hashed(int directory_fd, int source_directory, const char *path,
			 const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			 int hashed_fd)
{
	return fyai_cas_link_hashed_state(directory_fd, source_directory, path, blob, hasher,
					  hashed_fd, NULL);
}

int fyai_cas_link(int directory_fd, int source_directory, const char *path,
		  const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher)
{
	return fyai_cas_link_hashed(directory_fd, source_directory, path, blob, hasher, -1);
}

int fyai_cas_verify_file(int fd, const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher)
{
	struct fyai_cas_blob actual;
	int rc;

	rc = cas_copy_hash(fd, -1, &actual, hasher, NULL);
	if (!rc && (actual.size != blob->size ||
		    memcmp(actual.digest, blob->digest, sizeof(actual.digest)))) {
		errno = EIO;
		return -1;
	}
	return rc;
}

bool fyai_cas_borrowed_matches(const struct stat *st, const struct fyai_cas_blob *blob)
{
	return (uint64_t)st->st_ino == blob->source_inode &&
	       (uint64_t)st->st_dev == blob->source_device &&
	       (st->st_mode & 07777) == blob->source_mode && st->st_uid == blob->source_uid &&
	       st->st_gid == blob->source_gid;
}

int fyai_cas_verify_hasher(int directory_fd, const struct fyai_cas_blob *blob,
			   struct fy_blake3_hasher *hasher)
{
	struct fyai_cas_blob actual;
	struct stat st;
	int fd, rc, saved;

	fd = fyai_cas_open(directory_fd, blob);
	if (fd < 0)
		return -1;
	rc = fstat(fd, &st);
	if (!rc && !S_ISREG(st.st_mode)) {
		errno = EINVAL;
		rc = -1;
	}
	if (!rc && blob->borrowed && blob->source_inode && !fyai_cas_borrowed_matches(&st, blob)) {
		errno = EIO;
		rc = -1;
	}
	if (!rc)
		rc = cas_copy_hash(fd, -1, &actual, hasher, NULL);
	if (!rc && (actual.size != blob->size ||
		    memcmp(actual.digest, blob->digest, sizeof(actual.digest)))) {
		errno = EIO;
		rc = -1;
	}
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

/*
 * Publish a source file. A known size means that the caller read the size of a
 * regular file at offset 0, and the file is not stat'ed or seeked again.
 */
static int cas_put(int directory_fd, int source_fd, bool sized, uint64_t size,
		   struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
		   struct fyai_cas_copy_state *state)
{
	struct fyai_cas_blob result = { 0 };
	unsigned char random[16];
	char temporary[sizeof(".tmp-") + sizeof(random) * 2];
	char name[FYAI_CAS_DIGEST_SIZE];
	int fd = -1, rc = -1, saved, attempt;
	bool created = false;

	/* A random temporary name: concurrent writers must not share a file. */
	for (attempt = 0; attempt < 16; attempt++) {
		if (RAND_bytes(random, sizeof(random)) != 1) {
			errno = EIO;
			return -1;
		}
		memcpy(temporary, ".tmp-", 5);
		fyai_cas_hex(temporary + 5, random, sizeof(random));
		fd = openat(directory_fd, temporary,
			    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd >= 0) {
			created = true;
			break;
		}
		if (errno != EEXIST)
			return -1;
	}
	if (!created)
		return -1;
	if (sized && (size > (uint64_t)INT64_MAX || size > SIZE_MAX)) {
		errno = EOVERFLOW;
		rc = -1;
	} else if (sized) {
		rc = cas_hash_range(source_fd, fd, 0, (off_t)size, &result, hasher, state);
	} else {
		rc = cas_copy_hash(source_fd, fd, &result, hasher, state);
	}
	if (!rc)
		rc = fchmod(fd, 0444);
	if (!rc && (!state || !state->defer_file_sync))
		rc = fsync(fd);
	saved = errno;
	if (close(fd) < 0 && !rc) {
		rc = -1;
		saved = errno;
	}
	errno = saved;
	if (rc)
		goto out;
	/*
	 * The directory is private; publication never replaces a digest name.
	 */
	fyai_cas_blob_hex(name, &result);
	rc = linkat(directory_fd, temporary, directory_fd, name, 0);
	if (rc && errno == EEXIST)
		rc = fyai_cas_verify_hasher(directory_fd, &result, hasher);
	if (rc)
		goto out;
	rc = unlinkat(directory_fd, temporary, 0);
	if (rc)
		goto out;
	created = false;
	if (!state || !state->defer_directory_sync)
		rc = fsync(directory_fd);
	if (!rc)
		*blob = result;
out:
	saved = errno;
	if (created)
		unlinkat(directory_fd, temporary, 0);
	errno = saved;
	return rc;
}

int fyai_cas_put_hasher_state(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			      struct fy_blake3_hasher *hasher, struct fyai_cas_copy_state *state)
{
	return cas_put(directory_fd, source_fd, false, 0, blob, hasher, state);
}

int fyai_cas_put_sized_state(int directory_fd, int source_fd, uint64_t size,
			     struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			     struct fyai_cas_copy_state *state)
{
	return cas_put(directory_fd, source_fd, true, size, blob, hasher, state);
}

int fyai_cas_put_hasher(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			struct fy_blake3_hasher *hasher)
{
	return fyai_cas_put_hasher_state(directory_fd, source_fd, blob, hasher, NULL);
}

int fyai_cas_put(int directory_fd, int source_fd, struct fyai_cas_blob *blob)
{
	struct fy_blake3_hasher_cfg cfg = { 0 };
	struct fy_blake3_hasher *hasher;
	int rc, saved;

	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		return -1;
	}
	rc = fyai_cas_put_hasher(directory_fd, source_fd, blob, hasher);
	saved = errno;
	fy_blake3_hasher_destroy(hasher);
	errno = saved;
	return rc;
}

int fyai_cas_verify(int directory_fd, const struct fyai_cas_blob *blob)
{
	struct fy_blake3_hasher_cfg cfg = { 0 };
	struct fy_blake3_hasher *hasher;
	int rc, saved;

	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		return -1;
	}
	rc = fyai_cas_verify_hasher(directory_fd, blob, hasher);
	saved = errno;
	fy_blake3_hasher_destroy(hasher);
	errno = saved;
	return rc;
}

#else
int fyai_cas_clone_method(int source, int target, uint64_t size, struct fyai_cas_copy_state *state,
			  enum fyai_cas_method *method)
{
	(void)source;
	(void)target;
	(void)size;
	(void)state;
	(void)method;
	errno = ENOTSUP;
	return -1;
}

bool fyai_cas_borrowed_matches(const struct stat *st, const struct fyai_cas_blob *blob)
{
	(void)st;
	(void)blob;
	return true;
}

int fyai_cas_clone_state(int source, int target, uint64_t size, struct fyai_cas_copy_state *state)
{
	(void)source;
	(void)target;
	(void)size;
	(void)state;
	errno = ENOTSUP;
	return -1;
}
int fyai_cas_put_hasher_state(int fd, int source, struct fyai_cas_blob *blob,
			      struct fy_blake3_hasher *hasher, struct fyai_cas_copy_state *state)
{
	(void)fd;
	(void)source;
	(void)blob;
	(void)hasher;
	(void)state;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_hash_file(int fd, struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher)
{
	(void)fd;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}
int fyai_cas_link(int fd, int source, const char *path, const struct fyai_cas_blob *blob,
		  struct fy_blake3_hasher *hasher)
{
	(void)fd;
	(void)source;
	(void)path;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}
int fyai_cas_link_hashed(int fd, int source, const char *path, const struct fyai_cas_blob *blob,
			 struct fy_blake3_hasher *hasher, int hashed_fd)
{
	(void)fd;
	(void)source;
	(void)path;
	(void)blob;
	(void)hasher;
	(void)hashed_fd;
	errno = ENOTSUP;
	return -1;
}
int fyai_cas_link_hashed_state(int fd, int source, const char *path,
			       const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			       int hashed_fd, struct fyai_cas_copy_state *state)
{
	(void)fd;
	(void)source;
	(void)path;
	(void)blob;
	(void)hasher;
	(void)hashed_fd;
	(void)state;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_open(int fd, const struct fyai_cas_blob *blob)
{
	(void)fd;
	(void)blob;
	errno = ENOTSUP;
	return -1;
}
int fyai_cas_clone(int source, int target, uint64_t size)
{
	(void)source;
	(void)target;
	(void)size;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_copy(int source_fd, int target_fd, uint64_t size)
{
	(void)source_fd;
	(void)target_fd;
	(void)size;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_put(int directory_fd, int source_fd, struct fyai_cas_blob *blob)
{
	(void)directory_fd;
	(void)source_fd;
	(void)blob;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_put_hasher(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			struct fy_blake3_hasher *hasher)
{
	(void)directory_fd;
	(void)source_fd;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_verify(int directory_fd, const struct fyai_cas_blob *blob)
{
	(void)directory_fd;
	(void)blob;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_put_sized_state(int directory_fd, int source_fd, uint64_t size,
			     struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			     struct fyai_cas_copy_state *state)
{
	(void)directory_fd;
	(void)source_fd;
	(void)size;
	(void)blob;
	(void)hasher;
	(void)state;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_hash_file_sized(int fd, uint64_t size, struct fyai_cas_blob *blob,
			     struct fy_blake3_hasher *hasher)
{
	(void)fd;
	(void)size;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_verify_hasher(int directory_fd, const struct fyai_cas_blob *blob,
			   struct fy_blake3_hasher *hasher)
{
	(void)directory_fd;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}

int fyai_cas_verify_file(int fd, const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher)
{
	(void)fd;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}

#endif
