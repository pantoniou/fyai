/*
 * fyai_cast.h - asciicast v2 recording
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CAST_H
#define FYAI_CAST_H

#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_cast;

/*
 * Open the recording @path, a file of the asciicast v2 format that
 * `asciinema play` reads. A new file is made for the owner alone:
 * a recording holds all that the terminal showed. @command names the program
 * in the header; it can be NULL. @props, when it is a mapping, goes into the
 * header as the member `fyai`: what the program knew when it started, for the
 * person who reads the recording. Return NULL with a diagnostic on failure.
 */
struct fyai_cast *fyai_cast_open(struct fyai_ctx *ctx, const char *path,
				 int cols, int rows, const char *command,
				 fy_generic props);

/*
 * Add an event, stamped with the time since the open. A failed write ends the
 * recording and reports one warning; the calls that follow do nothing. Bytes
 * that are not UTF-8 become U+FFFD, and a character that arrives in two
 * pieces is held for the second one.
 */
void fyai_cast_output(struct fyai_cast *c, const void *data, size_t len);
void fyai_cast_input(struct fyai_cast *c, const void *data, size_t len);
void fyai_cast_resize(struct fyai_cast *c, int cols, int rows);
/* A named point of the timeline. The text is the label. */
void fyai_cast_marker(struct fyai_cast *c, const char *label);

void fyai_cast_close(struct fyai_cast *c);

#endif
