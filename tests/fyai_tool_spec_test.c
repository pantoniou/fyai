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
#include "fyai_tool_template.h"
#include "fyai_tool_registry.h"
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(tools, descriptions, tools_descriptions)
FYAI_TEST_ENTRY(tools, filtered, tools_filtered)
FYAI_TEST_ENTRY(tools, personas, tools_personas)
FYAI_TEST_ENTRY(tools, cache, tools_cache)
FYAI_TEST_ENTRY(tools, templates, tools_templates)
FYAI_TEST_ENTRY(tools, registry, tools_registry)

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
			"parameters"), "properties"), "questions"), "type"),
		"array"), "ask_user.questions type");
	require(fy_equal(
		fy_get(fy_get(fy_get(fy_get(fy_get(fy_get(
			tool_by_name(tools, "ask_user"), "function"),
			"parameters"), "properties"), "questions"), "items"),
		"type"),
		"object"),
		"ask_user.questions items");
}

static void test_filtered(void)
{
	fy_generic tools, tool, fn, n;
	bool has_ask_user = false;
	bool has_agent = false;

	/* A plain context keeps all tools. */
	tools = make_tools_filtered(&test_ctx);
	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		n = fy_get(fn, "name");
		if (fy_equal(n, "ask_user"))
			has_ask_user = true;
		if (fy_equal(n, "agent"))
			has_agent = true;
	}
	require(has_ask_user && has_agent,
		"plain context must have ask_user and agent");

	/* A sub-agent context removes agent and parent agent controls. */
	test_cfg.agent_child = true;
	tools = make_tools_filtered(&test_ctx);
	has_ask_user = has_agent = false;
	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		n = fy_get(fn, "name");
		if (fy_equal(n, "ask_user"))
			has_ask_user = true;
		if (fy_equal(n, "agent"))
			has_agent = true;
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

/* Every {{name}} in the embedded tools is a name that has a value. */
static void template_names_known(fy_generic tree)
{
	fy_generic key, item;
	const char *text, *open, *close, *name;
	char buffer[64];
	size_t length;

	if (fy_is_string(tree)) {
		text = fy_castp(&tree, "");
		for (open = strstr(text, "{{"); open; open = strstr(close + 2, "{{")) {
			close = strstr(open + 2, "}}");
			require(close, "an unclosed {{ in a tool description");
			length = (size_t)(close - open - 2);
			require(length < sizeof(buffer), "a placeholder name is too long");
			memcpy(buffer, open + 2, length);
			buffer[length] = '\0';
			require(fyai_tool_template_state(&test_ctx, buffer),
				"a tool description names an unknown placeholder");
		}
	} else if (fy_is_mapping(tree)) {
		fy_foreach_key_value(name, item, tree)
			template_names_known(item);
	} else if (fy_is_sequence(tree)) {
		fy_foreach(item, tree)
			template_names_known(item);
	}
	(void)key;
}

static bool template_has(fy_generic tree, const char *needle)
{
	fy_generic item;
	const char *name;

	if (fy_is_string(tree))
		return strstr(fy_castp(&tree, ""), needle) != NULL;
	if (fy_is_mapping(tree)) {
		fy_foreach_key_value(name, item, tree)
			if (template_has(item, needle))
				return true;
	} else if (fy_is_sequence(tree)) {
		fy_foreach(item, tree)
			if (template_has(item, needle))
				return true;
	}
	return false;
}

static void test_templates(void)
{
#ifdef __linux__
	char root[] = "/tmp/fyai-tool-template-XXXXXX", arena[256];
#endif
	fy_generic tools, agent;

	template_names_known(make_tools(&test_ctx));

	/* A run that cannot isolate says nothing of it, and offers no tool for it. */
	tools = make_tools_filtered(&test_ctx);
	require(!template_has(tools, "{{"), "a placeholder was not expanded");
	require(!template_has(tools, "templates"), "the templates are sent to the provider");
	require(!template_has(tools, "private copy"), "isolation is described but unavailable");
	require(fy_is_invalid(tool_by_name(tools, "project_view")), "project_view is offered");
	agent = tool_by_name(tools, "agent");
	require(!fy_is_valid(fy_get(fy_get(fy_get(fy_get(agent, "function"), "parameters"),
					   "properties"), "isolated")),
		"the isolated parameter is offered");

#ifdef __linux__
	/* With an arena in the project, isolation is optional, or on. */
	require(mkdtemp(root) != NULL, "mkdtemp");
	snprintf(arena, sizeof(arena), "%s/.fyai", root);
	require(!mkdir(arena, 0700), "mkdir .fyai");
	snprintf(arena, sizeof(arena), "%s/.fyai/arena", root);
	require(!mkdir(arena, 0700), "mkdir arena");
	test_cfg.arena_dir = arena;
	test_cfg.config_generation++;
	tools = make_tools_filtered(&test_ctx);
	require(template_has(tools, "set `isolated` to true"), "the optional text is missing");
	require(!template_has(tools, "isolated by default"), "the default text is present");
	require(fy_is_valid(tool_by_name(tools, "project_view")), "project_view is missing");

	test_cfg.config_doc = fy_mapping(test_ctx.gb, "agent",
					 fy_mapping(test_ctx.gb, "isolation", "view"));
	test_cfg.config_generation++;
	tools = make_tools_filtered(&test_ctx);
	require(template_has(tools, "isolated by default"), "the enabled text is missing");
	require(!template_has(tools, "{{"), "a placeholder was not expanded");

	test_cfg.arena_dir = NULL;
	require(!rmdir(arena), "remove the arena directory");
	snprintf(arena, sizeof(arena), "%s/.fyai", root);
	require(!rmdir(arena), "remove .fyai");
	require(!rmdir(root), "remove the project");
#endif
}

int tools_templates(void)
{
	return tools_run(test_templates);
}

/* Every tool of data/tools.yaml is registered with all that it needs. */
static void test_registry(void)
{
	fy_generic tools = make_tools(&test_ctx);
	const struct fyai_tool_def *def, *other;
	fy_generic tool, gname;
	const char *name;
	size_t i, j;

	fy_foreach(tool, tools) {
		gname = fy_get(fy_get(tool, "function"), "name");
		name = fy_castp(&gname, "");
		def = fyai_tool_find(name);
		if (!def) {
			fprintf(stderr, "tool '%s' is not registered\n", name);
			fail("a tool of tools.yaml has no registry entry");
		}
		require(def->run || def->run_text, "a tool needs a run function");
		require(def->head || (def->flags & FYAI_TOOL_SILENT),
			"a tool needs a head or must be silent");
	}

	/* The entries are unique, named, and hosted ones have no local run. */
	for (i = 0; (def = fyai_tool_at(i)); i++) {
		require(def->name && *def->name, "an entry needs a name");
		require(!(def->flags & FYAI_TOOL_HOSTED) ||
			(!def->run && !def->run_text),
			"a hosted tool is run by the provider");
		for (j = i + 1; (other = fyai_tool_at(j)); j++)
			require(strcmp(def->name, other->name),
				"a tool is registered twice");
	}

	/* A call that reads the tables of the parent never runs in a job. */
	for (i = 0; i < 5; i++) {
		static const char *const parent[] = {
			"agent_input", "agent_message", "cancel",
			"shell_close", "time",
		};

		require(fyai_tool_has(parent[i], FYAI_TOOL_PARENT),
			"a tool of the parent runs in a job");
		require(fyai_tool_find(parent[i])->effect !=
			FYAI_TOOL_EFFECT_NONE || !strcmp(parent[i], "time"),
			"a tool that ends processes needs an ordering class");
	}
	/* A wait stays in the parent unless it waits for an event. */
	require(fyai_tool_find("wait")->in_parent, "wait needs an in_parent test");
	/* The wire name of the shell finds the shell. */
	require(fyai_tool_find("exec_command") == fyai_tool_find("shell"),
		"the wire name of the shell must find it");
	require(!fyai_tool_find("no_such_tool"), "an unknown name has no entry");
}

int tools_registry(void)
{
	return tools_run(test_registry);
}
