/*
 * fyai_fork_test.c - tests for the state that a forked child drops
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_tools.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(fork, disown_drops_parent_state, fork_disown_drops_parent_state)

/* A child that copied a pointer of its parent must not follow it. */
int fork_disown_drops_parent_state(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	char parent;
	size_t i;

	ctx.workpane = (struct fyai_workpane_manager *)&parent;
	ctx.cmd_call = (struct fyai_cmd_call *)&parent;
	ctx.catalog_update = (struct fyai_catalog_update_request *)&parent;
	ctx.acp_rpc = (struct jsonrpc_conn *)&parent;
	ctx.transport_src = (struct fyai_event_source *)&parent;
	for (i = 0; i < ARRAY_SIZE(ctx.signal_src); i++)
		ctx.signal_src[i] = (struct fyai_event_source *)&parent;

	fyai_ctx_fork_disown(&ctx);

	FYAI_TCHECK(!ctx.workpane);
	FYAI_TCHECK(!ctx.cmd_call);
	FYAI_TCHECK(!ctx.catalog_update);
	FYAI_TCHECK(!ctx.acp_rpc);
	FYAI_TCHECK(!ctx.transport_src);
	for (i = 0; i < ARRAY_SIZE(ctx.signal_src); i++)
		FYAI_TCHECK(!ctx.signal_src[i]);
	printf("ok - a forked child drops the pointers of its parent\n");
	return 0;
}
