/*
 * fyai_test_registry.c - registry storage for the single test binary
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "fyai_test_registry.h"

/* Maximum number of registered tests. */
#define FYAI_TEST_MAX 512

static struct fyai_test_case fyai_tests[FYAI_TEST_MAX];
static int fyai_tests_count;

void fyai_test_register(const char *suite, const char *name, int (*run)(void))
{
	struct fyai_test_case *tc;

	if (fyai_tests_count >= FYAI_TEST_MAX)
		return;

	tc = &fyai_tests[fyai_tests_count++];
	tc->suite = suite;
	tc->name = name;
	tc->run = run;
}

int fyai_test_count(void)
{
	return fyai_tests_count;
}

const struct fyai_test_case *fyai_test_at(int idx)
{
	if (idx < 0 || idx >= fyai_tests_count)
		return NULL;
	return &fyai_tests[idx];
}

const struct fyai_test_case *fyai_test_find(const char *suite, const char *name)
{
	int i;

	for (i = 0; i < fyai_tests_count; i++) {
		if (!strcmp(fyai_tests[i].suite, suite) &&
		    !strcmp(fyai_tests[i].name, name))
			return &fyai_tests[i];
	}
	return NULL;
}
