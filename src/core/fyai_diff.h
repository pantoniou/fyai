/*
 * fyai_diff.h - line diff of two texts in the unified format
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FYAI_DIFF_H
#define FYAI_DIFF_H

#include <stddef.h>

/*
 * Compare @a and @b line by line and write a unified diff with @context lines
 * around each change to *@outp, a NUL-terminated heap string that the caller
 * frees. Two equal texts give an empty string. Past a bounded number of
 * edits, the changed region is written as one replacement: correct, but not
 * minimal. Returns 0, or -1 when memory runs out.
 */
int fyai_diff_unified(const char *a, size_t alen, const char *b, size_t blen,
		      const char *aname, const char *bname,
		      unsigned int context, char **outp);

#endif
