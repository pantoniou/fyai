/*
 * fyai_tool_spec_test.c - unit tests for the built-in tool specifications
 * (data/tools.yaml, see doc/tools-yaml.md)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_tool_spec.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(tools, descriptions, tools_descriptions)
FYAI_TEST_ENTRY(tools, filtered, tools_filtered)
FYAI_TEST_ENTRY(tools, personas, tools_personas)
FYAI_TEST_ENTRY(tools, cache, tools_cache)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;

static int tools_setup(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	memset(&test_cfg, 0, sizeof(test_cfg));
	memset(&test_ctx, 0, sizeof(test_ctx));

	test_ctx.cfg = &test_cfg;
	test_ctx.transient_gb = fy_generic_builder_create(&gb_cfg);
	if (!test_ctx.transient_gb)
		return 1;
	test_ctx.durable_gb = test_ctx.transient_gb;
	test_ctx.gb = test_ctx.transient_gb;
	test_cfg.gb = test_ctx.transient_gb;
	test_ctx.tools_spec = fy_invalid;
	return 0;
}

static void tools_teardown(void)
{
	fy_generic_builder_destroy(test_ctx.transient_gb);
}

static int tools_run(void (*testfn)(void))
{
	if (tools_setup())
		return 1;
	testfn();
	tools_teardown();
	return 0;
}

static fy_generic tool_by_name(fy_generic tools, const char *name)
{
	fy_generic tool, fn, n;

	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		n = fy_get(fn, "name");
		if (fy_equal(n, name))
			return tool;
	}
	return fy_invalid;
}

static fy_generic persona_description(fy_generic tools)
{
	fy_generic tool = tool_by_name(tools, "agent");

	return fy_get(fy_get(fy_get(fy_get(fy_get(tool, "function"),
					   "parameters"), "properties"),
			     "persona"), "description");
}

static void fail(const char *msg)
{
	fprintf(stderr, "FAIL %s\n", msg);
	exit(1);
}

static void require(bool cond, const char *msg)
{
	if (!cond)
		fail(msg);
}

/* ---- tests ---- */

static void test_descriptions(void)
{
	fy_generic tools = make_tools(&test_ctx);
	fy_generic tool, fn, params, props, prop, desc;
	size_t i, n;

	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		desc = fy_get(fn, "description");
		require(fy_is_string(desc),
			"tool description must be a string");
		require(*fy_castp(&desc, ""), "tool description must not be empty");

		params = fy_get(fn, "parameters");
		props = fy_get(params, "properties");
		n = fy_generic_mapping_get_pair_count(props);
		for (i = 0; i < n; i++) {
			prop = fy_generic_mapping_get_at_value(props, i);
			desc = fy_get(prop, "description");
			require(fy_is_string(desc),
				"property description must be a string");
			require(*fy_castp(&desc, ""),
				"property description must not be empty");
		}
	}

	/* property schema types */
	require(fy_equal(
		fy_get(fy_get(fy_get(fy_get(fy_get(
			tool_by_name(tools, "exec_command"), "function"),
			"parameters"), "properties"), "timeout"), "type"),
		"integer"), "shell.timeout type");
	require(fy_equal(
		fy_get(fy_get(fy_get(fy_get(fy_get(
			tool_by_name(tools, "agent"), "function"),
			"parameters"), "properties"), "timeout"), "type"),
		"integer"), "agent.timeout type");
	require(fy_equal(
		fy_get(fy_get(fy_get(fy_get(fy_get(
			tool_by_name(tools, "ask_user"), "function"),
			"parameters"), "properties"), "options"), "type"),
		"array"), "ask_user.options type");
	require(fy_equal(
		fy_get(fy_get(fy_get(fy_get(fy_get(fy_get(
			tool_by_name(tools, "ask_user"), "function"),
			"parameters"), "properties"), "options"), "items"),
		"type"),
		"string"),
		"ask_user.options items");
}

static void test_filtered(void)
{
	fy_generic tools, tool, fn, n;
	bool has_ask_user = false;
	bool has_agent = false;
	size_t count = 0;

	/* A plain context keeps all tools. */
	tools = make_tools_filtered(&test_ctx);
	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		n = fy_get(fn, "name");
		if (fy_equal(n, "ask_user"))
			has_ask_user = true;
		if (fy_equal(n, "agent"))
			has_agent = true;
		count++;
	}
	require(has_ask_user && has_agent,
		"plain context must have ask_user and agent");

	/* A sub-agent context removes agent and agent_input. */
	test_cfg.agent_child = true;
	tools = make_tools_filtered(&test_ctx);
	has_ask_user = has_agent = false;
	count = 0;
	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		n = fy_get(fn, "name");
		if (fy_equal(n, "ask_user"))
			has_ask_user = true;
		if (fy_equal(n, "agent"))
			has_agent = true;
		count++;
	}
	/* It cannot delegate or answer another sub-agent, but it can ask:
	 * the question goes up to the parent, where the person is. */
	require(has_ask_user && !has_agent,
		"a sub-agent keeps ask_user and loses agent");
}

static void test_personas(void)
{
	fy_generic config_doc = fy_mapping(test_ctx.gb,
		"agent", fy_mapping(test_ctx.gb,
			"personas", fy_mapping(test_ctx.gb,
				"triage", fy_mapping(test_ctx.gb,
					"description", "Triage persona"))));
	fy_generic tools, tool, fn, params, props, persona, desc;
	const char *want =
		"Optional named persona for the sub-agent. Available:\n"
		"- `triage`: Triage persona";

	test_cfg.config_doc = config_doc;
	tools = make_tools_filtered(&test_ctx);

	tool = tool_by_name(tools, "agent");
	require(fy_is_valid(tool), "agent tool missing");
	fn = fy_get(tool, "function");
	params = fy_get(fn, "parameters");
	props = fy_get(params, "properties");
	persona = fy_get(props, "persona");
	desc = fy_get(persona, "description");
	require(fy_equal(desc, want), "persona description not enriched");
}

static void test_cache(void)
{
	fy_generic personas, first, second, desc;

	first = make_tools_filtered(&test_ctx);
	second = make_tools_filtered(&test_ctx);
	require(fy_equal(first, second), "the cached tool set must be stable");
	require(fy_equal(first, test_ctx.tools_spec),
		"the context must keep the cached tool set");

	personas = fy_mapping(test_ctx.gb,
		"agent", fy_mapping(test_ctx.gb,
			"personas", fy_mapping(test_ctx.gb,
				"triage", fy_mapping(test_ctx.gb,
					"description", "Triage persona"))));
	test_cfg.config_doc = personas;
	test_cfg.config_generation++;
	second = make_tools_filtered(&test_ctx);
	desc = persona_description(second);
	require(strstr(fy_castp(&desc, ""), "triage") != NULL,
		"a configuration change must rebuild the tool set");
}

/* ---- entries ---- */

int tools_descriptions(void)
{
	return tools_run(test_descriptions);
}

int tools_filtered(void)
{
	return tools_run(test_filtered);
}

int tools_personas(void)
{
	return tools_run(test_personas);
}

int tools_cache(void)
{
	return tools_run(test_cache);
}
