/*
 * fyai_stackcheck_test.c - tests for the dead-stack-frame generic detector
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/*
 * The bug under test: a builder-less scratch generic returned from the
 * function that created it. noinline + a scribble over the vacated frame
 * make the dangle deterministic instead of data-dependent.
 */
__attribute__((noinline))
static fy_generic make_scratch_escapee(void)
{
	return fy_mapping("type", "text", "text", "this storage dies with me");
}

__attribute__((noinline))
static void scribble_stack(void)
{
	volatile char burn[512];

	memset((void *)burn, 0x5a, sizeof(burn));
}

/* A scratch generic passed *down* the chain lives in a parent frame. */
__attribute__((noinline))
static int check_live_parent(fy_generic v)
{
	return generic_in_dead_stack_frame(v) ? -1 : 0;
}

int main(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;
	fy_generic scratch, escaped, interned;
	int failures = 0;

	gb = fy_generic_builder_create(&gb_cfg);
	if (!gb)
		return 1;

#ifndef __linux__
	/* the detector is Linux-only and reports false elsewhere */
	return 0;
#endif

	/* in-place values carry no pointer: never flagged */
	if (generic_in_dead_stack_frame(fy_value(42)) ||
	    generic_in_dead_stack_frame(fy_value("hi")) ||
	    generic_in_dead_stack_frame(fy_invalid)) {
		fprintf(stderr, "in-place/invalid flagged as dead stack\n");
		failures++;
	}

	/* builder-backed values live in the arena: never flagged */
	interned = fy_gb_internalize(gb,
			fy_value("a string long enough to go out of place"));
	if (generic_in_dead_stack_frame(interned)) {
		fprintf(stderr, "arena generic flagged as dead stack\n");
		failures++;
	}

	/* a scratch generic in the current frame is live */
	scratch = fy_mapping("key", "a value long enough to be out of place");
	if (generic_in_dead_stack_frame(scratch)) {
		fprintf(stderr, "current-frame scratch flagged as dead\n");
		failures++;
	}

	/* ... and stays live when passed down into a callee */
	if (check_live_parent(scratch)) {
		fprintf(stderr, "parent-frame scratch flagged as dead\n");
		failures++;
	}

	/* the bug: a scratch generic returned from its creating frame */
	escaped = make_scratch_escapee();
	scribble_stack();
	if (!generic_in_dead_stack_frame(escaped)) {
		fprintf(stderr, "escaped scratch generic not detected\n");
		failures++;
	}

	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
