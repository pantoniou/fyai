/*
 * fyai_pending_test.c - unit tests for branch pending-work ownership
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_host.h"
#include "fyai_pending.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(pending, owner_start, pending_owner_start)

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/*
 * Verify PID reuse detection: a matching PID with a different process start
 * time must not be treated as the original live owner.
 */
int pending_owner_start(void)
{
	struct fy_generic_builder_cfg gbc = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbc);
	uint64_t start = 0;
	long long pid = (long long)getpid();

	failures = 0;
	CHECK(gb);
	/* On hosts without process start times, fall back to PID liveness. */
	if (fyai_host_process_start(getpid(), &start)) {
		CHECK(fyai_pending_owner_alive(fy_mapping(gb, "pid", pid,
							  "start", 1LL)));
		fy_generic_builder_destroy(gb);
		return failures ? 1 : 0;
	}
	CHECK(fyai_pending_owner_alive(fy_mapping(gb, "pid", pid,
						  "start", (long long)start)));
	CHECK(fyai_pending_owner_alive(fy_mapping(gb, "pid", pid)));
	CHECK(!fyai_pending_owner_alive(fy_mapping(gb, "pid", pid,
						   "start", (long long)start + 1)));
	CHECK(!fyai_pending_owner_alive(fy_mapping(gb, "pid", 0LL)));
	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
