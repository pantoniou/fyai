/*
 * fyai_scope_test.c - tests for the pin handle of the scope of an exchange
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include "fyai.h"
#include "fyai_test.h"
#include "fyai_tools.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(scope, pin_handle_balances, scope_pin_handle)

/*
 * A handle adds one count and removes it one time. A second pin of a held
 * handle and an unpin of a free one leave the count of another object alone.
 */
int scope_pin_handle(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	struct fyai_scope_pin a = { 0 }, b = { 0 };

	/* The positive control: a held handle counts. */
	fyai_scope_pin(&ctx, &a);
	FYAI_TCHECK(ctx.scope_pins == 1);
	fyai_scope_pin(&ctx, &b);
	FYAI_TCHECK(ctx.scope_pins == 2);
	fyai_scope_pin(&ctx, &a);
	FYAI_TCHECK(ctx.scope_pins == 2);
	fyai_scope_unpin(&ctx, &a);
	FYAI_TCHECK(ctx.scope_pins == 1);
	fyai_scope_unpin(&ctx, &a);
	FYAI_TCHECK(ctx.scope_pins == 1);
	FYAI_TCHECK(b.held);
	fyai_scope_unpin(&ctx, &b);
	FYAI_TCHECK(ctx.scope_pins == 0);
	return 0;
}
