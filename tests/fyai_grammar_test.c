/*
 * fyai_grammar_test.c - unit tests for the provider API grammars
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_grammar.h"
#include "fyai_provider.h"
#include "utils.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(grammar, lookup, grammar_lookup)
FYAI_TEST_ENTRY(grammar, canonical, grammar_canonical)
FYAI_TEST_ENTRY(grammar, reply_round_trip, grammar_reply_round_trip)
FYAI_TEST_ENTRY(grammar, result_round_trip, grammar_result_round_trip)
FYAI_TEST_ENTRY(grammar, prefix_stable, grammar_prefix_stable)
FYAI_TEST_ENTRY(grammar, cross_prefix_stable, grammar_cross_prefix_stable)
FYAI_TEST_ENTRY(grammar, cycle, grammar_cycle)
FYAI_TEST_ENTRY(grammar, chat_openrouter_search, grammar_chat_openrouter_search)
FYAI_TEST_ENTRY(grammar, history_round_trip, grammar_history_round_trip)
FYAI_TEST_ENTRY(grammar, history_losses, grammar_history_losses)
FYAI_TEST_ENTRY(grammar, temperature, grammar_temperature)
FYAI_TEST_ENTRY(grammar, paused_turn, grammar_paused_turn)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;

#define GRAMMARS 3

static const struct fyai_grammar *const test_grammars[GRAMMARS] = {
	&fyai_grammar_responses,
	&fyai_grammar_chat,
	&fyai_grammar_messages,
};

static void fail(const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, fmt, a, b);
	fputc('\n', stderr);
	exit(1);
}

static fy_generic parse(const char *json)
{
	fy_generic v;

	v = parse_json_string(test_ctx.transient_gb, json);
	if (fy_is_invalid(v))
		fail("failed to parse fixture: %s%s", json, "");
	return v;
}

static const char *emit(fy_generic v)
{
	const char *s;

	s = emit_json_string(test_ctx.transient_gb, v);
	if (!s)
		fail("failed to emit a generic%s%s", "", "");
	return s;
}

/*
 * Two replies of each grammar that say the same thing: a turn with text
 * and two parallel calls, and a final answer. The arguments carry keys out
 * of order and a nested value, so a reordering or re-spelling shows.
 */
static const char *const reply_calls[GRAMMARS] = {
	"{\"id\":\"resp_1\",\"output\":["
	"{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
	"{\"type\":\"output_text\",\"text\":\"Let me look.\"}]},"
	"{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"b.txt\\\",\\\"limit\\\":[1,2]}\"},"
	"{\"type\":\"function_call\",\"call_id\":\"c2\",\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}],"
	"\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}",

	"{\"id\":\"cc_1\",\"choices\":[{\"index\":0,\"message\":"
	"{\"role\":\"assistant\",\"content\":\"Let me look.\",\"tool_calls\":["
	"{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"b.txt\\\",\\\"limit\\\":[1,2]}\"}},"
	"{\"id\":\"c2\",\"type\":\"function\",\"function\":{\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}]},"
	"\"finish_reason\":\"tool_calls\"}],"
	"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}",

	"{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
	"\"content\":["
	"{\"type\":\"text\",\"text\":\"Let me look.\"},"
	"{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"read_file\","
	"\"input\":{\"path\":\"b.txt\",\"limit\":[1,2]}},"
	"{\"type\":\"tool_use\",\"id\":\"c2\",\"name\":\"read_file\","
	"\"input\":{\"path\":\"a.txt\"}}],"
	"\"stop_reason\":\"tool_use\","
	"\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}",
};

static const char *const reply_final[GRAMMARS] = {
	"{\"id\":\"resp_2\",\"output\":["
	"{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
	"{\"type\":\"output_text\",\"text\":\"Both files are short.\"}]}]}",

	"{\"id\":\"cc_2\",\"choices\":[{\"index\":0,\"message\":"
	"{\"role\":\"assistant\",\"content\":\"Both files are short.\"},"
	"\"finish_reason\":\"stop\"}]}",

	"{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\","
	"\"content\":[{\"type\":\"text\",\"text\":\"Both files are short.\"}],"
	"\"stop_reason\":\"end_turn\"}",
};

/*
 * The assistant turn of reply_calls as each grammar sends it back. A
 * provider caches a prefix of the request, so the turn must go back as the
 * provider sent it.
 */
static const char *const wire_calls[GRAMMARS] = {
	"[{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
	"{\"type\":\"output_text\",\"text\":\"Let me look.\"}]},"
	"{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"b.txt\\\",\\\"limit\\\":[1,2]}\"},"
	"{\"type\":\"function_call\",\"call_id\":\"c2\",\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}]",

	"[{\"role\":\"assistant\",\"content\":\"Let me look.\",\"tool_calls\":["
	"{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"b.txt\\\",\\\"limit\\\":[1,2]}\"}},"
	"{\"id\":\"c2\",\"type\":\"function\",\"function\":{\"name\":\"read_file\","
	"\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}]}]",

	"[{\"role\":\"assistant\",\"content\":["
	"{\"type\":\"text\",\"text\":\"Let me look.\"},"
	"{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"read_file\","
	"\"input\":{\"path\":\"b.txt\",\"limit\":[1,2]}},"
	"{\"type\":\"tool_use\",\"id\":\"c2\",\"name\":\"read_file\","
	"\"input\":{\"path\":\"a.txt\"}}]}]",
};

/* The results of the two calls, as each grammar sends them. */
static const char *const wire_results[GRAMMARS] = {
	"[{\"type\":\"function_call_output\",\"call_id\":\"c1\","
	"\"output\":\"one line\"},"
	"{\"type\":\"function_call_output\",\"call_id\":\"c2\","
	"\"output\":\"two lines\"}]",

	"[{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"one line\"},"
	"{\"role\":\"tool\",\"tool_call_id\":\"c2\",\"content\":\"two lines\"}]",

	"[{\"role\":\"user\",\"content\":["
	"{\"type\":\"tool_result\",\"tool_use_id\":\"c1\","
	"\"content\":\"one line\"},"
	"{\"type\":\"tool_result\",\"tool_use_id\":\"c2\","
	"\"content\":\"two lines\"}]}]",
};

static const char *const history_start =
	"[{\"role\":\"system\",\"content\":\"be brief\"},"
	"{\"role\":\"user\",\"content\":\"read both files\"}]";

static const char *const tool_results =
	"[{\"type\":\"function_call_output\",\"call_id\":\"c1\","
	"\"output\":\"one line\"},"
	"{\"type\":\"function_call_output\",\"call_id\":\"c2\","
	"\"output\":\"two lines\"}]";

static const char *const user_line =
	"[{\"role\":\"user\",\"content\":\"thanks\"}]";

static void use_grammar(const struct fyai_grammar *grammar)
{
	test_cfg.api_mode = grammar->mode;
}

/* The canonical response of @json, a reply of @grammar. */
static fy_generic reply(const struct fyai_grammar *grammar, const char *json)
{
	fy_generic response;

	use_grammar(grammar);
	response = fyai_response_canonical(&test_ctx, parse(json));
	if (fy_is_invalid(response))
		fail("%s: could not read the reply %s", grammar->name, json);
	return response;
}

/* The request input of @grammar for the canonical @messages. */
static fy_generic input(const struct fyai_grammar *grammar,
			fy_generic messages)
{
	fy_generic out;

	use_grammar(grammar);
	out = fyai_grammar_input(&test_ctx, messages);
	if (fy_is_invalid(out))
		fail("%s: could not build the input%s", grammar->name, "");
	return out;
}

/*
 * The cache_control marker of Messages moves to the last block of every
 * request. A provider ignores it when it matches a prefix, so a compare
 * leaves it out.
 */
static fy_generic uncached(fy_generic messages)
{
	struct fy_generic_builder *gb = test_ctx.transient_gb;
	fy_generic out, m, content, blocks, block;

	out = fy_seq_empty;
	fy_foreach(m, messages) {
		content = fy_get(m, "content");
		if (fy_is_sequence(content)) {
			blocks = fy_seq_empty;
			fy_foreach(block, content) {
				if (fy_is_mapping(block) &&
				    fy_is_valid(fy_get(block, "cache_control",
						       fy_invalid)))
					block = fy_disassoc(gb, block,
							    "cache_control");
				blocks = fy_append(gb, blocks, block);
			}
			m = fy_assoc(gb, m, "content", blocks);
		}
		out = fy_append(gb, out, m);
	}
	return out;
}

/* The messages of @seq from @start, as one sequence. */
static fy_generic tail(fy_generic seq, size_t start)
{
	fy_generic out = fy_seq_empty;
	size_t i;

	for (i = start; i < fy_len(seq); i++)
		out = fy_append(test_ctx.transient_gb, out, fy_get_at(seq, i));
	return out;
}

/*
 * Every message of request @a goes again in request @b, byte for byte. The
 * last message of @a can gain blocks in @b: Messages merges the blocks of a
 * role into one message, and a provider still matches the blocks before.
 */
static void expect_prefix(const char *what, fy_generic a, fy_generic b)
{
	fy_generic ma, mb, ca, cb;
	size_t i, j, n;

	n = fy_len(a);
	if (fy_len(b) < n)
		fail("%s: the next request is shorter:\n%s", what, emit(b));
	for (i = 0; i < n; i++) {
		ma = fy_get_at(a, i);
		mb = fy_get_at(b, i);
		if (!strcmp(emit(ma), emit(mb)))
			continue;
		ca = fy_get(ma, "content");
		cb = fy_get(mb, "content");
		if (i + 1 != n || !fy_is_sequence(ca) || !fy_is_sequence(cb) ||
		    fy_len(cb) < fy_len(ca) ||
		    fy_not_equal(fy_get(ma, "role"), fy_get(mb, "role")))
			fail("%s: a sent message changed:\n%s", what,
			     emit(ma));
		for (j = 0; j < fy_len(ca); j++)
			if (strcmp(emit(fy_get_at(ca, j)),
				   emit(fy_get_at(cb, j))))
				fail("%s: a sent block changed:\n%s", what,
				     emit(fy_get_at(ca, j)));
	}
}

/* The first @n messages of @seq. */
static fy_generic head(fy_generic seq, size_t n)
{
	fy_generic out = fy_seq_empty;
	size_t i;

	for (i = 0; i < n && i < fy_len(seq); i++)
		out = fy_append(test_ctx.transient_gb, out, fy_get_at(seq, i));
	return out;
}

static fy_generic concat(fy_generic a, fy_generic b)
{
	return fy_concat(test_ctx.transient_gb, a, b);
}

static bool compacts(const struct fyai_grammar *grammar)
{
	use_grammar(grammar);
	return fyai_grammar_compacts(&test_cfg);
}

/* Each grammar names itself, its alias and its catalogue protocol. */
static void test_lookup(void)
{
	const struct fyai_grammar *grammar;
	size_t i;

	if (fyai_grammar_find("responses") != &fyai_grammar_responses ||
	    fyai_grammar_find("chat-completions") != &fyai_grammar_chat ||
	    fyai_grammar_find("chat") != &fyai_grammar_chat ||
	    fyai_grammar_find("messages") != &fyai_grammar_messages ||
	    fyai_grammar_find("chat_completions") ||
	    fyai_grammar_find("") || fyai_grammar_find(NULL) ||
	    !fyai_grammar_default_url(OPENAI_RESPONSES_URL) ||
	    !fyai_grammar_default_url(ANTHROPIC_MESSAGES_URL) ||
	    fyai_grammar_default_url("http://127.0.0.1/v1/responses") ||
	    fyai_grammar_default_url(NULL))
		fail("grammar names do not resolve%s%s", "", "");
	i = 0;
	fyai_grammar_foreach(grammar) {
		if (i >= GRAMMARS || grammar != test_grammars[i] ||
		    fyai_grammar_get(grammar->mode) != grammar ||
		    strcmp(fyai_api_to_string(grammar->mode), grammar->name))
			fail("%s: the table is out of order%s",
			     grammar->name, "");
		i++;
	}
	if (i != FYAI_API_COUNT)
		fail("the table does not hold every grammar%s%s", "", "");
	if (strcmp(fyai_grammar_chat.protocol, "chat_completions") ||
	    !fyai_grammar_messages.key_header ||
	    fyai_grammar_responses.key_header ||
	    !compacts(&fyai_grammar_responses) ||
	    compacts(&fyai_grammar_chat) ||
	    fyai_grammar_messages.reasoning || fyai_grammar_messages.logprobs)
		fail("grammar properties are wrong%s%s", "", "");
}

/* The replies of every grammar read as the same canonical response. */
static void test_canonical(void)
{
	fy_generic r, calls, call, args;
	const char *first = NULL;
	const char *text;
	size_t i;

	for (i = 0; i < GRAMMARS; i++) {
		r = reply(test_grammars[i], reply_calls[i]);
		text = fy_get(r, "text", "");
		calls = fy_get(r, "calls");
		if (strcmp(text, "Let me look.") || fy_get(r, "final", true) ||
		    fy_len(calls) != 2)
			fail("%s: wrong reply %s", test_grammars[i]->name,
			     emit(r));
		/* The arguments are a JSON string with the same value. */
		call = fy_get_at(calls, 0);
		args = parse(fy_get(call, "arguments", ""));
		if (fy_not_equal(fy_get(call, "type"), "function_call") ||
		    fy_not_equal(fy_get(call, "call_id"), "c1") ||
		    fy_not_equal(fy_get(call, "name"), "read_file") ||
		    fy_not_equal(fy_get(args, "path"), "b.txt"))
			fail("%s: wrong call %s", test_grammars[i]->name,
			     emit(call));
		if (fy_get(fy_get(r, "usage"), "input", 0LL) != 10 ||
		    fy_get(fy_get(r, "usage"), "output", 0LL) != 5)
			fail("%s: wrong usage %s", test_grammars[i]->name,
			     emit(fy_get(r, "usage")));
		if (fy_is_null(fy_get(r, "wire")))
			fail("%s: no wire record%s", test_grammars[i]->name,
			     "");

		/* A stored turn has one shape: the calls are items. */
		if (strstr(emit(fy_get(r, "messages")), "tool_calls"))
			fail("%s: a stored turn has the Chat shape: %s",
			     test_grammars[i]->name,
			     emit(fy_get(r, "messages")));

		r = reply(test_grammars[i], reply_final[i]);
		if (!fy_get(r, "final", false) || fy_len(fy_get(r, "calls")) ||
		    strcmp(emit(fy_get(r, "messages")),
			   "[{\"role\": \"assistant\", "
			   "\"content\": \"Both files are short.\"}]")) {
			text = emit(fy_get(r, "messages"));
			fail("%s: wrong final answer %s",
			     test_grammars[i]->name, text);
		}
		text = emit(fy_get(reply(test_grammars[i], reply_calls[i]),
				   "calls"));
		if (first && test_grammars[i] != &fyai_grammar_messages &&
		    strcmp(first, text))
			fail("calls differ between grammars:\n%s\n%s",
			     first, text);
		if (!first)
			first = text;
	}
}

/*
 * A reply stored and built again for its own grammar is the message the
 * provider sent, so the next request starts with the bytes the provider
 * has cached.
 */
static void test_reply_round_trip(void)
{
	const struct fyai_grammar *g;
	fy_generic start, history, out, want;
	size_t i;

	for (i = 0; i < GRAMMARS; i++) {
		g = test_grammars[i];
		start = parse(history_start);
		history = concat(start,
			fy_get(reply(g, reply_calls[i]), "messages"));
		/* The tool results follow, so the turn is not the last. */
		history = concat(history, parse(tool_results));
		out = uncached(input(g, history));
		want = parse(wire_calls[i]);
		/* What follows the start of the history is the turn. */
		out = tail(out, fy_len(uncached(input(g, start))));
		out = head(out, fy_len(want));
		if (strcmp(emit(out), emit(want)))
			fail("the turn is not sent as it came:\n%s\n%s",
			     emit(want), emit(out));
	}
}

/* A tool result is sent the same each time, in the shape of the grammar. */
static void test_result_round_trip(void)
{
	const struct fyai_grammar *g;
	fy_generic results, out;
	const char *first;
	size_t i;

	results = parse(tool_results);
	for (i = 0; i < GRAMMARS; i++) {
		g = test_grammars[i];
		out = uncached(input(g, results));
		first = emit(out);
		if (strcmp(first, emit(parse(wire_results[i]))))
			fail("the results are not sent as expected:\n%s\n%s",
			     emit(parse(wire_results[i])), first);
		if (strcmp(first, emit(uncached(input(g, results)))))
			fail("%s: the input changes between builds%s",
			     g->name, "");
	}
}

/*
 * Three requests of one conversation: the question, the results of its
 * calls, and a later line. Each request starts with the one before.
 */
static void conversation(const struct fyai_grammar *from,
			 const char *calls_json, const char *final_json,
			 fy_generic *h1, fy_generic *h2, fy_generic *h3)
{
	*h1 = parse(history_start);
	*h2 = concat(concat(*h1, fy_get(reply(from, calls_json), "messages")),
		     parse(tool_results));
	*h3 = concat(concat(*h2, fy_get(reply(from, final_json), "messages")),
		     parse(user_line));
}

/*
 * Messages caches up to one breakpoint, on the last block of the request:
 * moving it forward each turn extends what the provider has cached.
 */
static void check_breakpoint(fy_generic request)
{
	fy_generic last, content;
	const char *text, *mark;

	last = fy_get_at(request, fy_len(request) - 1);
	content = fy_get(last, "content");
	text = emit(request);
	mark = strstr(text, "cache_control");
	if (fy_is_invalid(fy_get(fy_get_at(content, fy_len(content) - 1),
				 "cache_control", fy_invalid)) ||
	    !mark || strstr(mark + 1, "cache_control"))
		fail("messages: the cache breakpoint is misplaced:\n%s%s",
		     text, "");
}

static void check_prefixes(const struct fyai_grammar *to, fy_generic h1,
			   fy_generic h2, fy_generic h3)
{
	fy_generic r1, r2, r3;

	r1 = input(to, h1);
	r2 = input(to, h2);
	r3 = input(to, h3);
	expect_prefix(to->name, uncached(r1), uncached(r2));
	expect_prefix(to->name, uncached(r2), uncached(r3));
	if (strcmp(emit(r3), emit(input(to, h3))))
		fail("%s: the input changes between builds%s", to->name, "");
	if (to != &fyai_grammar_messages)
		return;
	check_breakpoint(r1);
	check_breakpoint(r2);
	check_breakpoint(r3);
}

static void test_prefix_stable(void)
{
	fy_generic h1, h2, h3;
	size_t i;

	for (i = 0; i < GRAMMARS; i++) {
		conversation(test_grammars[i], reply_calls[i], reply_final[i],
			     &h1, &h2, &h3);
		check_prefixes(test_grammars[i], h1, h2, h3);
	}
}

/* A conversation stored under one grammar continues on each other one. */
static void test_cross_prefix_stable(void)
{
	fy_generic h1, h2, h3;
	size_t i, j;

	for (i = 0; i < GRAMMARS; i++) {
		conversation(test_grammars[i], reply_calls[i], reply_final[i],
			     &h1, &h2, &h3);
		for (j = 0; j < GRAMMARS; j++)
			check_prefixes(test_grammars[j], h1, h2, h3);
	}
}

/*
 * The canonical fields of a response: what the core reads. The id, the
 * wire record and the reply document are of the provider. The arguments
 * are compared as values: Responses and Chat keep the string of the
 * provider, which must go back unchanged, and Messages encodes `input`.
 */
static const char *canonical_of(fy_generic r)
{
	struct fy_generic_builder *gb = test_ctx.transient_gb;
	fy_generic calls, call, in;

	calls = fy_seq_empty;
	in = fy_get(r, "calls", fy_seq_empty);
	fy_foreach(call, in)
		calls = fy_append(gb, calls, fy_assoc(gb, call, "arguments",
				parse(fy_get(call, "arguments", "{}"))));
	return emit(fy_mapping(gb,
		"text", fy_get(r, "text"),
		"calls", calls,
		"final", fy_get(r, "final"),
		"usage", fy_get(r, "usage")));
}

/*
 * One conversation that moves to the next grammar at each turn. Each reply
 * reads as the same canonical response, whichever grammar answered, and the
 * history it leaves builds for the grammar that answers next.
 */
static void test_cycle(void)
{
	const struct fyai_grammar *g, *next;
	fy_generic history, r;
	const char *want_calls, *want_final, *got;
	size_t turn, i;

	want_calls = canonical_of(reply(test_grammars[0], reply_calls[0]));
	want_final = canonical_of(reply(test_grammars[0], reply_final[0]));
	history = parse(history_start);
	for (turn = 0; turn < 3 * GRAMMARS; turn++) {
		i = turn % GRAMMARS;
		g = test_grammars[i];
		next = test_grammars[(turn + 1) % GRAMMARS];

		r = reply(g, reply_calls[i]);
		got = canonical_of(r);
		if (strcmp(got, want_calls))
			fail("the reply is not canonical:\n%s\n%s",
			     want_calls, got);
		history = concat(concat(history, fy_get(r, "messages")),
				 parse(tool_results));
		(void)input(next, history);

		r = reply(g, reply_final[i]);
		got = canonical_of(r);
		if (strcmp(got, want_final))
			fail("the answer is not canonical:\n%s\n%s",
			     want_final, got);
		history = concat(concat(history, fy_get(r, "messages")),
				 parse(user_line));
		(void)input(next, history);
	}
	/* The whole history builds for each grammar the same each time. */
	for (i = 0; i < GRAMMARS; i++)
		if (strcmp(emit(input(test_grammars[i], history)),
			   emit(input(test_grammars[i], history))))
			fail("%s: the input changes between builds%s",
			     test_grammars[i]->name, "");
}

/* The Chat request of one user line, with the search and tools set. */
static fy_generic chat_request(const char *search_type, bool tools)
{
	struct fyai_grammar_request req = { 0 };

	use_grammar(&fyai_grammar_chat);
	test_cfg.model = "mock-model";
	test_cfg.web_search_type = search_type;
	test_cfg.web_search = true;
	test_cfg.web_search_supported = true;
	test_cfg.enable_tools = tools;
	test_cfg.top_logprobs = -1;
	test_ctx.tools = tools ? parse(
		"[{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
		"\"description\":\"read\",\"parameters\":{}}}]") :
		fy_seq_empty;
	req.messages = parse("[{\"role\":\"user\",\"content\":\"hi\"}]");
	req.instructions = "be brief";
	req.previous_response_id = fy_invalid;
	return fyai_grammar_build_request(&test_ctx, &req);
}

/*
 * An endpoint that names a tool type for its search, as OpenRouter does,
 * declares it as a tool. The function tools go beside it and do not replace
 * it; an endpoint without a type takes the search as a request option.
 */
static void test_chat_openrouter_search(void)
{
	const char *out;

	out = emit(chat_request("openrouter:web_search", true));
	if (!strstr(out, "openrouter:web_search") ||
	    !strstr(out, "read_file") || !strstr(out, "tool_choice") ||
	    strstr(out, "web_search_options"))
		fail("openrouter with tools: %s%s", out, "");

	out = emit(chat_request("openrouter:web_search", false));
	if (!strstr(out, "openrouter:web_search") ||
	    strstr(out, "read_file") || strstr(out, "tool_choice"))
		fail("openrouter without tools: %s%s", out, "");

	out = emit(chat_request(NULL, true));
	if (strstr(out, "openrouter:web_search") ||
	    !strstr(out, "read_file") || !strstr(out, "web_search_options"))
		fail("openai with tools: %s%s", out, "");
}

/*
 * The history of a grammar is the inverse of its input: canonical messages
 * sent in the wire of a grammar read back as the same canonical messages.
 * The system message travels outside the input of Responses and Messages.
 */
static void test_history_round_trip(void)
{
	const struct fyai_grammar *g;
	fy_generic h1, h2, h3, want, got;
	size_t i;

	for (i = 0; i < GRAMMARS; i++) {
		g = test_grammars[i];
		conversation(&fyai_grammar_messages, reply_calls[2],
			     reply_final[2], &h1, &h2, &h3);
		want = g == &fyai_grammar_chat ? h3 : tail(h3, 1);
		got = fyai_grammar_history(g, &test_ctx,
					   uncached(input(g, h3)), NULL);
		if (strcmp(emit(want), emit(got)))
			fail("the history does not read back:\n%s\n%s",
			     emit(want), emit(got));
	}
}

/*
 * An import keeps a placeholder for content the canonical form cannot hold
 * and reports it; a reply drops it.
 */
static void test_history_losses(void)
{
	fy_generic wire, got, losses;

	wire = parse("[{\"role\":\"user\",\"content\":["
		"{\"type\":\"text\",\"text\":\"see \"},"
		"{\"type\":\"document\",\"source\":{}},"
		"{\"type\":\"thinking\",\"thinking\":\"hm\"},"
		"{\"type\":\"text\",\"text\":\" this\"}]}]");
	losses = fy_seq_empty;
	got = fyai_grammar_history(&fyai_grammar_messages, &test_ctx, wire,
				   &losses);
	if (strcmp(emit(got), "[{\"role\": \"user\", "
		   "\"content\": \"see [content omitted] this\"}]") ||
	    strcmp(emit(losses), "[{\"kind\": \"unsupported_content\", "
		   "\"content_type\": \"document\"}]"))
		fail("import losses:\n%s\n%s", emit(got), emit(losses));

	got = fyai_grammar_history(&fyai_grammar_messages, &test_ctx, wire,
				   NULL);
	if (strcmp(emit(got), "[{\"role\": \"user\", "
		   "\"content\": \"see  this\"}]"))
		fail("reply losses:\n%s%s", emit(got), "");

	losses = fy_seq_empty;
	got = fyai_grammar_history(&fyai_grammar_responses, &test_ctx,
		parse("[{\"type\":\"reasoning\",\"summary\":[]},"
		      "{\"type\":\"image_generation_call\"}]"), &losses);
	if (fy_len(got) || fy_len(losses) != 1 ||
	    fy_not_equal(fy_get(fy_get_at(losses, 0), "kind"),
			 "unsupported_item"))
		fail("item losses:\n%s\n%s", emit(got), emit(losses));
}

/* A request carries the configured temperature as its value is written. */
static void test_temperature(void)
{
	struct fyai_grammar_request req = { 0 };
	const char *out;
	size_t i;

	test_cfg.model = "mock-model";
	test_cfg.temperature = 0.2;
	test_cfg.top_logprobs = -1;
	req.messages = parse("[{\"role\":\"user\",\"content\":\"hi\"}]");
	req.instructions = "be brief";
	req.previous_response_id = fy_invalid;
	req.temperature = true;
	for (i = 0; i < GRAMMARS; i++) {
		if (test_grammars[i] == &fyai_grammar_messages)
			continue;
		use_grammar(test_grammars[i]);
		/* The wire value is the double of 0.2, not that of a float. */
		out = emit(fyai_grammar_build_request(&test_ctx, &req));
		if (fy_get(parse(out), "temperature", 0.0) != 0.2)
			fail("%s: the temperature is not 0.2: %s",
			     test_grammars[i]->name, out);
	}
}

/*
 * A paused Messages turn keeps its content blocks and stores its call as a
 * call item too. Messages sends the blocks and the call once; the other
 * grammars send the words of the blocks and the call.
 */
static void test_paused_turn(void)
{
	fy_generic history, out;
	const char *text, *first;

	history = parse("[{\"role\":\"user\",\"content\":\"look it up\"},"
		"{\"role\":\"assistant\",\"content\":["
		"{\"type\":\"server_tool_use\",\"id\":\"srv_1\","
		"\"name\":\"web_search\",\"input\":{\"query\":\"fyai\"}},"
		"{\"type\":\"web_search_tool_result\",\"tool_use_id\":\"srv_1\","
		"\"content\":[]},"
		"{\"type\":\"text\",\"text\":\"Found it.\"},"
		"{\"type\":\"tool_use\",\"id\":\"call_9\",\"name\":\"read_file\","
		"\"input\":{\"path\":\"a.txt\"}}]},"
		"{\"type\":\"function_call\",\"call_id\":\"call_9\","
		"\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"},"
		"{\"type\":\"function_call_output\",\"call_id\":\"call_9\","
		"\"output\":\"hello\"}]");

	text = emit(input(&fyai_grammar_messages, history));
	first = strstr(text, "\"id\": \"call_9\"");
	if (!first || strstr(first + 1, "\"id\": \"call_9\"") ||
	    !strstr(text, "server_tool_use"))
		fail("messages: the paused turn is not sent once: %s%s",
		     text, "");

	out = input(&fyai_grammar_chat, history);
	text = emit(out);
	if (strstr(text, "server_tool_use") ||
	    !strstr(text, "\"content\": \"Found it.\", \"tool_calls\""))
		fail("chat: the paused turn is not words and a call: %s%s",
		     text, "");

	text = emit(input(&fyai_grammar_responses, history));
	if (strstr(text, "server_tool_use") || !strstr(text, "Found it.") ||
	    !strstr(text, "\"call_id\": \"call_9\""))
		fail("responses: the paused turn is not words and a call: %s%s",
		     text, "");
}

static int grammar_run(void (*testfn)(void))
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
	test_ctx.gb = test_ctx.transient_gb;
	test_ctx.durable_gb = test_ctx.transient_gb;
	test_cfg.gb = test_ctx.transient_gb;
	test_cfg.catalog = fy_invalid;
	test_ctx.tools_spec = fy_invalid;
	test_ctx.media_pending = fy_seq_empty;

	testfn();

	fy_generic_builder_destroy(test_ctx.transient_gb);
	return 0;
}

int grammar_lookup(void)
{
	return grammar_run(test_lookup);
}

int grammar_canonical(void)
{
	return grammar_run(test_canonical);
}

int grammar_reply_round_trip(void)
{
	return grammar_run(test_reply_round_trip);
}

int grammar_result_round_trip(void)
{
	return grammar_run(test_result_round_trip);
}

int grammar_prefix_stable(void)
{
	return grammar_run(test_prefix_stable);
}

int grammar_cross_prefix_stable(void)
{
	return grammar_run(test_cross_prefix_stable);
}

int grammar_cycle(void)
{
	return grammar_run(test_cycle);
}

int grammar_chat_openrouter_search(void)
{
	return grammar_run(test_chat_openrouter_search);
}

int grammar_history_round_trip(void)
{
	return grammar_run(test_history_round_trip);
}

int grammar_history_losses(void)
{
	return grammar_run(test_history_losses);
}

int grammar_temperature(void)
{
	return grammar_run(test_temperature);
}

int grammar_paused_turn(void)
{
	return grammar_run(test_paused_turn);
}
