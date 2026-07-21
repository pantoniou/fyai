/*
 * fyai_test.h - assertions for the unit tests
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * assert() is compiled out under -DNDEBUG, which the Release build sets. A
 * test whose checks vanish in Release still links and still runs, so it
 * reports success without having verified anything. FYAI_TCHECK() is an
 * ordinary if/abort instead of an assert, so a check costs the same in every
 * build type and cannot be configured away.
 *
 * It is still not a place to put work: the condition is a predicate, and the
 * operation under test belongs in a local above it, exactly as
 * fyai_error_check() requires of the tree proper.
 */

#ifndef FYAI_TEST_H
#define FYAI_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define FYAI_TCHECK(cond)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "%s:%d: %s: check failed: %s\n",	\
				__FILE__, __LINE__, __func__, #cond);	\
			abort();					\
		}							\
	} while (0)

#endif
