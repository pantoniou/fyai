/*
 * fyai_buf_test.c - unit tests for the growing byte buffer
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "fyai_buf.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(buf, append_keeps_a_nul, buf_append)
FYAI_TEST_ENTRY(buf, extend_counts_the_bytes, buf_extend)
FYAI_TEST_ENTRY(buf, a_queue_is_consumed_from_the_front, buf_queue)
FYAI_TEST_ENTRY(buf, trim_stops_at_the_front, buf_trim)

int buf_append(void)
{
	struct fyai_buf b = { 0 };

	FYAI_TCHECK(!fyai_buf_pending(&b));
	FYAI_TCHECK(!fyai_buf_append(&b, "ab"));
	FYAI_TCHECK(!fyai_buf_append_data(&b, "cd", 2));
	FYAI_TCHECK(!fyai_buf_append_line(&b, "ef", 2));
	FYAI_TCHECK(!strcmp(b.data, "abcdef\n") && fyai_buf_pending(&b) == 7);
	fyai_buf_free(&b);
	FYAI_TCHECK(!b.data && !b.len && !b.cap && !b.off);
	return 0;
}

int buf_extend(void)
{
	struct fyai_buf b = { 0 };
	char *at;
	size_t i;

	at = fyai_buf_extend(&b, 3);
	FYAI_TCHECK(at);
	memcpy(at, "xyz", 3);
	FYAI_TCHECK(fyai_buf_pending(&b) == 3 && !strcmp(b.data, "xyz"));
	/* The buffer grows past its first size and keeps what it holds. */
	for (i = 0; i < 5000; i++)
		FYAI_TCHECK(!fyai_buf_append_data(&b, "q", 1));
	FYAI_TCHECK(fyai_buf_pending(&b) == 5003 && !memcmp(b.data, "xyz", 3));
	fyai_buf_free(&b);
	return 0;
}

int buf_queue(void)
{
	struct fyai_buf b = { 0 };

	FYAI_TCHECK(!fyai_buf_append(&b, "abcde"));
	fyai_buf_consume(&b, 2);
	FYAI_TCHECK(fyai_buf_pending(&b) == 3 && !memcmp(b.data + b.off, "cde", 3));
	FYAI_TCHECK(!fyai_buf_append(&b, "fg"));
	FYAI_TCHECK(fyai_buf_pending(&b) == 5 && !memcmp(b.data + b.off, "cdefg", 5));
	/* A queue that was read to its end starts again at its front. */
	fyai_buf_consume(&b, 5);
	FYAI_TCHECK(!fyai_buf_pending(&b) && !b.off && !b.len);
	FYAI_TCHECK(!fyai_buf_append(&b, "h"));
	FYAI_TCHECK(b.off == 0 && !strcmp(b.data, "h"));
	fyai_buf_free(&b);
	return 0;
}

int buf_trim(void)
{
	struct fyai_buf b = { 0 };

	FYAI_TCHECK(!fyai_buf_append(&b, "ab\r\n\n"));
	fyai_buf_trim(&b);
	FYAI_TCHECK(!strcmp(b.data, "ab"));
	/* The bytes already consumed are not content to trim. */
	fyai_buf_consume(&b, 1);
	fyai_buf_trim(&b);
	FYAI_TCHECK(fyai_buf_pending(&b) == 1);
	fyai_buf_free(&b);
	return 0;
}
