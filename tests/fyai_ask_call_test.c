/*
 * fyai_ask_call_test.c - an ask_user call that the page answers
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_ask.h"
#include "fyai_tools_int.h"
#include "fyai_uimodel.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(ask_call, answered, ask_call_answered)
FYAI_TEST_ENTRY(ask_call, declined, ask_call_declined)
FYAI_TEST_ENTRY(ask_call, cancelled, ask_call_cancelled)
FYAI_TEST_ENTRY(ask_call, not_for_the_page, ask_call_not_for_the_page)
FYAI_TEST_ENTRY(ask_call, group_entry, ask_call_group_entry)
FYAI_TEST_ENTRY(ask_call, group_cancel, ask_call_group_cancel)

struct env {
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
	struct fy_generic_builder *gb;
	int notified;
};

static void model_seen(void *data, enum fyai_ui_store store,
		       unsigned long long seq, fy_generic root)
{
	(void)data;
	(void)store;
	(void)seq;
	(void)root;
}

static void notified(void *userdata)
{
	struct env *e = userdata;

	e->notified++;
}

/* A context whose UI model has a consumer: a question can be put. */
static void env_open(struct env *e)
{
	struct fy_generic_builder_cfg bcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	memset(e, 0, sizeof(*e));
	e->ctx.cfg = &e->cfg;
	e->gb = fy_generic_builder_create(&bcfg);
	FYAI_TCHECK(e->gb);
	e->cfg.gb = e->gb;
	e->ctx.transient_gb = e->gb;
	FYAI_TCHECK(!fyai_uimodel_create(&e->ctx));
	FYAI_TCHECK(!fyai_uimodel_subscribe(&e->ctx, model_seen, e));
}

static void env_close(struct env *e)
{
	fyai_uimodel_unsubscribe(&e->ctx, model_seen, e);
	fyai_uimodel_release(&e->ctx);
	fy_generic_builder_destroy(e->gb);
}

static fy_generic one_question(struct env *e)
{
	fy_generic opts, q;

	opts = fy_sequence(e->gb,
			   fy_mapping(e->gb, "label", "yes", "description", "Go"),
			   fy_mapping(e->gb, "label", "no", "description", "Stop"));
	q = fy_mapping(e->gb, "header", "Plan", "question", "Proceed?",
		       "options", opts);
	return fy_mapping(e->gb, "questions", fy_sequence(e->gb, q));
}

int ask_call_answered(void)
{
	struct env e;
	struct fyai_ask_call *c;
	struct fyai_ask_reply reply = { .selected = 1u << 1 };
	fy_generic result;

	env_open(&e);
	c = fyai_ask_call_start(&e.ctx, one_question(&e), notified, &e);
	FYAI_TCHECK(c);
	FYAI_TCHECK(!fyai_ask_call_done(c) && !e.notified);
	FYAI_TCHECK(fy_is_invalid(fyai_ask_call_result(c)));

	fyai_ask_settle(&e.ctx, "q1", &reply, 1);
	FYAI_TCHECK(fyai_ask_call_done(c) && e.notified == 1);
	result = fyai_ask_call_result(c);
	FYAI_TCHECK(fy_is_string(result));
	FYAI_TCHECK(strstr(fy_castp(&result, ""), "\"no\""));
	FYAI_TCHECK(strstr(fy_castp(&result, ""), "answered"));
	fyai_ask_call_destroy(c);
	env_close(&e);
	printf("ok - an answer settles the call from the callback\n");
	return 0;
}

int ask_call_declined(void)
{
	struct env e;
	struct fyai_ask_call *c;
	fy_generic result;

	env_open(&e);
	c = fyai_ask_call_start(&e.ctx, one_question(&e), notified, &e);
	FYAI_TCHECK(c);
	fyai_ask_settle(&e.ctx, "q1", NULL, 0);
	FYAI_TCHECK(fyai_ask_call_done(c) && e.notified == 1);
	result = fyai_ask_call_result(c);
	FYAI_TCHECK(fy_is_string(result) &&
		    strstr(fy_castp(&result, ""), "declined"));
	fyai_ask_call_destroy(c);
	env_close(&e);
	printf("ok - no answer gives the declined result\n");
	return 0;
}

/* A cancel takes the questions back, settles the call and tells nobody. */
int ask_call_cancelled(void)
{
	struct env e;
	struct fyai_ask_call *c, *d;
	fy_generic result;

	env_open(&e);
	c = fyai_ask_call_start(&e.ctx, one_question(&e), notified, &e);
	FYAI_TCHECK(c);
	fyai_ask_call_cancel(c);
	FYAI_TCHECK(fyai_ask_call_done(c) && !e.notified);
	result = fyai_ask_call_result(c);
	FYAI_TCHECK(fy_is_string(result) &&
		    strstr(fy_castp(&result, ""), "declined"));
	/* The set is gone: a late settle finds nothing. */
	fyai_ask_settle(&e.ctx, "q1", NULL, 0);
	FYAI_TCHECK(!e.notified);
	fyai_ask_call_destroy(c);

	/* A call that is destroyed while it waits takes its questions back. */
	d = fyai_ask_call_start(&e.ctx, one_question(&e), notified, &e);
	FYAI_TCHECK(d);
	fyai_ask_call_destroy(d);
	fyai_ask_settle(&e.ctx, "q2", NULL, 0);
	FYAI_TCHECK(!e.notified);
	env_close(&e);
	printf("ok - a cancel and a destroy take the questions back\n");
	return 0;
}

/* The call is the page's only when the page can ask and nothing else answers. */
int ask_call_not_for_the_page(void)
{
	struct env e;

	env_open(&e);
	e.cfg.answers[0] = "yes";
	e.cfg.answer_count = 1;
	FYAI_TCHECK(!fyai_ask_call_start(&e.ctx, one_question(&e), NULL, NULL));
	e.ctx.answer_next = 1;
	FYAI_TCHECK(fyai_ask_call_start(&e.ctx, one_question(&e), NULL, NULL));
	fyai_ask_settle_all(&e.ctx);
	/* Questions that are not valid are left to the text path. */
	FYAI_TCHECK(!fyai_ask_call_start(&e.ctx, fy_mapping(e.gb, "questions",
			fy_seq_empty), NULL, NULL));
	env_close(&e);
	printf("ok - a call the page cannot answer runs as text\n");
	return 0;
}

static fy_generic ask_tool_call(struct env *e)
{
	return fy_mapping(e->gb, "type", "function_call", "name", "ask_user",
			  "call_id", "call_ask_1", "arguments",
			  fy_value(e->gb, "{\"questions\":[{\"header\":\"Plan\","
				   "\"question\":\"Proceed?\",\"options\":["
				   "{\"label\":\"yes\",\"description\":\"Go\"},"
				   "{\"label\":\"no\",\"description\":\"Stop\"}]}]}"));
}

static void group_done(struct fyai_tool_job_group *group, void *userdata)
{
	struct env *e = userdata;

	(void)group;
	e->notified++;
}

/* The group holds the call while the user decides, and collects its answer. */
int ask_call_group_entry(void)
{
	struct env e;
	struct fyai_tool_job_group *g;
	struct fyai_ask_reply reply = { .selected = 1u << 0 };
	fy_generic result;
	bool ok = false;

	env_open(&e);
	g = fyai_tool_job_group_create_notify(&e.ctx, group_done, &e);
	FYAI_TCHECK(g);
	FYAI_TCHECK(!fyai_tool_job_group_add(g, ask_tool_call(&e)));
	FYAI_TCHECK(!fyai_tool_job_group_submit(g));
	FYAI_TCHECK(!fyai_tool_job_group_done(g));
	FYAI_TCHECK(fyai_tool_job_group_collect(g, 0, &result, &ok) < 0);

	fyai_ask_settle(&e.ctx, "q1", &reply, 1);
	FYAI_TCHECK(fyai_tool_job_group_done(g));
	FYAI_TCHECK(!fyai_tool_job_group_collect(g, 0, &result, &ok));
	FYAI_TCHECK(ok && fy_is_string(result) &&
		    strstr(fy_castp(&result, ""), "\"yes\""));
	fyai_tool_job_group_destroy(g);
	env_close(&e);
	printf("ok - a tool group waits for the user and collects the answer\n");
	return 0;
}

/* A cancel of the group ends the call with the declined result. */
int ask_call_group_cancel(void)
{
	struct env e;
	struct fyai_tool_job_group *g;
	fy_generic result;
	bool ok = false;

	env_open(&e);
	g = fyai_tool_job_group_create_notify(&e.ctx, group_done, &e);
	FYAI_TCHECK(g);
	FYAI_TCHECK(!fyai_tool_job_group_add(g, ask_tool_call(&e)));
	FYAI_TCHECK(!fyai_tool_job_group_submit(g));
	fyai_tool_job_group_cancel(g);
	FYAI_TCHECK(fyai_tool_job_group_done(g));
	FYAI_TCHECK(!fyai_tool_job_group_collect(g, 0, &result, &ok));
	FYAI_TCHECK(fy_is_string(result) &&
		    strstr(fy_castp(&result, ""), "declined"));
	/* The set was taken back with the cancel. */
	fyai_ask_settle(&e.ctx, "q1", NULL, 0);
	fyai_tool_job_group_destroy(g);
	env_close(&e);
	printf("ok - a cancelled tool group declines the question\n");
	return 0;
}
