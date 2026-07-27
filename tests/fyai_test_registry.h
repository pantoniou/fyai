/*
 * fyai_test_registry.h - single-binary unit test registry
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TEST_REGISTRY_H
#define FYAI_TEST_REGISTRY_H

/* Declare a test:
 *
 *	FYAI_TEST_ENTRY(suite, test_name, entry_function)
 *
 * Put declarations after the includes. CMake uses them to generate the
 * registry and CTest tests. */
#define FYAI_TEST_ENTRY(suite, name, entry)

struct fyai_test_case {
	const char *suite;
	const char *name;
	int (*run)(void);
};

/* Add a test to the registry. */
void fyai_test_register(const char *suite, const char *name, int (*run)(void));

/* Add all generated test entries to the registry. */
void fyai_test_register_all(void);

/* Access tests in registration order. */
int fyai_test_count(void);
const struct fyai_test_case *fyai_test_at(int idx);

/* Find an exact suite and test name. */
const struct fyai_test_case *fyai_test_find(const char *suite, const char *name);

#endif
