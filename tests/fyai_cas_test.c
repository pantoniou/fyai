/*
 * fyai_cas_test.c - unit tests for the content-addressed blob store
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
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libfyaml/libfyaml-blake3.h>

#include "fyai_cas.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(cas, digest_parse, cas_digest_parse)
FYAI_TEST_ENTRY(cas, linked_digest, cas_linked_digest)
FYAI_TEST_ENTRY(cas, owned_reuse, cas_owned_reuse)
FYAI_TEST_ENTRY(cas, mapped_capture, cas_mapped_capture)
FYAI_TEST_ENTRY(cas, publication, cas_publication)
FYAI_TEST_ENTRY(cas, corrupt_reuse, cas_corrupt_reuse)
FYAI_TEST_ENTRY(cas, sized, cas_sized)

int cas_digest_parse(void)
{
	static const char good[] = "000102030405060708090a0b0c0d0e0f"
				   "101112131415161718191a1b1c1d1e1f";
	unsigned char bytes[FYAI_CAS_HASH_SIZE], keep[FYAI_CAS_HASH_SIZE];
	char hex[FYAI_CAS_DIGEST_SIZE], bad[FYAI_CAS_DIGEST_SIZE], longer[FYAI_CAS_DIGEST_SIZE + 1];
	size_t i;
	int rc;

	rc = fyai_cas_digest_parse(bytes, good);
	FYAI_TCHECK(!rc);
	for (i = 0; i < sizeof(bytes); i++)
		FYAI_TCHECK(bytes[i] == i);
	fyai_cas_hex(hex, bytes, sizeof(bytes));
	FYAI_TCHECK(!strcmp(hex, good));
	memcpy(keep, bytes, sizeof(keep));
	memcpy(bad, good, sizeof(bad));
	bad[63] = 'G';
	rc = fyai_cas_digest_parse(bytes, bad);
	FYAI_TCHECK(rc < 0 && errno == EINVAL && !memcmp(bytes, keep, sizeof(keep)));
	memcpy(bad, good, sizeof(bad));
	bad[0] = 'A';
	rc = fyai_cas_digest_parse(bytes, bad);
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	rc = fyai_cas_digest_parse(bytes, "00");
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	snprintf(longer, sizeof(longer), "%s0", good);
	rc = fyai_cas_digest_parse(bytes, longer);
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	return 0;
}

#ifdef __linux__
int cas_linked_digest(void)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	struct fyai_cas_blob blob = { .borrowed = true };
	char path[] = "/tmp/fyai-cas-linked-XXXXXX", name[80], hex[FYAI_CAS_DIGEST_SIZE];
	int dir, source, other, rc;

	FYAI_TCHECK(mkdtemp(path));
	dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(dir >= 0);
	source = openat(dir, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(source >= 0 && write(source, "abc", 3) == 3);
	hasher = fy_blake3_hasher_create(&cfg);
	FYAI_TCHECK(hasher);
	FYAI_TCHECK(lseek(source, 0, SEEK_SET) == 0);
	rc = fyai_cas_hash_file(source, &blob, hasher);
	FYAI_TCHECK(!rc);
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, source);
	FYAI_TCHECK(!rc);
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, source);
	FYAI_TCHECK(!rc);
	fyai_cas_blob_hex(hex, &blob);
	snprintf(name, sizeof(name), "borrowed/%s", hex);
	FYAI_TCHECK(!unlinkat(dir, name, 0));
	other = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(other >= 0 && write(other, "bad", 3) == 3);
	close(other);
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, source);
	FYAI_TCHECK(rc < 0 && errno == EIO);
	FYAI_TCHECK(!unlinkat(dir, name, 0) && !unlinkat(dir, "source", 0));
	other = openat(dir, "source", O_WRONLY | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(other >= 0 && write(other, "bad", 3) == 3);
	close(other);
	/* A replaced pathname must not inherit the digest of the open
	 * descriptor. */
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, source);
	FYAI_TCHECK(rc < 0 && errno == EIO);
	FYAI_TCHECK(!unlinkat(dir, name, 0) && !unlinkat(dir, "source", 0));
	FYAI_TCHECK(!unlinkat(dir, "borrowed", AT_REMOVEDIR));
	close(source);
	close(dir);
	fy_blake3_hasher_destroy(hasher);
	FYAI_TCHECK(!rmdir(path));
	return 0;
}

/*
 * An owned object of the published form, a read-only file of the blob size, is
 * reused without a rehash. A writable one is not the published form and is
 * verified.
 */
int cas_owned_reuse(void)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	struct fyai_cas_blob blob = { 0 };
	char path[] = "/tmp/fyai-cas-owned-XXXXXX", name[FYAI_CAS_DIGEST_SIZE + 16];
	int dir, source, other, rc;

	FYAI_TCHECK(mkdtemp(path));
	dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(dir >= 0);
	source = openat(dir, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(source >= 0 && write(source, "abc", 3) == 3);
	hasher = fy_blake3_hasher_create(&cfg);
	FYAI_TCHECK(hasher);
	FYAI_TCHECK(lseek(source, 0, SEEK_SET) == 0);
	FYAI_TCHECK(!fyai_cas_hash_file(source, &blob, hasher));
	FYAI_TCHECK(!fyai_cas_name(name, sizeof(name), &blob));
	/* The stored object holds other bytes of the same size, and is read-only. */
	other = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL, 0444);
	FYAI_TCHECK(other >= 0 && write(other, "bad", 3) == 3);
	close(other);
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, source);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!fchmodat(dir, name, 0644, 0));
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, source);
	FYAI_TCHECK(rc < 0 && errno == EIO);
	/* Naming the file by path gives the same answers. */
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, FYAI_CAS_HASHED_BY_PATH);
	FYAI_TCHECK(rc < 0 && errno == EIO);
	FYAI_TCHECK(!unlinkat(dir, name, 0) && !linkat(dir, "source", dir, name, 0));
	FYAI_TCHECK(!fchmodat(dir, name, 0644, 0));
	rc = fyai_cas_link_hashed(dir, dir, "source", &blob, hasher, FYAI_CAS_HASHED_BY_PATH);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!unlinkat(dir, name, 0) && !unlinkat(dir, "source", 0));
	close(source);
	close(dir);
	fy_blake3_hasher_destroy(hasher);
	FYAI_TCHECK(!rmdir(path));
	return 0;
}

int cas_publication(void)
{
	char directory[] = "/tmp/fyai-cas-test-XXXXXX";
	struct fyai_cas_blob first, second;
	struct stat st;
	char *path, hex[FYAI_CAS_DIGEST_SIZE];
	int dir, source, rc;
	off_t offset;

	path = mkdtemp(directory);
	FYAI_TCHECK(path);
	dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(dir >= 0);
	source = openat(dir, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(source >= 0);
	rc = fyai_cas_put(dir, source, &first);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(first.size == 0);
	fyai_cas_blob_hex(hex, &first);
	FYAI_TCHECK(!strcmp(hex, "af1349b9f5f9a1a6a0404dea36dcc9499bcb"
				 "25c9adc112b7cc9a93cae41f3262"));
	rc = fyai_cas_verify(dir, &first);
	FYAI_TCHECK(!rc);
	rc = fstatat(dir, hex, &st, AT_SYMLINK_NOFOLLOW);
	FYAI_TCHECK(!rc && !(st.st_mode & 0222));
	rc = fyai_cas_put(dir, source, &second);
	FYAI_TCHECK(!rc && !memcmp(first.digest, second.digest, sizeof(first.digest)));
	rc = unlinkat(dir, hex, 0);
	FYAI_TCHECK(!rc);
	rc = write(source, "abc", 3);
	FYAI_TCHECK(rc == 3);
	offset = lseek(source, 0, SEEK_SET);
	FYAI_TCHECK(offset == 0);
	rc = fyai_cas_put(dir, source, &first);
	FYAI_TCHECK(!rc && first.size == 3);
	fyai_cas_blob_hex(hex, &first);
	FYAI_TCHECK(!strcmp(hex, "6437b3ac38465133ffb63b75273a8db548c5"
				 "58465d79db03fd359c6cd5bd9d85"));
	rc = unlinkat(dir, hex, 0);
	FYAI_TCHECK(!rc);
	close(source);
	unlinkat(dir, "source", 0);
	close(dir);
	rmdir(path);
	return 0;
}

int cas_corrupt_reuse(void)
{
	char directory[] = "/tmp/fyai-cas-test-XXXXXX";
	struct fyai_cas_blob blob, output;
	char hex[FYAI_CAS_DIGEST_SIZE];
	char *path;
	int dir, source, bad, rc;

	path = mkdtemp(directory);
	FYAI_TCHECK(path);
	dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(dir >= 0);
	source = openat(dir, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(source >= 0);
	rc = fyai_cas_put(dir, source, &blob);
	FYAI_TCHECK(!rc);
	fyai_cas_blob_hex(hex, &blob);
	rc = unlinkat(dir, hex, 0);
	FYAI_TCHECK(!rc);
	bad = openat(dir, hex, O_WRONLY | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(bad >= 0);
	rc = write(bad, "bad", 3);
	FYAI_TCHECK(rc == 3);
	rc = fchmod(bad, 0444);
	FYAI_TCHECK(!rc);
	close(bad);
	rc = fyai_cas_put(dir, source, &output);
	FYAI_TCHECK(rc < 0 && errno == EIO);
	rc = unlinkat(dir, hex, 0);
	FYAI_TCHECK(!rc);
	rc = symlinkat("source", dir, hex);
	FYAI_TCHECK(!rc);
	rc = fyai_cas_put(dir, source, &output);
	FYAI_TCHECK(rc < 0 && errno == ELOOP);
	memset(&output, 0, sizeof(output));
	rc = fyai_cas_verify(dir, &output);
	FYAI_TCHECK(rc < 0 && errno == ENOENT);
	unlinkat(dir, hex, 0);
	close(source);
	unlinkat(dir, "source", 0);
	close(dir);
	rmdir(path);
	return 0;
}

int cas_mapped_capture(void)
{
	char directory[] = "/tmp/fyai-cas-test-XXXXXX";
	struct fyai_cas_blob blob;
	const size_t size = 8 * 1024 * 1024 + 19;
	unsigned char *input;
	void *copy;
	char *path, hex[FYAI_CAS_DIGEST_SIZE];
	int dir, source, object, rc;
	size_t i;
	off_t offset;

	path = mkdtemp(directory);
	FYAI_TCHECK(path);
	dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(dir >= 0);
	source = openat(dir, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(source >= 0);
	rc = ftruncate(source, size);
	FYAI_TCHECK(!rc);
	input = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, source, 0);
	FYAI_TCHECK(input != MAP_FAILED);
	for (i = 0; i < size; i++)
		input[i] = (unsigned char)(i * 17 + i / 257);
	offset = lseek(source, 7, SEEK_SET);
	FYAI_TCHECK(offset == 7);
	rc = fyai_cas_put(dir, source, &blob);
	FYAI_TCHECK(!rc && blob.size == size - 7);
	offset = lseek(source, 0, SEEK_CUR);
	FYAI_TCHECK(offset == (off_t)size);
	rc = fyai_cas_verify(dir, &blob);
	FYAI_TCHECK(!rc);
	fyai_cas_blob_hex(hex, &blob);
	object = openat(dir, hex, O_RDONLY | O_NOFOLLOW);
	FYAI_TCHECK(object >= 0);
	copy = mmap(NULL, blob.size, PROT_READ, MAP_PRIVATE, object, 0);
	FYAI_TCHECK(copy != MAP_FAILED);
	FYAI_TCHECK(!memcmp(copy, input + 7, blob.size));
	munmap(copy, blob.size);
	munmap(input, size);
	close(object);
	close(source);
	unlinkat(dir, hex, 0);
	unlinkat(dir, "source", 0);
	close(dir);
	rmdir(path);
	return 0;
}

int cas_sized(void)
{
	/* Both sides of the read and the mapping threshold, and the empty file. */
	static const size_t sizes[] = { 0, 1, 3, 65535, 65536, 65537, 200000 };
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	struct fyai_cas_blob legacy, sized, hashed;
	char first[] = "/tmp/fyai-cas-sized-XXXXXX", second[] = "/tmp/fyai-cas-sized-XXXXXX";
	char hex[FYAI_CAS_DIGEST_SIZE];
	unsigned char *data;
	size_t i;
	int one, two, source, again, rc;

	hasher = fy_blake3_hasher_create(&cfg);
	FYAI_TCHECK(hasher);
	FYAI_TCHECK(mkdtemp(first) && mkdtemp(second));
	one = open(first, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	two = open(second, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(one >= 0 && two >= 0);
	data = malloc(200000);
	FYAI_TCHECK(data);
	for (i = 0; i < 200000; i++)
		data[i] = (unsigned char)(i * 31 + i / 251);
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		source = openat(one, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
		FYAI_TCHECK(source >= 0);
		FYAI_TCHECK(pwrite(source, data, sizes[i], 0) == (ssize_t)sizes[i]);
		FYAI_TCHECK(lseek(source, 0, SEEK_SET) == 0);
		rc = fyai_cas_put(one, source, &legacy);
		FYAI_TCHECK(!rc && legacy.size == sizes[i]);
		/* A fresh descriptor at offset 0: no stat and no seek by the callee. */
		again = openat(one, "source", O_RDONLY | O_CLOEXEC);
		FYAI_TCHECK(again >= 0);
		rc = fyai_cas_put_sized_state(two, again, sizes[i], &sized, hasher, NULL);
		FYAI_TCHECK(!rc && sized.size == sizes[i]);
		FYAI_TCHECK(!memcmp(legacy.digest, sized.digest, sizeof(legacy.digest)));
		FYAI_TCHECK(!fyai_cas_verify(two, &sized));
		close(again);
		again = openat(one, "source", O_RDONLY | O_CLOEXEC);
		FYAI_TCHECK(again >= 0);
		rc = fyai_cas_hash_file_sized(again, sizes[i], &hashed, hasher);
		FYAI_TCHECK(!rc && hashed.size == sizes[i]);
		FYAI_TCHECK(!memcmp(legacy.digest, hashed.digest, sizeof(legacy.digest)));
		close(again);
		/* The prefix of a longer file is hashed as the file of that size. */
		if (sizes[i] > 1) {
			again = openat(one, "source", O_RDONLY | O_CLOEXEC);
			FYAI_TCHECK(again >= 0);
			rc = fyai_cas_hash_file_sized(again, sizes[i] - 1, &hashed, hasher);
			FYAI_TCHECK(!rc && hashed.size == sizes[i] - 1);
			FYAI_TCHECK(memcmp(legacy.digest, hashed.digest, sizeof(legacy.digest)));
			close(again);
		}
		fyai_cas_blob_hex(hex, &legacy);
		FYAI_TCHECK(!unlinkat(one, hex, 0) && !unlinkat(two, hex, 0));
		FYAI_TCHECK(!unlinkat(one, "source", 0));
		close(source);
	}
	free(data);
	close(one);
	close(two);
	fy_blake3_hasher_destroy(hasher);
	FYAI_TCHECK(!rmdir(first) && !rmdir(second));
	return 0;
}

#else
int cas_owned_reuse(void)
{
	return 0;
}

int cas_sized(void)
{
	return 0;
}

int cas_linked_digest(void)
{
	return 0;
}
int cas_publication(void)
{
	return 0;
}
int cas_corrupt_reuse(void)
{
	return 0;
}
int cas_mapped_capture(void)
{
	return 0;
}
#endif
