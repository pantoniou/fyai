/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include "fyai_cas.h"

#ifdef __linux__
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
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

static int cas_copy_hash(int source_fd, int target_fd, struct fyai_cas_blob *blob,
			 struct fy_blake3_hasher *md)
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
		rc = fyai_cas_copy(source_fd, target_fd, size);
		if (rc)
			goto out;
		rc = -1;
		/* The private capture cannot be truncated by a host source writer. */
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

int fyai_cas_verify_file(int fd, const struct fyai_cas_blob *blob,
			 struct fy_blake3_hasher *hasher)
{
	struct fyai_cas_blob actual;
	int rc;

	rc = cas_copy_hash(fd, -1, &actual, hasher);
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
	fd = openat(directory_fd, blob->digest, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -1;
	rc = fstat(fd, &st);
	if (!rc && (!S_ISREG(st.st_mode) || (st.st_mode & 0222))) {
		errno = EINVAL;
		rc = -1;
	}
	if (!rc)
		rc = cas_copy_hash(fd, -1, &actual, hasher);
	if (!rc && (actual.size != blob->size || strcmp(actual.digest, blob->digest))) {
		errno = EIO;
		rc = -1;
	}
	if (!rc)
		rc = fsync(fd);
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

int fyai_cas_put_hasher(int directory_fd, int source_fd, struct fyai_cas_blob *blob,
			struct fy_blake3_hasher *hasher)
{
	struct fyai_cas_blob result;
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
	rc = cas_copy_hash(source_fd, fd, &result, hasher);
	if (!rc)
		rc = fchmod(fd, 0444);
	if (!rc)
		rc = fsync(fd);
	saved = errno;
	if (close(fd) < 0 && !rc) {
		rc = -1;
		saved = errno;
	}
	errno = saved;
	if (rc)
		goto out;
	/* The directory is private; publication never replaces a digest name. */
	rc = linkat(directory_fd, temporary, directory_fd, result.digest, 0);
	if (rc && errno == EEXIST)
		rc = fyai_cas_verify_hasher(directory_fd, &result, hasher);
	if (rc)
		goto out;
	rc = unlinkat(directory_fd, temporary, 0);
	if (rc)
		goto out;
	created = false;
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

int fyai_cas_verify_file(int fd, const struct fyai_cas_blob *blob,
		       struct fy_blake3_hasher *hasher)
{
	(void)fd;
	(void)blob;
	(void)hasher;
	errno = ENOTSUP;
	return -1;
}

#endif
