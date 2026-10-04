/*
 * fyai_wire_test.c - unit tests for the binary encoding and the scratch arena
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdalign.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "fyai_scratch.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "fyai_wire.h"

FYAI_TEST_ENTRY(wire, roundtrip, wire_roundtrip)
FYAI_TEST_ENTRY(wire, large_frame, wire_large_frame)
FYAI_TEST_ENTRY(wire, damaged_frames, wire_damaged_frames)
FYAI_TEST_ENTRY(wire, limits, wire_limits)
FYAI_TEST_ENTRY(scratch, arena, scratch_arena)

static struct fy_generic_builder *wire_builder(void)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };

	return fy_generic_builder_create(&cfg);
}

/* Write the value through a temporary file and return its bytes. */
static unsigned char *wire_bytes(fy_generic value, size_t *length)
{
	struct fyai_wire_writer counter, writer;
	unsigned char *bytes;
	FILE *file;
	int rc;

	fyai_wire_writer_init(&counter, -1);
	rc = fyai_wire_put_value(&counter, value);
	if (rc)
		return NULL;
	file = tmpfile();
	if (!file)
		return NULL;
	fyai_wire_writer_init(&writer, fileno(file));
	rc = fyai_wire_put_value(&writer, value);
	if (!rc)
		rc = fyai_wire_flush(&writer);
	*length = counter.total;
	bytes = malloc(*length + 1);
	if (rc || !bytes || writer.total != counter.total ||
	    pread(fileno(file), bytes, *length, 0) != (ssize_t)*length) {
		free(bytes);
		bytes = NULL;
	}
	fclose(file);
	return bytes;
}

int wire_roundtrip(void)
{
	static const char digest[] = "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";
	struct fy_generic_builder *gb;
	struct fyai_scratch scratch = { 0 };
	struct fyai_wire_reader reader;
	fy_generic value, decoded, entries;
	unsigned char *bytes;
	size_t length;
	int rc;

	gb = wire_builder();
	FYAI_TCHECK(gb);
	rc = fyai_scratch_open(&scratch);
	FYAI_TCHECK(!rc);
	entries = fy_sequence(gb, fy_mapping(gb, "name_hex", "2e676974", "kind", "directory", "digest", digest),
			      fy_mapping(gb, "name_hex", "0", "kind", "file"));
	value = fy_mapping(gb, "version", 2LL, "digest", digest, "odd_hex", "abc", "upper_hex", "ABCD",
			   "short", "a", "empty", "", "text", "caf\xc3\xa9 \"quoted\"\n", "entries",
			   entries, "negative", -5LL, "big", (long long)LLONG_MAX, "small",
			   (long long)LLONG_MIN, "yes", true, "no", false, "nothing", fy_null,
			   "none", fy_sequence(gb), "table", fy_mapping(gb));
	FYAI_TCHECK(fy_is_mapping(value));
	bytes = wire_bytes(value, &length);
	FYAI_TCHECK(bytes && length);
	fyai_wire_reader_init(&reader, bytes, length);
	rc = fyai_wire_get_value(&reader, gb, &scratch, &decoded);
	FYAI_TCHECK(!rc && reader.position == reader.end);
	FYAI_TCHECK(fy_equal(value, decoded));
	FYAI_TCHECK(!strcmp(fy_get(decoded, "digest", ""), digest));
	FYAI_TCHECK(!strcmp(fy_get(decoded, "odd_hex", ""), "abc"));
	FYAI_TCHECK(!strcmp(fy_get(decoded, "upper_hex", ""), "ABCD"));
	FYAI_TCHECK(fy_get(decoded, "big", 0LL) == LLONG_MAX && fy_get(decoded, "small", 0LL) == LLONG_MIN);
	/* A digest takes 32 bytes, not 64, and a key order survives. */
	{
		fy_generic digests = fy_mapping(gb, digest, 1LL);
		unsigned char *small;
		size_t small_length;

		small = wire_bytes(digests, &small_length);
		FYAI_TCHECK(small && small_length < 1 + 1 + 1 + 1 + 32 + 2 + 1);
		free(small);
	}
	free(bytes);
	fyai_scratch_close(&scratch);
	fy_generic_builder_destroy(gb);
	return 0;
}

int wire_large_frame(void)
{
	struct fy_generic_builder *gb;
	struct fyai_scratch scratch = { 0 };
	struct fyai_wire_reader reader;
	fy_generic *items, value, decoded;
	unsigned char *bytes;
	size_t i, length, count = 30000;
	char text[64];
	int rc;

	gb = wire_builder();
	FYAI_TCHECK(gb);
	rc = fyai_scratch_open(&scratch);
	FYAI_TCHECK(!rc);
	items = malloc(count * sizeof(*items));
	FYAI_TCHECK(items);
	for (i = 0; i < count; i++) {
		snprintf(text, sizeof(text), "entry-%zu-%0*zx", i, (int)(i % 17) * 2, i);
		items[i] = fy_value(gb, text);
	}
	value = fy_gb_sequence_create(gb, count, items);
	free(items);
	FYAI_TCHECK(fy_is_sequence(value));
	bytes = wire_bytes(value, &length);
	/* The frame spans many buffers of the writer. */
	FYAI_TCHECK(bytes && length > 4 * sizeof(((struct fyai_wire_writer *)0)->buffer));
	fyai_wire_reader_init(&reader, bytes, length);
	rc = fyai_wire_get_value(&reader, gb, &scratch, &decoded);
	FYAI_TCHECK(!rc && reader.position == reader.end && fy_equal(value, decoded));
	free(bytes);
	fyai_scratch_close(&scratch);
	fy_generic_builder_destroy(gb);
	return 0;
}

int wire_damaged_frames(void)
{
	struct fy_generic_builder *gb;
	struct fyai_scratch scratch = { 0 };
	struct fyai_wire_reader reader;
	fy_generic value, decoded;
	unsigned char *bytes, copy[16];
	size_t length, cut;
	int rc;

	gb = wire_builder();
	FYAI_TCHECK(gb);
	rc = fyai_scratch_open(&scratch);
	FYAI_TCHECK(!rc);
	value = fy_mapping(gb, "a", fy_sequence(gb, "one", "6162", 3LL), "b", fy_mapping(gb, "c", true));
	bytes = wire_bytes(value, &length);
	FYAI_TCHECK(bytes);
	/* Every proper prefix of a frame is refused. */
	for (cut = 0; cut < length; cut++) {
		fyai_wire_reader_init(&reader, bytes, cut);
		errno = 0;
		rc = fyai_wire_get_value(&reader, gb, &scratch, &decoded);
		FYAI_TCHECK(rc < 0 && errno == EBADMSG);
	}
	/* An unknown tag, and a count that exceeds the bytes that remain. */
	copy[0] = 0x7f;
	fyai_wire_reader_init(&reader, copy, 1);
	errno = 0;
	FYAI_TCHECK(fyai_wire_get_value(&reader, gb, &scratch, &decoded) < 0 && errno == EBADMSG);
	copy[0] = 6;
	copy[1] = 0xff;
	copy[2] = 0xff;
	copy[3] = 0x7f;
	fyai_wire_reader_init(&reader, copy, 4);
	errno = 0;
	FYAI_TCHECK(fyai_wire_get_value(&reader, gb, &scratch, &decoded) < 0 && errno == EBADMSG);
	/* A varint of more than ten bytes. */
	memset(copy, 0xff, sizeof(copy));
	fyai_wire_reader_init(&reader, copy, sizeof(copy));
	errno = 0;
	FYAI_TCHECK(fyai_wire_get_varint(&reader, &(uint64_t){ 0 }) < 0 && errno == EBADMSG);
	free(bytes);
	fyai_scratch_close(&scratch);
	fy_generic_builder_destroy(gb);
	return 0;
}

int wire_limits(void)
{
	struct fy_generic_builder *gb;
	struct fyai_wire_writer counter;
	struct fyai_scratch scratch = { 0 };
	struct fyai_wire_reader reader;
	fy_generic value;
	unsigned char *deep;
	size_t i;
	int rc;

	gb = wire_builder();
	FYAI_TCHECK(gb);
	/* A value nested deeper than the limit is not encoded. */
	value = fy_value(gb, 1LL);
	for (i = 0; i < FYAI_WIRE_MAX_DEPTH + 4; i++)
		value = fy_gb_sequence_create(gb, 1, &value);
	fyai_wire_writer_init(&counter, -1);
	errno = 0;
	FYAI_TCHECK(fyai_wire_put_value(&counter, value) < 0 && errno == ELOOP);
	/* A float has no encoding. */
	fyai_wire_writer_init(&counter, -1);
	errno = 0;
	FYAI_TCHECK(fyai_wire_put_value(&counter, fy_value(gb, 1.5)) < 0 && errno == ENOTSUP);
	/* The decoder stops at the same depth: a frame of nested sequences. */
	rc = fyai_scratch_open(&scratch);
	FYAI_TCHECK(!rc);
	deep = malloc(2 * (FYAI_WIRE_MAX_DEPTH + 8));
	FYAI_TCHECK(deep);
	for (i = 0; i < FYAI_WIRE_MAX_DEPTH + 8; i++) {
		deep[i * 2] = 6;
		deep[i * 2 + 1] = 1;
	}
	fyai_wire_reader_init(&reader, deep, 2 * (FYAI_WIRE_MAX_DEPTH + 8));
	errno = 0;
	FYAI_TCHECK(fyai_wire_get_value(&reader, gb, &scratch, &value) < 0 && errno == ELOOP);
	free(deep);
	fyai_scratch_close(&scratch);
	fy_generic_builder_destroy(gb);
	return 0;
}

int scratch_arena(void)
{
	struct fyai_scratch scratch = { 0 };
	int *numbers, *grown;
	char *text, *second;
	size_t i;
	int rc;

	rc = fyai_scratch_open(&scratch);
	FYAI_TCHECK(!rc);
	numbers = fyai_scratch_calloc(&scratch, 1000, sizeof(*numbers));
	FYAI_TCHECK(numbers);
	for (i = 0; i < 1000; i++)
		FYAI_TCHECK(!numbers[i]);
	for (i = 0; i < 1000; i++)
		numbers[i] = (int)i;
	grown = fyai_scratch_grow(&scratch, numbers, 1000, 100000, sizeof(*numbers));
	FYAI_TCHECK(grown && grown != numbers);
	for (i = 0; i < 1000; i++)
		FYAI_TCHECK(grown[i] == (int)i && numbers[i] == (int)i);
	grown = fyai_scratch_grow(&scratch, NULL, 0, 10, sizeof(*grown));
	FYAI_TCHECK(grown);
	text = fyai_scratch_strndup(&scratch, "abcdef", 3);
	second = fyai_scratch_strndup(&scratch, "", 0);
	FYAI_TCHECK(text && !strcmp(text, "abc") && second && !*second);
	/* An alignment that suits any type, and a request that cannot fit. */
	FYAI_TCHECK(((uintptr_t)fyai_scratch_alloc(&scratch, 1) % alignof(max_align_t)) == 0);
	errno = 0;
	FYAI_TCHECK(!fyai_scratch_calloc(&scratch, SIZE_MAX / 2, 4) && errno == ENOMEM);
	fyai_scratch_close(&scratch);
	fyai_scratch_close(&scratch);
	return 0;
}
