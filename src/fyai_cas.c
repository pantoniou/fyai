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
#include "fyai_cas.h"

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

static void cas_hex(char *out, const unsigned char *bytes, size_t count)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < count; i++) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 15];
	}
	out[count * 2] = '\0';
}

int fyai_cas_copy(int source_fd, int target_fd, uint64_t size)
{
	ssize_t copied;
	size_t count;
	bool fallback = false;

	while (size) {
		count = size > (1U << 30) ? (1U << 30) : (size_t)size;
		copied = fallback ? sendfile(target_fd, source_fd, NULL, count) :
				    copy_file_range(source_fd, NULL, target_fd, NULL, count, 0);
		if (copied < 0 && errno == EINTR)
			continue;
		if (copied < 0 && !fallback &&
		    (errno == EXDEV || errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL)) {
			fallback = true;
			continue;
		}
		if (copied <= 0) {
			if (!copied)
				errno = EAGAIN;
			return -1;
		}
		size -= (uint64_t)copied;
	}
	return 0;
}

int fyai_cas_clone_method(int source_fd, int target_fd, uint64_t size,
			  struct fyai_cas_copy_state *state, int *method)
{
	struct fyai_cas_copy_state local = { .backend = 0 };
	struct stat st;
	ssize_t copied;
	size_t count;
	int backend, expected, rc;
	bool probe = false;

	if (method)
		*method = 2;
	if (!size)
		return 0;
	if (!state)
		state = &local;
	for (;;) {
		backend = atomic_load_explicit(&state->backend, memory_order_acquire);
		if (backend > 0)
			break;
		if (!backend) {
			expected = 0;
			if (atomic_compare_exchange_strong_explicit(&state->backend, &expected, -1,
								    memory_order_acq_rel,
								    memory_order_acquire)) {
				probe = true;
				break;
			}
		}
		sched_yield();
	}
	if (probe || backend == 1) {
		rc = fstat(source_fd, &st);
		if (rc)
			goto failed;
		if ((uint64_t)st.st_size == size && lseek(source_fd, 0, SEEK_CUR) == 0 &&
		    lseek(target_fd, 0, SEEK_CUR) == 0) {
			do {
				rc = ioctl(target_fd, FICLONE, source_fd);
			} while (rc && errno == EINTR);
			if (!rc) {
				atomic_store_explicit(&state->backend, 1, memory_order_release);
				atomic_fetch_add_explicit(&state->reflinked_bytes, size,
							  memory_order_relaxed);
				if (method)
					*method = 1;
				if (lseek(source_fd, size, SEEK_SET) < 0 ||
				    lseek(target_fd, size, SEEK_SET) < 0)
					return -1;
				return 0;
			}
			if (errno != EOPNOTSUPP && errno != ENOTTY && errno != EXDEV &&
			    errno != EINVAL && errno != ENOSYS)
				goto failed;
		}
		backend = 2;
	}
	while (size) {
		count = size > (1U << 30) ? (1U << 30) : (size_t)size;
		copied = backend == 3 ? sendfile(target_fd, source_fd, NULL, count) :
					copy_file_range(source_fd, NULL, target_fd, NULL, count, 0);
		if (copied < 0 && errno == EINTR)
			continue;
		if (copied < 0 && backend != 3 &&
		    (errno == EXDEV || errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL)) {
			backend = 3;
			continue;
		}
		if (copied <= 0) {
			if (!copied)
				errno = EAGAIN;
			goto failed;
		}
		if (probe || backend == 3)
			atomic_store_explicit(&state->backend, backend, memory_order_release);
		probe = false;
		atomic_fetch_add_explicit(&state->copied_bytes, copied, memory_order_relaxed);
		if (method)
			*method = backend;
		size -= (uint64_t)copied;
	}
	return 0;
failed:
	if (probe)
		atomic_store_explicit(&state->backend, 0, memory_order_release);
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

static int cas_name(char *path, size_t size, const struct fyai_cas_blob *blob)
{
	size_t i, prefix = blob->borrowed ? 9 : 0;

	if (size < prefix + FYAI_CAS_DIGEST_SIZE) {
		errno = ENAMETOOLONG;
		return -1;
	}
	for (i = 0; i < 64; i++)
		if (!((blob->digest[i] >= '0' && blob->digest[i] <= '9') ||
		      (blob->digest[i] >= 'a' && blob->digest[i] <= 'f'))) {
			errno = EINVAL;
			return -1;
		}
	if (blob->digest[64]) {
		errno = EINVAL;
		return -1;
	}
	if (prefix)
		memcpy(path, "borrowed/", prefix);
	memcpy(path + prefix, blob->digest, FYAI_CAS_DIGEST_SIZE);
	return 0;
}

int fyai_cas_open(int directory_fd, const struct fyai_cas_blob *blob)
{
	char path[9 + FYAI_CAS_DIGEST_SIZE];
	int rc;

	rc = cas_name(path, sizeof(path), blob);
	if (rc)
		return -1;
	return openat(directory_fd, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
}

static int cas_copy_hash(int source_fd, int target_fd, struct fyai_cas_blob *blob,
			 struct fy_blake3_hasher *md, struct fyai_cas_copy_state *state)
{
	const uint8_t *digest;
	struct stat st;
	void *source = MAP_FAILED, *captured = MAP_FAILED;
	const unsigned char *data = NULL;
	off_t start;
	size_t size;
	int rc, saved;

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
	size = start < st.st_size ? (size_t)(st.st_size - start) : 0;
	rc = -1;
	if (target_fd >= 0) {
		rc = fyai_cas_clone_state(source_fd, target_fd, size, state);
		if (rc)
			goto out;
		rc = -1;
		/* The private capture cannot be truncated by a host source
		 * writer. */
		if (size) {
			captured = mmap(NULL, size, PROT_READ, MAP_PRIVATE, target_fd, 0);
			if (captured == MAP_FAILED)
				goto out;
			data = captured;
		}
	} else if (size) {
		/* Verification maps an immutable published CAS object. */
		source = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, source_fd, 0);
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
	cas_hex(blob->digest, digest, FY_BLAKE3_OUT_LEN);
	blob->size = size;
	if (size && lseek(source_fd, st.st_size, SEEK_SET) < 0)
		goto out;
	rc = 0;
out:
	saved = errno;
	if (captured != MAP_FAILED)
		munmap(captured, size);
	if (source != MAP_FAILED)
		munmap(source, (size_t)st.st_size);
	errno = saved;
	return rc;
}

int fyai_cas_hash_file(int fd, struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher)
{
	return cas_copy_hash(fd, -1, blob, hasher, NULL);
}

int fyai_cas_link_hashed_state(int directory_fd, int source_directory, const char *path,
			       const struct fyai_cas_blob *blob, struct fy_blake3_hasher *hasher,
			       int hashed_fd, struct fyai_cas_copy_state *state)
{
	struct stat source, object;
	char name[9 + FYAI_CAS_DIGEST_SIZE];
	int rc, borrowed, fd, saved;

	rc = cas_name(name, sizeof(name), blob);
	if (rc)
		return -1;
	if (blob->borrowed) {
		rc = mkdirat(directory_fd, "borrowed", 0700);
		if (rc && errno != EEXIST)
			return -1;
	}
	rc = linkat(source_directory, path, directory_fd, name, 0);
	if ((!rc && blob->borrowed) || (rc && errno == EEXIST)) {
		if (hashed_fd >= 0) {
			rc = fstat(hashed_fd, &source);
			if (!rc)
				rc = fstatat(directory_fd, name, &object, AT_SYMLINK_NOFOLLOW);
			if (rc)
				return -1;
			if (S_ISREG(object.st_mode) && source.st_dev == object.st_dev &&
			    source.st_ino == object.st_ino &&
			    (uint64_t)object.st_size == blob->size)
				rc = 0;
			else
				rc = fyai_cas_verify_hasher(directory_fd, blob, hasher);
		} else
			rc = fyai_cas_verify_hasher(directory_fd, blob, hasher);
	}
	if (!rc && blob->borrowed && (!state || !state->defer_file_sync)) {
		fd = fyai_cas_open(directory_fd, blob);
		if (fd < 0)
			return -1;
		rc = fsync(fd);
		saved = errno;
		close(fd);
		errno = saved;
		if (rc)
			return -1;
	}
	if (!rc && state && state->defer_directory_sync)
		return 0;
	if (!rc && blob->borrowed) {
		borrowed = openat(directory_fd, "borrowed",
				  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (borrowed < 0)
			return -1;
		rc = fsync(borrowed);
		close(borrowed);
	}
	if (!rc)
		rc = fsync(directory_fd);
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
	if (!rc && (actual.size != blob->size || strcmp(actual.digest, blob->digest))) {
		errno = EIO;
		return -1;
	}
	return rc;
}

int fyai_cas_verify_hasher(int directory_fd, const struct fyai_cas_blob *blob,
			   struct fy_blake3_hasher *hasher)
{
	struct fyai_cas_blob actual;
	struct stat st;
	size_t i;
	int fd, rc, saved;

	for (i = 0; i < FYAI_CAS_DIGEST_SIZE - 1; i++) {
		if (!((blob->digest[i] >= '0' && blob->digest[i] <= '9') ||
		      (blob->digest[i] >= 'a' && blob->digest[i] <= 'f'))) {
			errno = EINVAL;
			return -1;
		}
	}
	if (blob->digest[i]) {
		errno = EINVAL;
		return -1;
	}
	fd = fyai_cas_open(directory_fd, blob);
	if (fd < 0)
		return -1;
	rc = fstat(fd, &st);
	if (!rc && !S_ISREG(st.st_mode)) {
		errno = EINVAL;
		rc = -1;
	}
	if (!rc && blob->borrowed && blob->source_inode &&
	    ((uint64_t)st.st_ino != blob->source_inode ||
	     (uint64_t)st.st_dev != blob->source_device ||
	     (st.st_mode & 07777) != blob->source_mode || st.st_uid != blob->source_uid ||
	     st.st_gid != blob->source_gid)) {
		errno = EIO;
		rc = -1;
	}
	if (!rc)
		rc = cas_copy_hash(fd, -1, &actual, hasher, NULL);
	if (!rc && (actual.size != blob->size || strcmp(actual.digest, blob->digest))) {
		errno = EIO;
		rc = -1;
	}
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

int fyai_cas_put_hasher_state(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			      struct fy_blake3_hasher *hasher, struct fyai_cas_copy_state *state)
{
	struct fyai_cas_blob result = { 0 };
	unsigned char random[16];
	char temporary[sizeof(".tmp-") + sizeof(random) * 2];
	int fd = -1, rc = -1, saved, attempt;
	bool created = false;

	for (attempt = 0; attempt < 16; attempt++) {
		if (RAND_bytes(random, sizeof(random)) != 1) {
			errno = EIO;
			return -1;
		}
		memcpy(temporary, ".tmp-", 5);
		cas_hex(temporary + 5, random, sizeof(random));
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
	rc = cas_copy_hash(source_fd, fd, &result, hasher, state);
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
	rc = linkat(directory_fd, temporary, directory_fd, result.digest, 0);
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
			  int *method)
{
	(void)source;
	(void)target;
	(void)size;
	(void)state;
	(void)method;
	errno = ENOTSUP;
	return -1;
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
