/*
 * fyai_cast_file.c - read a recording of a session
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cast_file.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* Lines parsed with one scratch builder before it is made again. */
#define CAST_BATCH 512

int fyai_cast_file_open(struct fyai_ctx *ctx, struct fyai_cast_file *f,
			const char *path, fy_generic *header)
{
	ssize_t got;

	memset(f, 0, sizeof(*f));
	f->fp = fopen(path, "r");
	fyai_error_check(ctx, f->fp, err, "cast: cannot open %s: %s", path,
			 strerror(errno));
	f->gb = fy_generic_builder_create(NULL);
	fyai_error_check(ctx, f->gb, err, "cast: cannot make a builder");
	got = getline(&f->line, &f->cap, f->fp);
	fyai_error_check(ctx, got > 0, err, "cast: %s has no header", path);
	*header = parse_json_string(f->gb, f->line);
	fyai_error_check(ctx, fy_is_mapping(*header) &&
			 fy_get(*header, "version", 0LL) == 2, err,
			 "cast: %s is not an asciicast v2 recording", path);
	return 0;
err:
	return -1;
}

void fyai_cast_file_close(struct fyai_cast_file *f)
{
	if (f->fp)
		fclose(f->fp);
	free(f->line);
	fy_generic_builder_destroy(f->gb);
}

fy_generic fyai_cast_file_next(struct fyai_cast_file *f)
{
	fy_generic v;

	if (f->parsed++ % CAST_BATCH == CAST_BATCH - 1) {
		fy_generic_builder_destroy(f->gb);
		f->gb = fy_generic_builder_create(NULL);
		if (!f->gb)
			return fy_invalid;
	}
	for (;;) {
		if (getline(&f->line, &f->cap, f->fp) <= 0)
			return fy_invalid;
		v = parse_json_string(f->gb, f->line);
		if (fy_is_sequence(v) && fy_len(v) == 3)
			return v;
	}
}
