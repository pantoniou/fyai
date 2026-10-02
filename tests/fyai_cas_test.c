/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fyai_cas.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(cas, mapped_capture, cas_mapped_capture)
FYAI_TEST_ENTRY(cas, publication, cas_publication)
FYAI_TEST_ENTRY(cas, corrupt_reuse, cas_corrupt_reuse)

#ifdef __linux__
int cas_publication(void)
{
	char directory[] = "/tmp/fyai-cas-test-XXXXXX";
	struct fyai_cas_blob first, second;
	struct stat st;
	char *path;
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
	FYAI_TCHECK(!strcmp(first.digest,
		"af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"));
	rc = fyai_cas_verify(dir, &first);
	FYAI_TCHECK(!rc);
	rc = fstatat(dir, first.digest, &st, AT_SYMLINK_NOFOLLOW);
	FYAI_TCHECK(!rc && !(st.st_mode & 0222));
	rc = fyai_cas_put(dir, source, &second);
	FYAI_TCHECK(!rc && !strcmp(first.digest, second.digest));
	rc = unlinkat(dir, first.digest, 0);
	FYAI_TCHECK(!rc);
	rc = write(source, "abc", 3);
	FYAI_TCHECK(rc == 3);
	offset = lseek(source, 0, SEEK_SET);
	FYAI_TCHECK(offset == 0);
	rc = fyai_cas_put(dir, source, &first);
	FYAI_TCHECK(!rc && first.size == 3);
	FYAI_TCHECK(!strcmp(first.digest,
		"6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"));
	rc = unlinkat(dir, first.digest, 0);
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
	rc = unlinkat(dir, blob.digest, 0);
	FYAI_TCHECK(!rc);
	bad = openat(dir, blob.digest, O_WRONLY | O_CREAT | O_EXCL, 0600);
	FYAI_TCHECK(bad >= 0);
	rc = write(bad, "bad", 3);
	FYAI_TCHECK(rc == 3);
	rc = fchmod(bad, 0444);
	FYAI_TCHECK(!rc);
	close(bad);
	rc = fyai_cas_put(dir, source, &output);
	FYAI_TCHECK(rc < 0 && errno == EIO);
	rc = unlinkat(dir, blob.digest, 0);
	FYAI_TCHECK(!rc);
	rc = symlinkat("source", dir, blob.digest);
	FYAI_TCHECK(!rc);
	rc = fyai_cas_put(dir, source, &output);
	FYAI_TCHECK(rc < 0 && errno == ELOOP);
	memset(&output, '/', sizeof(output));
	rc = fyai_cas_verify(dir, &output);
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	unlinkat(dir, blob.digest, 0);
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
	char *path;
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
	object = openat(dir, blob.digest, O_RDONLY | O_NOFOLLOW);
	FYAI_TCHECK(object >= 0);
	copy = mmap(NULL, blob.size, PROT_READ, MAP_PRIVATE, object, 0);
	FYAI_TCHECK(copy != MAP_FAILED);
	FYAI_TCHECK(!memcmp(copy, input + 7, blob.size));
	munmap(copy, blob.size);
	munmap(input, size);
	close(object);
	close(source);
	unlinkat(dir, blob.digest, 0);
	unlinkat(dir, "source", 0);
	close(dir);
	rmdir(path);
	return 0;
}

#else
int cas_publication(void) { return 0; }
int cas_corrupt_reuse(void) { return 0; }
int cas_mapped_capture(void) { return 0; }
#endif
