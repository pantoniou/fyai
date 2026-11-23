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

#include <libfyaml/libfyaml-generic.h>

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

/*
 * Count the lines that a unified diff adds and removes in each file. @diff is
 * the text that fyai_diff_unified() or `git diff` writes, with or without
 * `diff --git` rows. *@out is a sequence of {path, added, removed, binary},
 * one for each file in the order of the diff, built in @gb and valid as long
 * as @gb. A path is the new name, or the old name of a deleted file. Hunk
 * headers give the length of each hunk, so a removed row that starts with
 * `-- ` is not read as a file header. Returns 0, or -1 when memory runs out.
 */
int fyai_diff_stat(struct fy_generic_builder *gb, const char *diff, size_t len,
		   fy_generic *out);

#endif
