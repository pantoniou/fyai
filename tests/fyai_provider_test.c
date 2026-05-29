/*
 * fyai_provider_test.c - unit tests for the provider wire translators
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_provider.h"
#include "utils.h"

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;

static fy_generic parse(const char *json)
{
	fy_generic v;

	v = parse_json_string(test_ctx.transient_gb, json);
	if (fy_generic_is_invalid(v)) {
		fprintf(stderr, "failed to parse fixture: %s\n", json);
		exit(1);
	}
	return v;
}

static const char *emit(fy_generic v)
{
	const char *s;

	s = emit_request_body(test_ctx.transient_gb, v);
	if (!s) {
		fprintf(stderr, "failed to emit generic\n");
		exit(1);
	}
	return s;
}

static void expect_contains(const char *what, const char *haystack,
			    const char *needle)
{
	if (!strstr(haystack, needle)) {
		fprintf(stderr, "%s: missing %s in:\n%s\n",
			what, needle, haystack);
		exit(1);
	}
}

static void expect_absent(const char *what, const char *haystack,
			  const char *needle)
{
	if (strstr(haystack, needle)) {
		fprintf(stderr, "%s: unexpected %s in:\n%s\n",
			what, needle, haystack);
		exit(1);
	}
}

/* Chat-shaped canonical turn -> Responses input items. */
static void test_responses_input(void)
{
	fy_generic messages;
	const char *out;

	test_cfg.api_mode = FYAI_API_RESPONSES;

	messages = parse(
		"["
		"{\"role\": \"system\",\"content\": \"sys prompt\"},"
		"{\"role\": \"user\",\"content\": \"question\"},"
		"{\"role\": \"assistant\",\"content\":null,\"tool_calls\": ["
		"{\"id\": \"c1\",\"type\": \"function\",\"function\":"
		"{\"name\": \"read_file\",\"arguments\": \"{\\\"path\\\":\\\"f\\\"}\"}}]},"
		"{\"role\": \"tool\",\"tool_call_id\": \"c1\",\"content\": \"file data\"},"
		"{\"type\": \"reasoning\",\"id\": \"rs_1\",\"summary\":[]},"
		"{\"role\": \"assistant\",\"content\": \"the answer\"}"
		"]");

	out = emit(fyai_responses_input(&test_ctx, messages));

	/* the system turn is carried via `instructions`, never in input */
	expect_absent("responses_input", out, "\"system\"");
	/* stored reasoning items must never be replayed */
	expect_absent("responses_input", out, "reasoning");
	expect_contains("responses_input", out, "\"role\": \"user\"");
	expect_contains("responses_input", out,
			"\"type\": \"function_call\"");
	expect_contains("responses_input", out, "\"call_id\": \"c1\"");
	expect_contains("responses_input", out, "\"name\": \"read_file\"");
	expect_contains("responses_input", out,
			"\"type\": \"function_call_output\"");
	expect_contains("responses_input", out, "\"output\": \"file data\"");
	expect_contains("responses_input", out, "\"the answer\"");
}

/* Responses-native canonical turn -> Chat Completions messages. */
static void test_chat_input(void)
{
	fy_generic messages;
	const char *out;

	test_cfg.api_mode = FYAI_API_CHAT_COMPLETIONS;

	messages = parse(
		"["
		"{\"role\": \"user\",\"content\": \"question\"},"
		"{\"type\": \"reasoning\",\"id\": \"rs_1\"},"
		"{\"type\": \"function_call\",\"call_id\": \"c1\","
		"\"name\": \"read_file\",\"arguments\": \"{}\"},"
		"{\"type\": \"shell_call\",\"call_id\": \"s1\","
		"\"action\":{\"type\": \"exec\",\"commands\":[\"echo hi\"]}},"
		"{\"type\": \"function_call_output\",\"call_id\": \"c1\","
		"\"output\": \"file data\"},"
		"{\"type\": \"message\",\"role\": \"assistant\",\"content\":["
		"{\"type\": \"output_text\",\"text\": \"part one \"},"
		"{\"type\": \"output_text\",\"text\": \"part two\"}]}"
		"]");

	out = emit(fyai_chat_input(&test_ctx, messages));

	/* reasoning has no Chat analogue */
	expect_absent("chat_input", out, "reasoning");
	/* consecutive call items batch into one assistant tool_calls run */
	expect_contains("chat_input", out, "\"tool_calls\": [");
	expect_contains("chat_input", out, "\"id\": \"c1\"");
	expect_contains("chat_input", out, "\"name\": \"read_file\"");
	/* a native shell_call synthesizes a shell function call */
	expect_contains("chat_input", out, "\"id\": \"s1\"");
	expect_contains("chat_input", out, "\"name\": \"shell\"");
	expect_contains("chat_input", out, "echo hi");
	expect_contains("chat_input", out, "\"role\": \"tool\"");
	expect_contains("chat_input", out, "\"tool_call_id\": \"c1\"");
	/* message item text parts join into one content string */
	expect_contains("chat_input", out, "part one part two");
}

static void test_extract_usage(void)
{
	fy_generic doc, usage;

	test_cfg.api_mode = FYAI_API_CHAT_COMPLETIONS;
	doc = parse(
		"{\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":40,"
		"\"total_tokens\":140,"
		"\"prompt_tokens_details\":{\"cached_tokens\":25},"
		"\"completion_tokens_details\":{\"reasoning_tokens\":10},"
		"\"cost\":0.5}}");
	usage = fyai_extract_usage(&test_ctx, doc);
	if (fy_get(usage, "input", 0LL) != 100 ||
	    fy_get(usage, "output", 0LL) != 40 ||
	    fy_get(usage, "cached", 0LL) != 25 ||
	    fy_get(usage, "reasoning", 0LL) != 10 ||
	    fy_get(usage, "total", 0LL) != 140 ||
	    fy_get(usage, "cost", 0.0) != 0.5) {
		fprintf(stderr, "chat usage mismatch: %s\n", emit(usage));
		exit(1);
	}

	test_cfg.api_mode = FYAI_API_RESPONSES;
	doc = parse(
		"{\"usage\":{\"input_tokens\":60,\"output_tokens\":30,"
		"\"input_tokens_details\":{\"cached_tokens\":12},"
		"\"output_tokens_details\":{\"reasoning_tokens\":8}}}");
	usage = fyai_extract_usage(&test_ctx, doc);
	if (fy_get(usage, "input", 0LL) != 60 ||
	    fy_get(usage, "output", 0LL) != 30 ||
	    fy_get(usage, "cached", 0LL) != 12 ||
	    fy_get(usage, "reasoning", 0LL) != 8 ||
	    /* total absent on the wire: derived as input + output */
	    fy_get(usage, "total", 0LL) != 90) {
		fprintf(stderr, "responses usage mismatch: %s\n", emit(usage));
		exit(1);
	}

	/* no usage at all -> invalid */
	doc = parse("{\"id\": \"x\"}");
	if (fy_generic_is_valid(fyai_extract_usage(&test_ctx, doc))) {
		fprintf(stderr, "usage from empty doc\n");
		exit(1);
	}
}

static void test_response_accessors(void)
{
	fy_generic doc;
	const char *text;

	test_cfg.api_mode = FYAI_API_RESPONSES;
	doc = parse(
		"{\"id\": \"resp_1\",\"output\":["
		"{\"type\": \"reasoning\",\"id\": \"rs\"},"
		"{\"type\": \"message\",\"role\": \"assistant\",\"content\":["
		"{\"type\": \"output_text\",\"text\": \"hello \"},"
		"{\"type\": \"refusal\",\"refusal\": \"nope\"},"
		"{\"type\": \"output_text\",\"text\": \"world\"}]},"
		"{\"type\": \"function_call\",\"call_id\": \"c9\","
		"\"name\": \"shell\",\"arguments\": \"{}\"}"
		"]}");

	text = fy_cast(fyai_response_output_text(&test_ctx, doc), "");
	if (strcmp(text, "hello world")) {
		fprintf(stderr, "output_text: got '%s'\n", text);
		exit(1);
	}
	if (fy_len(fyai_response_tool_calls(&test_ctx, doc)) != 1 ||
	    !fyai_response_needs_tool_calls(&test_ctx, doc) ||
	    fyai_response_is_final(&test_ctx, doc)) {
		fprintf(stderr, "responses tool call detection failed\n");
		exit(1);
	}

	test_cfg.api_mode = FYAI_API_CHAT_COMPLETIONS;
	doc = parse(
		"{\"id\": \"cc\",\"choices\":[{\"message\":"
		"{\"role\": \"assistant\",\"content\": \"plain answer\"}}]}");
	text = fy_cast(fyai_response_output_text(&test_ctx, doc), "");
	if (strcmp(text, "plain answer") ||
	    !fyai_response_is_final(&test_ctx, doc)) {
		fprintf(stderr, "chat accessors failed: '%s'\n", text);
		exit(1);
	}
}

/* Canonical (mixed-provenance) turn -> Anthropic Messages shape. */
static void test_messages_input(void)
{
	fy_generic messages;
	const char *out;

	test_cfg.api_mode = FYAI_API_MESSAGES;

	messages = parse(
		"["
		"{\"role\": \"system\",\"content\": \"sys prompt\"},"
		"{\"role\": \"user\",\"content\": \"question\"},"
		"{\"type\": \"reasoning\",\"id\": \"rs_1\"},"
		"{\"type\": \"function_call\",\"call_id\": \"c1\","
		"\"name\": \"read_file\",\"arguments\": \"{\\\"path\\\": \\\"f\\\"}\"},"
		"{\"type\": \"function_call_output\",\"call_id\": \"c1\","
		"\"output\": \"file data\"},"
		"{\"role\": \"assistant\",\"content\": null,\"tool_calls\": ["
		"{\"id\": \"c2\",\"type\": \"function\",\"function\":"
		"{\"name\": \"shell\",\"arguments\": \"{\\\"command\\\": \\\"ls\\\"}\"}}]},"
		"{\"role\": \"tool\",\"tool_call_id\": \"c2\",\"content\": \"listing\"},"
		"{\"role\": \"assistant\",\"content\": \"the answer\"}"
		"]");

	out = emit(fyai_messages_input(&test_ctx, messages));

	/* system travels in the request `system` field, reasoning is dropped */
	expect_absent("messages_input", out, "\"system\"");
	expect_absent("messages_input", out, "reasoning");
	/* every canonical tool shape lands as tool_use / tool_result blocks */
	expect_contains("messages_input", out, "\"type\": \"tool_use\"");
	expect_contains("messages_input", out, "\"id\": \"c1\"");
	expect_contains("messages_input", out,
			"\"input\": {\"path\": \"f\"}");
	expect_contains("messages_input", out, "\"type\": \"tool_result\"");
	expect_contains("messages_input", out, "\"tool_use_id\": \"c1\"");
	expect_contains("messages_input", out, "\"content\": [{\"type\": \"tool_result\", \"tool_use_id\": \"c1\", \"content\": \"file data\"}");
	expect_contains("messages_input", out, "\"id\": \"c2\"");
	expect_contains("messages_input", out,
			"\"input\": {\"command\": \"ls\"}");
	expect_contains("messages_input", out, "\"tool_use_id\": \"c2\"");
	expect_contains("messages_input", out, "the answer");
	/* no Chat wire detail may leak through */
	expect_absent("messages_input", out, "tool_calls");
	expect_absent("messages_input", out, "tool_call_id");
	/* the moving prompt-cache breakpoint rides the last history block */
	expect_contains("messages_input", out,
			"\"text\": \"the answer\", \"cache_control\": "
			"{\"type\": \"ephemeral\"}");
}

/* Per-token extents from logprob entries: bytes-driven lengths, offsets. */
static void test_token_extents(void)
{
	struct fy_generic_builder *gb = test_ctx.transient_gb;
	fy_generic entries, extents, ext, chunks;
	size_t pos;

	pos = 0;
	extents = fy_seq_empty;

	/* `bytes` is authoritative: "é" is 2 bytes though the text shows 1
	 * char; a missing bytes array falls back to strlen; empty tokens are
	 * skipped; a second batch continues from the accumulated offset. */
	entries = parse(
		"["
		"{\"token\": \"Hi\",\"logprob\": -0.25,\"bytes\": [72,105]},"
		"{\"token\": \"é\",\"logprob\": -1.5,\"bytes\": [195,169]},"
		"{\"token\": \"\",\"logprob\": 0.0,\"bytes\": []}"
		"]");
	extents = fyai_token_extents_append(gb, extents, entries, &pos);
	entries = parse("[{\"token\": \"!\",\"logprob\": -0.5}]");
	extents = fyai_token_extents_append(gb, extents, entries, &pos);

	if (fy_len(extents) != 3 || pos != 5) {
		fprintf(stderr, "token extents: len %zu pos %zu\n",
			(size_t)fy_len(extents), pos);
		exit(1);
	}
	ext = fy_get(extents, 1, fy_invalid);
	if (strcmp(fy_get(ext, "text", ""), "é") ||
	    fy_get(ext, "pos", -1LL) != 2 ||
	    fy_get(ext, "lp", 0.0) != -1.5) {
		fprintf(stderr, "token extents entry 1: %s\n", emit(extents));
		exit(1);
	}
	ext = fy_get(extents, 2, fy_invalid);
	if (fy_get(ext, "pos", -1LL) != 4) {
		fprintf(stderr, "token extents entry 2: %s\n", emit(extents));
		exit(1);
	}

	/* chunk fallback: one {text,pos} per chunk, running join offsets */
	chunks = parse("[\"for\",\" sure\",\"\",\"!\"]");
	extents = fyai_chunk_extents(gb, chunks);
	if (fy_len(extents) != 3 ||
	    fy_get(fy_get(extents, 1, fy_invalid), "pos", -1LL) != 3 ||
	    fy_get(fy_get(extents, 2, fy_invalid), "pos", -1LL) != 8) {
		fprintf(stderr, "chunk extents: %s\n", emit(extents));
		exit(1);
	}
	ext = fy_get(extents, 0, fy_invalid);
	if (fy_generic_is_valid(fy_get(ext, "lp", fy_invalid))) {
		fprintf(stderr, "chunk extents carry lp: %s\n", emit(extents));
		exit(1);
	}
}

/* Anthropic response doc -> normalized canonical items + usage. */
static void test_messages_response(void)
{
	fy_generic doc, calls, usage;
	const char *out;
	const char *text;

	test_cfg.api_mode = FYAI_API_MESSAGES;

	doc = parse(
		"{\"id\": \"msg_1\",\"type\": \"message\",\"role\": \"assistant\","
		"\"content\": ["
		"{\"type\": \"text\",\"text\": \"part one \"},"
		"{\"type\": \"tool_use\",\"id\": \"toolu_1\","
		"\"name\": \"read_file\",\"input\": {\"path\": \"f\"}},"
		"{\"type\": \"text\",\"text\": \"part two\"}"
		"],"
		"\"stop_reason\": \"tool_use\","
		"\"usage\": {\"input_tokens\": 50,\"output_tokens\": 20,"
		"\"cache_read_input_tokens\": 30,"
		"\"cache_creation_input_tokens\": 5}}");

	text = fy_cast(fyai_response_output_text(&test_ctx, doc), "");
	if (strcmp(text, "part one part two")) {
		fprintf(stderr, "messages output_text: got '%s'\n", text);
		exit(1);
	}

	calls = fyai_response_tool_calls(&test_ctx, doc);
	if (fy_len(calls) != 1 || fyai_response_is_final(&test_ctx, doc)) {
		fprintf(stderr, "messages tool call detection failed\n");
		exit(1);
	}
	/* tool_use normalizes to a function_call item with re-encoded args */
	out = emit(calls);
	expect_contains("messages_response", out,
			"\"type\": \"function_call\"");
	expect_contains("messages_response", out, "\"call_id\": \"toolu_1\"");
	expect_contains("messages_response", out,
			"\"arguments\": \"{\\\"path\\\": \\\"f\\\"}\"");

	usage = fyai_extract_usage(&test_ctx, doc);
	if (fy_get(usage, "input", 0LL) != 50 ||
	    fy_get(usage, "output", 0LL) != 20 ||
	    fy_get(usage, "cached", 0LL) != 30 ||
	    fy_get(usage, "cache_write", 0LL) != 5 ||
	    fy_get(usage, "total", 0LL) != 70) {
		fprintf(stderr, "messages usage mismatch: %s\n", emit(usage));
		exit(1);
	}
}

int main(void)
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

	test_responses_input();
	test_chat_input();
	test_extract_usage();
	test_response_accessors();
	test_token_extents();
	test_messages_input();
	test_messages_response();

	fy_generic_builder_destroy(test_ctx.transient_gb);
	return 0;
}
