/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAST_FILE_H
#define FYAI_CAST_FILE_H

#include <stdio.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * Read a recording that --cast wrote (src/front/fyai_cast.c): a header line, then one
 * array [seconds, kind, text] for each event. The reader parses a line at a
 * time with a scratch builder that it makes again from time to time, so the
 * value of an event lives until the next call of fyai_cast_file_next().
 */
struct fyai_cast_file {
	FILE *fp;
	char *line;
	size_t cap;
	struct fy_generic_builder *gb;
	unsigned int parsed;
};

/* Open @path and read its header into *@header. Returns 0, or -1 with the cause raised. */
int fyai_cast_file_open(struct fyai_ctx *ctx, struct fyai_cast_file *f,
			const char *path, fy_generic *header);
void fyai_cast_file_close(struct fyai_cast_file *f);

/* The next event, or fy_invalid at the end. */
fy_generic fyai_cast_file_next(struct fyai_cast_file *f);

#endif
