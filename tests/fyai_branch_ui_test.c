/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_agents.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_session.h"
#include "fyai_sink.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(branch_ui, template_unbounded, branch_ui_template_unbounded)
FYAI_TEST_ENTRY(branch_ui, template_literal, branch_ui_template_literal)
FYAI_TEST_ENTRY(branch_ui, admission_release, branch_ui_admission_release)
FYAI_TEST_ENTRY(branch_ui, depth_detail, branch_ui_depth_detail)
FYAI_TEST_ENTRY(branch_ui, subscription_activity, branch_ui_subscription_activity)
FYAI_TEST_ENTRY(branch_ui, preview_extent, branch_ui_preview_extent)
FYAI_TEST_ENTRY(branch_ui, tile_identity, branch_ui_tile_identity)

struct activity_capture {
	volatile bool received;
	char state[24];
};

static fy_generic capture_activity(struct jsonrpc_conn *conn, const char *method,
				   fy_generic params, fy_generic id,
				   void *userdata, fy_generic *errorp)
{
	struct activity_capture *capture = userdata;

	(void)conn;
	(void)id;
	*errorp = fy_invalid;
	FYAI_TCHECK(!strcmp(method, "agent/event"));
	snprintf(capture->state, sizeof(capture->state), "%s",
		 fy_get(params, "state", ""));
	capture->received = true;
	return fy_invalid;
}

int branch_ui_subscription_activity(void)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fyai_cfg cfg = {};
	struct fyai_ctx ctx = {
		.cfg = &cfg, .transient_gb = gb, .agent_execution = 1,
		.agent_branch = "main/agent:worker",
	};
	struct activity_capture capture = {};
	struct jsonrpc_conn *sender, *receiver;
	const char *states[] = { "waiting for input", "retrying", "running" };
	fy_generic params, result, error;
	int outbound[2], inbound[2], rc;
	size_t i;
	bool handled;

	FYAI_TCHECK(gb != NULL);
	rc = pipe(outbound);
	FYAI_TCHECK(!rc);
	rc = pipe(inbound);
	FYAI_TCHECK(!rc);
	sender = jsonrpc_conn_stdio(&ctx, outbound[1], inbound[0], 0, "owner", NULL);
	receiver = jsonrpc_conn_stdio(&ctx, inbound[1], outbound[0], 0, "observer", NULL);
	FYAI_TCHECK(sender && receiver);
	rc = jsonrpc_conn_serve(receiver, capture_activity, &capture);
	FYAI_TCHECK(!rc);
	params = fy_gb_mapping(gb, "execution", 1LL, "action", "subscribe", "enabled", true);
	handled = fyai_agents_serve(&ctx, NULL, "agent/control", params,
		fy_invalid, &result, &error);
	FYAI_TCHECK(handled && fy_is_invalid(error));
	for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		ctx.tool_rpc = NULL;
		fyai_agents_activity(&ctx, states[i]);
		ctx.tool_rpc = sender;
		capture.received = false;
		handled = fyai_agents_serve(&ctx, NULL, "agent/control", params,
			fy_invalid, &result, &error);
		FYAI_TCHECK(handled && fy_is_invalid(error));
		rc = fyai_event_loop_run_until(ctx.el, &capture.received, 1000);
		FYAI_TCHECK(!rc);
		FYAI_TCHECK(!strcmp(capture.state, states[i]));
	}
	fyai_agents_cleanup(&ctx);
	jsonrpc_conn_destroy(sender);
	jsonrpc_conn_destroy(receiver);
	close(outbound[0]);
	close(outbound[1]);
	close(inbound[0]);
	close(inbound[1]);
	fyai_event_loop_destroy(ctx.el);
	fyai_event_pool_drain(&ctx);
	fy_generic_builder_destroy(gb);
	return 0;
}

int branch_ui_template_unbounded(void)
{
	char value[4096];
	char *out;
	struct fyai_tmpl_var vars[] = { { "cwd", value } };

	memset(value, 'a', sizeof(value) - 1);
	value[sizeof(value) - 1] = '\0';
	out = fyai_prompt_expand("{{{cwd}}}{unknown}", vars, 1);
	FYAI_TCHECK(out != NULL);
	FYAI_TCHECK(strlen(out) == strlen(value) + 2);
	FYAI_TCHECK(out[0] == '{' && out[strlen(out) - 1] == '}');
	free(out);
	return 0;
}

int branch_ui_template_literal(void)
{
	char *out = fyai_prompt_literal("日本語/é/[x]*\n\033\177");

	FYAI_TCHECK(out != NULL);
	FYAI_TCHECK(!strcmp(out, "日本語\\/é\\/\\[x\\]\\*"));
	free(out);
	return 0;
}

int branch_ui_depth_detail(void)
{
	FYAI_TCHECK(fyai_agents_detail(0) == 2);
	FYAI_TCHECK(fyai_agents_detail(1) == 2);
	FYAI_TCHECK(fyai_agents_detail(2) == 1);
	FYAI_TCHECK(fyai_agents_detail(8) == 0);
	return 0;
}

int branch_ui_admission_release(void)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fyai_cfg cfg = { .agent_max_live_agents = 1 };
	struct fyai_ctx ctx = { .cfg = &cfg, .transient_gb = gb };
	fy_generic params, result = fy_invalid, error = fy_invalid;
	long long execution;
	bool handled;

	FYAI_TCHECK(gb != NULL);
	params = fy_gb_mapping(gb, "branch", "main/agent:one", "parent", 0LL);
	handled = fyai_agents_serve(&ctx, NULL, "agent/admit", params,
		fy_value(1LL), &result, &error);
	FYAI_TCHECK(handled && fy_is_invalid(error));
	execution = fy_get(result, "execution", 0LL);
	FYAI_TCHECK(execution > 0);
	params = fy_gb_mapping(gb, "branch", "main/agent:two", "parent", 0LL);
	handled = fyai_agents_serve(&ctx, NULL, "agent/admit", params,
		fy_value(2LL), &result, &error);
	FYAI_TCHECK(handled && fy_is_valid(error));
	params = fy_gb_mapping(gb, "execution", execution,
		"sequence", 1LL, "state", "completed");
	(void)fyai_agents_serve(&ctx, NULL, "agent/event", params,
		fy_invalid, &result, &error);
	FYAI_TCHECK(!strcmp(fyai_agents_state(&ctx, "main/agent:one"), "completed"));
	error = fy_invalid;
	params = fy_gb_mapping(gb, "branch", "main/agent:two", "parent", 0LL);
	handled = fyai_agents_serve(&ctx, NULL, "agent/admit", params,
		fy_value(3LL), &result, &error);
	FYAI_TCHECK(handled && fy_is_invalid(error));
	FYAI_TCHECK(fy_get(result, "execution", 0LL) > execution);
	/* An old execution cannot revive itself after releasing admission. */
	params = fy_gb_mapping(gb, "execution", execution,
		"sequence", 2LL, "state", "running");
	(void)fyai_agents_serve(&ctx, NULL, "agent/event", params,
		fy_invalid, &result, &error);
	FYAI_TCHECK(!strcmp(fyai_agents_state(&ctx, "main/agent:one"), "completed"));
	cfg.agent_max_live_agents = 2;
	params = fy_gb_mapping(gb, "branch", "main/agent:two/agent:two", "parent", 2LL);
	(void)fyai_agents_serve(&ctx, NULL, "agent/admit", params,
		fy_value(4LL), &result, &error);
	FYAI_TCHECK(fy_is_invalid(error));
	FYAI_TCHECK(fyai_agents_ambiguous(&ctx, "two"));
	FYAI_TCHECK(!fyai_agents_ambiguous(&ctx, "main/agent:two"));
	fyai_agents_cleanup(&ctx);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The live tile header reads branch-keyed identity from the registry. */
int branch_ui_tile_identity(void)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fyai_cfg cfg = { .agent_max_live_agents = 2 };
	struct fyai_ctx ctx = { .cfg = &cfg, .transient_gb = gb };
	fy_generic params, result = fy_invalid, error = fy_invalid;
	long long execution, id, started;
	const char *model;
	bool handled;

	FYAI_TCHECK(gb != NULL);
	FYAI_TCHECK(!fyai_agents_branch_identity(&ctx, "main/agent:one",
						 &model, &id, &started));
	FYAI_TCHECK(!model && !id && !started);
	params = fy_gb_mapping(gb, "branch", "main/agent:one", "parent", 0LL,
			       "model", "mock-model");
	handled = fyai_agents_serve(&ctx, NULL, "agent/admit", params,
		fy_value(1LL), &result, &error);
	FYAI_TCHECK(handled && fy_is_invalid(error));
	execution = fy_get(result, "execution", 0LL);
	FYAI_TCHECK(execution > 0);
	FYAI_TCHECK(fyai_agents_branch_identity(&ctx, "main/agent:one",
						&model, &id, &started));
	FYAI_TCHECK(id == execution && started > 0);
	FYAI_TCHECK(model && !strcmp(model, "mock-model"));
	/* The first event of the child replaces the model from the admission. */
	params = fy_gb_mapping(gb, "execution", execution, "sequence", 1LL,
			       "state", "running", "model", "child-model");
	(void)fyai_agents_serve(&ctx, NULL, "agent/event", params,
		fy_invalid, &result, &error);
	FYAI_TCHECK(fyai_agents_branch_identity(&ctx, "main/agent:one",
						&model, &id, &started));
	FYAI_TCHECK(model && !strcmp(model, "child-model"));
	FYAI_TCHECK(!strcmp(fyai_agents_state(&ctx, "main/agent:one"),
			    "running"));
	FYAI_TCHECK(!fyai_agents_branch_identity(&ctx, "main/agent:two",
						 &model, &id, &started));
	FYAI_TCHECK(!model && !id && !started);
	fyai_agents_cleanup(&ctx);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The preview takes its share of the browser pane, and stands down when what
 * is left cannot carry the page. */
int branch_ui_preview_extent(void)
{
	struct fyai_sink_page page = { .markdown = "x", .aside = "y", .extent = 40 };
	enum fyai_sink_split split;
	int extent;

	page.split = FYAI_SINK_SPLIT_AUTO;
	extent = fyai_sink_page_extent(&page, 30, 120, &split);
	/* The rule and the column each side of it come off the width first. */
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_RIGHT && extent == 47);
	page.aside_cols = 36;
	extent = fyai_sink_page_extent(&page, 30, 120, &split);
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_RIGHT && extent == 36);
	page.split = FYAI_SINK_SPLIT_BOTTOM;
	extent = fyai_sink_page_extent(&page, 30, 120, &split);
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_BOTTOM && extent == 12);
	page.aside_cols = 0;
	page.split = FYAI_SINK_SPLIT_AUTO;

	extent = fyai_sink_page_extent(&page, 20, 80, &split);
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_BOTTOM && extent == 8);

	extent = fyai_sink_page_extent(&page, 10, 60, &split);
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_NONE && !extent);

	/* A pane too narrow for both keeps the page whole. */
	page.split = FYAI_SINK_SPLIT_RIGHT;
	extent = fyai_sink_page_extent(&page, 30, 70, &split);
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_NONE && !extent);

	/* Without an aside there is nothing to place. */
	page.aside = NULL;
	page.split = FYAI_SINK_SPLIT_BOTTOM;
	extent = fyai_sink_page_extent(&page, 40, 120, &split);
	FYAI_TCHECK(split == FYAI_SINK_SPLIT_NONE && !extent);
	return 0;
}
