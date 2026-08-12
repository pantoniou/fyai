/*
 * fyai_read_test.c - tests for the bounded file read
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "utils.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(read, limit_truncates, read_limit_truncates)
FYAI_TEST_ENTRY(read, limit_reports_full_size, read_limit_reports_full_size)
FYAI_TEST_ENTRY(read, limit_spares_small, read_limit_spares_small)
FYAI_TEST_ENTRY(read, limit_exact_fit, read_limit_exact_fit)
FYAI_TEST_ENTRY(read, zero_limit_reads_all, read_zero_limit_reads_all)
FYAI_TEST_ENTRY(read, binary_reports_full_size, read_binary_reports_full_size)

/* Write @len bytes of filler to a scratch file and return its path. */
static const char *read_make_file(size_t len, const char *fill)
{
	static char made[] = "/tmp/fyai-read-test-XXXXXX";
	size_t i;
	FILE *fp;
	int fd;

	strcpy(made, "/tmp/fyai-read-test-XXXXXX");
	fd = mkstemp(made);
	if (fd < 0)
		return NULL;
	fp = fdopen(fd, "wb");
	if (!fp) {
		close(fd);
		return NULL;
	}
	for (i = 0; i < len; i++)
		fputc(fill[i % strlen(fill)], fp);
	fclose(fp);
	return made;
}

int read_limit_truncates(void)
{
	const char *path;
	size_t full;
	char *text;

	path = read_make_file(10000, "abcdefgh");
	FYAI_TCHECK(path != NULL);

	text = read_text_file_limited(path, 100, &full);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) == 100);
	FYAI_TCHECK(full == 10000);

	free(text);
	unlink(path);
	printf("ok - a bounded read keeps the first max_bytes bytes\n");
	return 0;
}

int read_limit_reports_full_size(void)
{
	const char *path;
	size_t full;
	char *text;

	/*
	 * The complete size must come from the stream, not from the bytes
	 * kept, so the caller can report how much it did not show.
	 */
	path = read_make_file(70001, "xy");
	FYAI_TCHECK(path != NULL);

	text = read_text_file_limited(path, 4096, &full);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) == 4096);
	FYAI_TCHECK(full == 70001);

	free(text);
	unlink(path);
	printf("ok - a bounded read reports the complete file size\n");
	return 0;
}

int read_limit_spares_small(void)
{
	const char *path;
	size_t full;
	char *text;

	path = read_make_file(37, "hello ");
	FYAI_TCHECK(path != NULL);

	text = read_text_file_limited(path, 4096, &full);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) == 37);
	FYAI_TCHECK(full == 37);

	free(text);
	unlink(path);
	printf("ok - a file under the limit is read complete\n");
	return 0;
}

int read_limit_exact_fit(void)
{
	const char *path;
	size_t full;
	char *text;

	/* A file of exactly max_bytes is complete, not truncated. */
	path = read_make_file(256, "z");
	FYAI_TCHECK(path != NULL);

	text = read_text_file_limited(path, 256, &full);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) == 256);
	FYAI_TCHECK(full == 256);

	free(text);
	unlink(path);
	printf("ok - a file of exactly max_bytes is not truncated\n");
	return 0;
}

int read_zero_limit_reads_all(void)
{
	const char *path;
	size_t full;
	char *text;

	path = read_make_file(9000, "0123456789");
	FYAI_TCHECK(path != NULL);

	text = read_text_file_limited(path, 0, &full);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) == 9000);
	FYAI_TCHECK(full == 9000);

	/* The unbounded wrapper must keep its old behaviour. */
	free(text);
	text = read_text_file(path);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) == 9000);

	free(text);
	unlink(path);
	printf("ok - a zero limit reads the complete file\n");
	return 0;
}

int read_binary_reports_full_size(void)
{
	char buf[3000];
	const char *path;
	size_t full;
	char *text;
	FILE *fp;
	char made[] = "/tmp/fyai-read-bin-XXXXXX";
	int fd;

	memset(buf, 0, sizeof(buf));
	fd = mkstemp(made);
	FYAI_TCHECK(fd >= 0);
	fp = fdopen(fd, "wb");
	FYAI_TCHECK(fp != NULL);
	fwrite(buf, 1, sizeof(buf), fp);
	fclose(fp);
	path = made;

	/*
	 * A binary file reports a size rather than content, so it is never
	 * truncated: the size it reports is the complete one.
	 */
	text = read_text_file_limited(path, 512, &full);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(full == 3000);
	FYAI_TCHECK(strstr(text, "3000") != NULL);
	FYAI_TCHECK(strlen(text) < 512);

	free(text);
	unlink(path);
	printf("ok - a bounded binary read reports the complete size\n");
	return 0;
}
