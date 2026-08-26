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
#include "fyai_test_scratch.h"

FYAI_TEST_ENTRY(read, limit_truncates, read_limit_truncates)
FYAI_TEST_ENTRY(read, limit_reports_full_size, read_limit_reports_full_size)
FYAI_TEST_ENTRY(read, limit_spares_small, read_limit_spares_small)
FYAI_TEST_ENTRY(read, limit_exact_fit, read_limit_exact_fit)
FYAI_TEST_ENTRY(read, zero_limit_reads_all, read_zero_limit_reads_all)
FYAI_TEST_ENTRY(read, binary_reports_full_size, read_binary_reports_full_size)

/* Write @len bytes of filler to a scratch file and return its path. */
static const char *read_make_file(size_t len, const char *fill)
{
	const char *made;
	size_t i;
	FILE *fp;

	made = fyai_test_scratch_file("read-test", NULL);
	if (!made)
		return NULL;
	fp = fopen(made, "wb");
	if (!fp)
		return NULL;
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
	const char *made;

	memset(buf, 0, sizeof(buf));
	made = fyai_test_scratch_file("read-bin", NULL);
	FYAI_TCHECK(made != NULL);
	fp = fopen(made, "wb");
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

FYAI_TEST_ENTRY(read, window_offset_limit, read_window_offset_limit)
FYAI_TEST_ENTRY(read, window_pages_whole_file, read_window_pages_whole_file)
FYAI_TEST_ENTRY(read, window_past_end, read_window_past_end)
FYAI_TEST_ENTRY(read, window_byte_cap, read_window_byte_cap)
FYAI_TEST_ENTRY(read, window_byte_offset, read_window_byte_offset)

/* Write @lines numbered lines and return the scratch path. */
static const char *read_make_lines(int lines)
{
	const char *made;
	FILE *fp;
	int i;

	made = fyai_test_scratch_file("read-lines", NULL);
	if (!made)
		return NULL;
	fp = fopen(made, "wb");
	if (!fp)
		return NULL;
	for (i = 1; i <= lines; i++)
		fprintf(fp, "line %d\n", i);
	fclose(fp);
	return made;
}

int read_window_offset_limit(void)
{
	struct read_text_info info;
	const char *path;
	char *text;

	path = read_make_lines(100);
	FYAI_TCHECK(path != NULL);

	text = read_text_file_window(path, 10, -1, 5, 0, &info);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(!strcmp(text, "line 10\nline 11\nline 12\nline 13\nline 14\n"));
	FYAI_TCHECK(info.first_line == 10);
	FYAI_TCHECK(info.last_line == 14);
	FYAI_TCHECK(info.total_lines == 100);
	FYAI_TCHECK(!info.byte_capped);

	free(text);
	unlink(path);
	printf("ok - a line window returns exactly its lines\n");
	return 0;
}

int read_window_pages_whole_file(void)
{
	struct read_text_info info;
	const char *path;
	long long next;
	int rounds;
	char *text;

	/*
	 * Paging with the reported offset must cover every line exactly once
	 * and terminate: that is the contract the truncation note states.
	 */
	path = read_make_lines(250);
	FYAI_TCHECK(path != NULL);

	next = 1;
	rounds = 0;
	while (rounds < 100) {
		text = read_text_file_window(path, next, -1, 30, 0, &info);
		FYAI_TCHECK(text != NULL);
		if (!info.first_line) {
			free(text);
			break;
		}
		FYAI_TCHECK(info.first_line == next);
		next = info.last_line + 1;
		free(text);
		rounds++;
		if (info.last_line >= info.total_lines)
			break;
	}
	FYAI_TCHECK(next == 251);
	FYAI_TCHECK(rounds == 9);

	unlink(path);
	printf("ok - paging by the reported offset covers the file\n");
	return 0;
}

int read_window_past_end(void)
{
	struct read_text_info info;
	const char *path;
	char *text;

	path = read_make_lines(10);
	FYAI_TCHECK(path != NULL);

	/* Past the end is empty, not an error, and still reports the total. */
	text = read_text_file_window(path, 500, -1, 5, 0, &info);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(!*text);
	FYAI_TCHECK(info.first_line == 0);
	FYAI_TCHECK(info.total_lines == 10);

	free(text);
	unlink(path);
	printf("ok - a window past the end returns nothing and reports it\n");
	return 0;
}

int read_window_byte_cap(void)
{
	struct read_text_info info;
	const char *path;
	char *text;

	path = read_make_lines(1000);
	FYAI_TCHECK(path != NULL);

	/* The byte bound still applies inside a line window. */
	text = read_text_file_window(path, 1, -1, 900, 100, &info);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strlen(text) <= 100);
	FYAI_TCHECK(info.byte_capped);
	FYAI_TCHECK(info.first_line == 1);
	FYAI_TCHECK(info.last_line > 0 && info.last_line < 900);
	FYAI_TCHECK(info.total_lines == 1000);

	free(text);
	unlink(path);
	printf("ok - the byte bound applies inside a line window\n");
	return 0;
}

int read_window_byte_offset(void)
{
	struct read_text_info info;
	const char *path;
	char *text;

	path = read_make_lines(10);
	FYAI_TCHECK(path != NULL);
	text = read_text_file_window(path, 1, 5, 0, 12, &info);
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(!strcmp(text, "1\nline 2\nlin"));
	FYAI_TCHECK(info.first_byte == 5);
	FYAI_TCHECK(info.next_byte == 17);
	FYAI_TCHECK(info.byte_capped);

	free(text);
	unlink(path);
	printf("ok - a byte offset resumes at the requested byte\n");
	return 0;
}
