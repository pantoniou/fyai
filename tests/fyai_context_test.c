/*
 * fyai_context_test.c - tests for the context-window output allowance
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_session.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(context, allowance_fits, context_allowance_fits)
FYAI_TEST_ENTRY(context, allowance_reduced, context_allowance_reduced)
FYAI_TEST_ENTRY(context, allowance_no_window, context_allowance_no_window)
FYAI_TEST_ENTRY(context, allowance_prompt_over, context_allowance_prompt_over)

static struct fyai_cfg ctxt_cfg;
static struct fyai_ctx ctxt_ctx = { .cfg = &ctxt_cfg };

static long long ctxt_allowance(int max_tokens, long long prompt,
				long long window)
{
	ctxt_cfg.max_tokens = max_tokens;
	return fyai_context_output_tokens(&ctxt_ctx, prompt, window);
}

int context_allowance_fits(void)
{
	/* Room to spare: the configured allowance stands. */
	FYAI_TCHECK(ctxt_allowance(8192, 1000, 100000) == 8192);
	FYAI_TCHECK(ctxt_allowance(8192, 91808, 100000) == 8192);

	printf("ok - an allowance that fits is not reduced\n");
	return 0;
}

int context_allowance_reduced(void)
{
	/* The prompt fits but leaves less than the allowance asks for. */
	FYAI_TCHECK(ctxt_allowance(8192, 95000, 100000) == 5000);
	FYAI_TCHECK(ctxt_allowance(384000, 649282, 1000000) == 350718);

	printf("ok - an allowance is reduced to the room the prompt leaves\n");
	return 0;
}

int context_allowance_no_window(void)
{
	/* No declared window and no allowance are both left alone. */
	FYAI_TCHECK(ctxt_allowance(8192, 999999, 0) == 8192);
	FYAI_TCHECK(ctxt_allowance(0, 1000, 100000) == 0);

	printf("ok - an undeclared window leaves the allowance alone\n");
	return 0;
}

int context_allowance_prompt_over(void)
{
	/*
	 * A prompt at or over the window leaves no room to reduce into. The
	 * configured allowance is reported unchanged, so the fill reads over
	 * the window - the request check refuses it separately.
	 */
	FYAI_TCHECK(ctxt_allowance(8192, 100000, 100000) == 8192);
	FYAI_TCHECK(ctxt_allowance(8192, 120000, 100000) == 8192);

	printf("ok - a prompt over the window leaves the allowance unchanged\n");
	return 0;
}

FYAI_TEST_ENTRY(context, source_names, context_source_names)

int context_source_names(void)
{
	/*
	 * The prompt report names the source, so the names are part of the
	 * interface rather than a debug detail.
	 */
	FYAI_TCHECK(!strcmp(fyai_context_source_name(FYAICS_LAST_CALL),
			    "last call"));
	FYAI_TCHECK(!strcmp(fyai_context_source_name(FYAICS_STORED), "stored"));
	FYAI_TCHECK(!strcmp(fyai_context_source_name(FYAICS_NONE), "none"));

	printf("ok - each prompt source has a reported name\n");
	return 0;
}
