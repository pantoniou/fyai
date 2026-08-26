/*
 * fyai_test_scratch.h - temporary files and directories that a test leaves
 *                       behind nowhere
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TEST_SCRATCH_H
#define FYAI_TEST_SCRATCH_H

/*
 * A test checks with FYAI_TCHECK() or exit(), so it has no single return
 * path that could remove what it made. These functions record each path and
 * remove all of them at exit, whichever way the binary ends.
 *
 * Every returned path belongs to this module. Do not free it.
 */

/* Make a scratch directory named after @tag and return its path. */
const char *fyai_test_scratch_dir(const char *tag);

/* Make a scratch file named after @tag, with an optional @suffix such as
 * ".yaml", and return its path. The file is created and closed. */
const char *fyai_test_scratch_file(const char *tag, const char *suffix);

/* Record @path, which a test made itself, for removal. Returns 0 on
 * success. */
int fyai_test_scratch_add(const char *path);

/* Remove every recorded path now. The exit handler calls this too. */
void fyai_test_scratch_cleanup(void);

/*
 * Put the scratch of this run under one directory and point $TMPDIR at it,
 * so a test that crashes still leaves its files where the run removes them.
 * Call it one time, from the process that owns the run.
 */
int fyai_test_scratch_root(void);

/*
 * Drop the records without removing anything. A forked child calls this so
 * that its own exit does not remove what the parent owns.
 */
void fyai_test_scratch_forget(void);

#endif
