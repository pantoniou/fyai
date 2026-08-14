/*
 * fyai_display.c - conversation views and interactive presentation
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fyai_branch.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_markdown.h"
#include "fyai_output.h"
#include "fyai_provider.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_textual.h"
#include "fyai_ui.h"
#include "fyai_turn.h"

static void fyai_print_user_turn(struct fyai_ctx *ctx, const char *line,
				 bool erase_prompt);

/* Prose must not contain a directive opener at column zero. */
/* Return the opener length and its escape run length. */
static size_t fyai_export_opener_len(const char *p, size_t *runp)
{
	const char *run;
	const char *tail;

	if (strncmp(p, FYAI_TEXTUAL_PREFIX, FYAI_TEXTUAL_PREFIX_LEN))
		return 0;
	run = p + FYAI_TEXTUAL_PREFIX_LEN;
	tail = run;
	while (*tail == 'x')
		tail++;
	/* Count the run before the separator is consumed. */
	*runp = (size_t)(tail - run);
	if (tail > run) {
		if (*tail != '-')
			return 0;
		tail++;
	}
	if (strncmp(tail, FYAI_TEXTUAL_TAIL, FYAI_TEXTUAL_TAIL_LEN))
		return 0;
	return (size_t)(tail - p) + FYAI_TEXTUAL_TAIL_LEN;
}

static void fyai_export_escaped_opener(FILE *fp, const char *opener,
				       size_t len, size_t run)
{
	fwrite(FYAI_TEXTUAL_PREFIX, 1, FYAI_TEXTUAL_PREFIX_LEN, fp);
	/* Extend the run by one x; a zero-length run also needs its separator. */
	fputc('x', fp);
	if (!run)
		fputc('-', fp);
	fwrite(opener + FYAI_TEXTUAL_PREFIX_LEN, 1,
	       len - FYAI_TEXTUAL_PREFIX_LEN, fp);
}

/* Insert one 'x' into an opener at column zero. */
static void fyai_export_body(FILE *fp, const char *text)
{
	const char *p, *start;
	size_t len;
	size_t run;
	bool at_column_zero;

	at_column_zero = true;
	for (p = text, start = text; *p; ) {
		len = 0;
		run = 0;
		if (!at_column_zero) {
			at_column_zero = *p == '\n';
			p++;
			continue;
		}
		len = fyai_export_opener_len(p, &run);
		if (!len) {
			at_column_zero = false;
			continue;
		}
		fwrite(start, 1, (size_t)(p - start), fp);
		fyai_export_escaped_opener(fp, p, len, run);
		p += len;
		start = p;
		at_column_zero = false;
	}
	fwrite(start, 1, (size_t)(p - start), fp);
}

/* Normalize the role without converting generic strings to temporary pointers. */
static fy_generic fyai_export_role(fy_generic message)
{
	fy_generic role;
	fy_generic type;

	role = fy_get(message, "role", fy_invalid);
	/* Chat grammar answers a call with role "tool". */
	if (fy_equal(role, "tool"))
		return fy_value("tool_result");
	if (fy_is_string(role) && fy_len(role))
		return role;
	type = fy_get(message, "type", fy_invalid);
	if (fy_equal(type, "message"))
		return fy_value("assistant");
	if (fyai_item_type_is_call(type))
		return fy_value("tool_call");
	if (fyai_item_type_is_call_output(type))
		return fy_value("tool_result");
	return fy_value("item");
}

/*
 * A tool result is a separate canonical message, and a turn ends between a
 * call and its result whenever the model stops to run the tool. The export
 * joins the two into one directive, so nothing in the file has to name a call:
 * no ordinal to renumber and no position to preserve under a merge. This table
 * holds the results, collected before emission because the result of a call is
 * written after it in the message stream.
 */
struct fyai_export_call {
	char *id;
	fy_generic output;
};

struct fyai_export_calls {
	struct fyai_export_call *items;
	size_t count;
	size_t capacity;
};

struct fyai_export_entries {
	fy_generic *items;
	size_t count;
	size_t capacity;
};

static void fyai_export_calls_cleanup(struct fyai_export_calls *calls)
{
	size_t i;

	for (i = 0; i < calls->count; i++)
		free(calls->items[i].id);
	free(calls->items);
}

static int fyai_export_call_add(struct fyai_ctx *ctx,
				struct fyai_export_calls *calls,
				const char *id, fy_generic output)
{
	struct fyai_export_call *items;
	size_t capacity;
	char *copy;

	if (!id || !*id)
		return 0;
	if (calls->count == calls->capacity) {
		capacity = calls->capacity ? calls->capacity * 2 : 8;
		items = realloc(calls->items, capacity * sizeof(*items));
		fyai_error_check(ctx, items, err, "export: out of memory");
		calls->items = items;
		calls->capacity = capacity;
	}
	copy = strdup(id);
	fyai_error_check(ctx, copy, err, "export: out of memory");
	calls->items[calls->count].id = copy;
	calls->items[calls->count++].output = output;
	return 0;
err:
	return -1;
}

/* The result recorded for @id, or fy_invalid when the call has none. */
static fy_generic fyai_export_call_find(const struct fyai_export_calls *calls,
					const char *id)
{
	size_t i;

	if (!id || !*id)
		return fy_invalid;
	for (i = 0; i < calls->count; i++)
		if (!strcmp(calls->items[i].id, id))
			return calls->items[i].output;
	return fy_invalid;
}

/* The result carried by @message, whichever grammar produced it. */
static fy_generic fyai_export_result_of(fy_generic message)
{
	fy_generic result;

	result = fy_get(message, "output", fy_invalid);
	if (fy_is_valid(result))
		return result;
	return fy_get(message, "content", fy_invalid);
}

static fy_generic fyai_export_result_id(fy_generic message)
{
	fy_generic type;

	type = fy_get(message, "type", fy_invalid);
	if (fyai_item_type_is_call_output(type))
		return fy_get(message, "call_id");
	if (fy_equal(fy_get(message, "role", fy_invalid), "tool"))
		return fy_get(message, "tool_call_id");
	return fy_invalid;
}

/* Record every tool result in @messages against the call it answers. */
static int fyai_export_collect_results(struct fyai_ctx *ctx,
				       struct fyai_export_calls *calls,
				       fy_generic messages)
{
	fy_generic message;
	fy_generic gid;
	fy_generic result;
	int rc;

	fy_foreach(message, messages) {
		gid = fyai_export_result_id(message);
		if (fy_is_invalid(gid))
			continue;
		result = fyai_export_result_of(message);
		fyai_error_check(ctx, fy_is_valid(result) &&
				 !fy_is_null(result), err,
				 "export: a tool result has no output");
		rc = fyai_export_call_add(ctx, calls, fy_castp(&gid, ""), result);
		fyai_error_check(ctx, !rc, err, "export: cannot record a tool result");
	}
	return 0;
err:
	return -1;
}

static void fyai_export_entries_cleanup(struct fyai_export_entries *entries)
{
	free(entries->items);
}

static int fyai_export_entry_add(struct fyai_ctx *ctx,
				 struct fyai_export_entries *entries,
				 fy_generic entry)
{
	fy_generic *items;
	size_t capacity;

	if (entries->count == entries->capacity) {
		capacity = entries->capacity ? entries->capacity * 2 : 16;
		items = realloc(entries->items, capacity * sizeof(*items));
		fyai_error_check(ctx, items, err, "export: out of memory");
		entries->items = items;
		entries->capacity = capacity;
	}
	entries->items[entries->count++] = entry;
	return 0;
err:
	return -1;
}

/* Emit a YAML directive. */
static int fyai_export_directive(struct fyai_ctx *ctx, FILE *fp,
				 fy_generic map)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic_sized_string text;
	fy_generic emitted;

	emitted = fy_gb_emit(gb, map, FYAI_YAML_EMIT_FLAGS, NULL);
	fyai_error_check(ctx, fy_is_valid(emitted), err,
			 "export: cannot format a directive");
	text = fy_castp(&emitted, fy_szstr_empty);
	fputs(FYAI_TEXTUAL_OPEN "\n", fp);
	fwrite(text.data, 1, text.size, fp);
	if (text.size && text.data[text.size - 1] != '\n')
		fputc('\n', fp);
	fputs(FYAI_TEXTUAL_CLOSE "\n", fp);
	return 0;
err:
	return -1;
}

/* Flatten a configuration into slash paths. */
static fy_generic fyai_export_config_flatten(struct fy_generic_builder *gb,
					     const char *prefix, fy_generic value,
					     fy_generic out)
{
	char path[512];
	fy_generic key;
	fy_generic val;

	fy_foreach_key_value(key, val, value) {
		if (!fy_is_string(key))
			continue;
		snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "",
			 fy_castp(&key, ""));
		if (fy_is_mapping(val) && !fy_empty(val))
			out = fyai_export_config_flatten(gb, path, val, out);
		else
			out = fy_assoc(gb, out, path, val);
	}
	return out;
}

/* Return the slash-path changes from @previous to @config. */
static fy_generic fyai_export_config_delta(struct fy_generic_builder *gb,
					   fy_generic config, fy_generic previous)
{
	fy_generic old_flat;
	fy_generic new_flat;
	fy_generic delta;
	fy_generic key;
	fy_generic val;

	old_flat = fyai_export_config_flatten(gb, "", previous, fy_map_empty);
	new_flat = fyai_export_config_flatten(gb, "", config, fy_map_empty);
	delta = fy_map_empty;
	fy_foreach_key_value(key, val, new_flat) {
		if (!fy_equal(fy_get(old_flat, key, fy_invalid), val))
			delta = fy_assoc(gb, delta, key, val);
	}
	fy_foreach(key, old_flat) {
		if (fy_is_invalid(fy_get(new_flat, key, fy_invalid)))
			delta = fy_assoc(gb, delta, key, fy_null);
	}
	return delta;
}

static int fyai_export_config(struct fyai_ctx *ctx, FILE *fp,
			      fy_generic config, fy_generic previous)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic delta;
	int rc;

	if (fy_is_valid(previous) && fy_equal(config, previous))
		return 0;
	/* Emit the complete first configuration and later changes. */
	if (fy_is_invalid(previous)) {
		rc = fyai_export_directive(ctx, fp,
			fy_mapping(gb, "kind", "config", "config", config));
		fyai_error_check(ctx, !rc, err,
				 "export: cannot write the configuration");
		fputs("## Configuration\n\n", fp);
		return 0;
	}
	delta = fyai_export_config_delta(gb, config, previous);
	if (!fy_generic_mapping_get_pair_count(delta))
		return 0;
	rc = fyai_export_directive(ctx, fp,
		fy_mapping(gb, "kind", "config-update", "config-update", delta));
	fyai_error_check(ctx, !rc, err,
			 "export: cannot write a configuration update");
	fputs("## Configuration update\n\n", fp);
	return 0;
err:
	return -1;
}

/* Emit a tool call with its result. */
static int fyai_export_tool_call(struct fyai_ctx *ctx, FILE *fp,
				 const struct fyai_export_calls *calls,
				 const char *id, const char *name,
				 fy_generic args)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic description;
	fy_generic result;
	fy_generic map;
	const char *desc;
	int rc;

	map = fy_mapping(gb, "kind", "tool_call", "tool", name,
			 "arguments", args);
	/* An interrupted turn can hold a call that never got an answer. */
	result = fyai_export_call_find(calls, id);
	if (fy_is_valid(result))
		map = fy_assoc(gb, map, "result", result);
	rc = fyai_export_directive(ctx, fp, map);
	fyai_error_check(ctx, !rc, err, "export: cannot write a tool call");
	description = fy_get(args, "description");
	desc = fy_castp(&description, "");
	fprintf(fp, "### %s%s%s\n\n", name, *desc ? " — " : "", desc);
	return 0;
err:
	return -1;
}

/* Emit a publish or turn boundary. */
static int fyai_export_marker(struct fyai_ctx *ctx, FILE *fp,
			      const char *kind)
{
	return fyai_export_directive(ctx, fp,
		fy_mapping(ctx->transient_gb, "kind", kind));
}

/* Emit a compaction operation without its provider output. */
static int fyai_export_compact(struct fyai_ctx *ctx, FILE *fp,
			       fy_generic instructions)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic map;
	int rc;

	map = fy_mapping(gb, "kind", "compact");
	if (fy_is_string(instructions))
		map = fy_assoc(gb, map, "instructions", instructions);
	rc = fyai_export_directive(ctx, fp, map);
	fyai_error_check(ctx, !rc, err, "export: cannot write a compaction");
	fputs("## Compaction\n\n", fp);
	return 0;
err:
	return -1;
}

static const char *fyai_export_turn_heading(const char *role)
{
	if (!strcmp(role, "assistant"))
		return "Assistant";
	if (!strcmp(role, "user"))
		return "User";
	if (!strcmp(role, "system"))
		return "System";
	return role;
}

static int fyai_export_message(struct fyai_ctx *ctx, FILE *fp,
			       const char *role, const char *text)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	int rc;

	rc = fyai_export_directive(ctx, fp,
		fy_mapping(gb, "kind", "message", "role", role));
	fyai_error_check(ctx, !rc, err, "export: cannot write a message");
	fprintf(fp, "## %s\n\n", fyai_export_turn_heading(role));
	fyai_export_body(fp, text);
	if (*text && text[strlen(text) - 1] != '\n')
		fputc('\n', fp);
	fputc('\n', fp);
	return 0;
err:
	return -1;
}

static fy_generic fyai_export_item_text(struct fy_generic_builder *gb,
						fy_generic message)
{
	fy_generic content, chunks, part, type;

	content = fy_get(message, "content");
	if (fy_is_string(content))
		return content;
	chunks = fy_seq_empty;
	fy_foreach(part, content) {
		type = fy_get(part, "type");
		if (fy_not_equal(type, "output_text") &&
		    fy_not_equal(type, "text"))
			continue;
		chunks = fy_append(gb, chunks, fy_get(part, "text", ""));
	}
	return fyai_join_strings(gb, chunks);
}

static int fyai_export_turn_messages(struct fyai_ctx *ctx, FILE *fp,
				     const struct fyai_export_calls *calls,
				     fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic message, content, calls_in_message, call, function;
	fy_generic args, gid, gname, role;
	const char *role_text, *text, *args_text;
	bool assistant_open = false;
	int rc;

	fy_foreach(message, messages) {
		if (fy_equal(fy_get(message, "type"), "function_call")) {
			if (!assistant_open) {
				rc = fyai_export_message(ctx, fp, "assistant", "");
				fyai_error_check(ctx, !rc, err,
						 "export: cannot write a message");
				assistant_open = true;
			}
			gid = fy_get(message, "call_id");
			gname = fy_get(message, "name");
			args_text = fy_get(message, "arguments", "{}");
			args = parse_json_string(gb, args_text);
			rc = fyai_export_tool_call(ctx, fp, calls,
				fy_castp(&gid, ""), fy_castp(&gname, "tool"), args);
			fyai_error_check(ctx, !rc, err,
					 "export: cannot write a tool call");
			continue;
		}
		if (fy_equal(fy_get(message, "type"), "shell_call")) {
			if (!assistant_open) {
				rc = fyai_export_message(ctx, fp, "assistant", "");
				fyai_error_check(ctx, !rc, err,
						 "export: cannot write a message");
				assistant_open = true;
			}
			gid = fy_get(message, "call_id");
			args = fy_mapping(gb, "command",
				fy_get_at_path(gb, message, "action", "commands", 0));
			rc = fyai_export_tool_call(ctx, fp, calls,
				fy_castp(&gid, ""), "shell", args);
			fyai_error_check(ctx, !rc, err,
					 "export: cannot write a tool call");
			continue;
		}
		if (fy_is_valid(fyai_export_result_id(message)))
			continue;

		role = fyai_export_role(message);
		role_text = fy_castp(&role, "item");
		if (fy_equal(role, "assistant")) {
			content = fy_get(message, "content", fy_invalid);
			if (fy_is_string(content) && *fy_castp(&content, "")) {
				rc = fyai_export_message(ctx, fp, role_text,
						      fy_castp(&content, ""));
				fyai_error_check(ctx, !rc, err,
						 "export: cannot write a message");
				assistant_open = true;
			}
			calls_in_message = fy_get(message, "tool_calls");
			if (fy_is_sequence(calls_in_message)) {
				if (!assistant_open) {
					rc = fyai_export_message(ctx, fp, role_text, "");
					fyai_error_check(ctx, !rc, err,
							 "export: cannot write a message");
					assistant_open = true;
				}
				fy_foreach(call, calls_in_message) {
					function = fy_get(call, "function");
					gid = fy_get(call, "id");
					gname = fy_get(function, "name");
					args_text = fy_get(function, "arguments", "{}");
					args = parse_json_string(gb, args_text);
					rc = fyai_export_tool_call(ctx, fp, calls,
						fy_castp(&gid, ""),
						fy_castp(&gname, "tool"), args);
					fyai_error_check(ctx, !rc, err,
							 "export: cannot write a tool call");
				}
			}
		}
		if (fy_equal(role, "tool_result"))
			continue;
		content = fy_get(message, "content", fy_invalid);
		if (fy_is_string(content))
			text = fy_castp(&content, "");
		else if (fy_equal(fy_get(message, "type"), "message")) {
			content = fyai_export_item_text(gb, message);
			text = fy_castp(&content, "");
		} else
			text = "";
		if (fy_not_equal(role, "tool_call") && fy_not_equal(role, "item") &&
		    !(assistant_open && fy_equal(role, "assistant") &&
		      fy_is_invalid(fy_get(message, "type")))) {
			rc = fyai_export_message(ctx, fp, role_text, text);
			fyai_error_check(ctx, !rc, err,
					 "export: cannot write a message");
			if (fy_equal(role, "assistant"))
				assistant_open = true;
		}
	}
	return 0;
err:
	return -1;
}

int fyai_export_view(struct fyai_ctx *ctx, const char *path)
{
	struct fyai_turn_stack stack;
	struct fy_generic_builder *gb;
	struct fyai_export_calls calls;
	struct fyai_export_entries entries;
	struct fyai_branch branch;
	fy_generic messages;
	const char *conversation_path;
	FILE *fp;
	size_t i, entry_index;
	int rc;
	int close_rc;
	fy_generic entry;
	fy_generic previous_config;
	fy_generic previous_head;
	fy_generic meta;
	bool decoded;

	memset(&stack, 0, sizeof(stack));
	memset(&calls, 0, sizeof(calls));
	memset(&entries, 0, sizeof(entries));
	fp = NULL;
	gb = ctx->transient_gb;
	assert(gb);
	rc = -1;
	/* No path is stdout, so an export composes with a pipe by default. */
	conversation_path = path ? path : "standard output";
	if (path) {
		fp = fopen(path, "wb");
		fyai_error_check(ctx, fp, out, "export: cannot write %s", path);
	} else
		fp = stdout;
	rc = fyai_export_directive(ctx, fp,
		fy_mapping(gb, "format", 1LL, "kind", "conversation"));
	fyai_error_check(ctx, !rc, out, "export: cannot write the document header");
	fputc('\n', fp);
	for (entry = ctx->branch_prev; fy_is_valid(entry);
	     entry = branch.prev) {
		decoded = fyai_branch_decode(entry, &branch);
		fyai_error_check(ctx, decoded, out,
				 "export: cannot read branch history");
		rc = fyai_export_entry_add(ctx, &entries, entry);
		fyai_error_check(ctx, !rc, out, "export: out of memory");
	}
	/* Collect results before their earlier calls are emitted. */
	previous_head = fy_invalid;
	for (entry_index = entries.count; entry_index-- > 0; ) {
		decoded = fyai_branch_decode(entries.items[entry_index], &branch);
		fyai_error_check(ctx, decoded, out,
			"export: cannot read branch history");
		rc = fyai_turn_stack_init(&stack, branch.head, previous_head);
		fyai_error_check(ctx, !rc, out, "export: cannot read conversation");
		for (i = 0; i < stack.count; i++) {
			messages = fy_get(stack.items[i], "messages", fy_seq_empty);
			rc = fyai_export_collect_results(ctx, &calls, messages);
			fyai_error_check(ctx, !rc, out, "export: out of memory");
		}
		fyai_turn_stack_cleanup(&stack);
		previous_head = branch.head;
	}
	previous_config = fy_invalid;
	previous_head = fy_invalid;
	for (entry_index = entries.count; entry_index-- > 0; ) {
		decoded = fyai_branch_decode(entries.items[entry_index], &branch);
		fyai_error_check(ctx, decoded, out,
			"export: cannot read branch history");
		rc = fyai_export_marker(ctx, fp, "publish");
		fyai_error_check(ctx, !rc, out, "export: cannot write a boundary");
		fputs("## Publish\n\n", fp);
		if (fy_is_valid(branch.config)) {
			rc = fyai_export_config(ctx, fp, branch.config, previous_config);
			fyai_error_check(ctx, !rc, out,
				"export: cannot format configuration");
		}
		rc = fyai_turn_stack_init(&stack, branch.head, previous_head);
		fyai_error_check(ctx, !rc, out, "export: cannot read conversation");
		for (i = 0; i < stack.count; i++) {
			messages = fy_get(stack.items[i], "messages", fy_seq_empty);
			meta = fyai_turn_meta(stack.items[i]);
			/* Replace provider compaction output with its operation. */
			if (fy_is_valid(fy_get(meta, "compacted_from",
						       fy_invalid))) {
				rc = fyai_export_compact(ctx, fp,
					fy_get(meta, "compact_instructions",
					       fy_invalid));
				fyai_error_check(ctx, !rc, out,
					"export: cannot write a compaction");
				continue;
			}
			rc = fyai_export_marker(ctx, fp, "turn");
			fyai_error_check(ctx, !rc, out, "export: cannot write a turn");
			fputs("## Turn\n\n", fp);
			rc = fyai_export_turn_messages(ctx, fp, &calls, messages);
			fyai_error_check(ctx, !rc, out, "export: cannot write a turn");
		}
		fyai_turn_stack_cleanup(&stack);
		previous_config = branch.config;
		previous_head = branch.head;
	}
	close_rc = fp == stdout ? fflush(fp) : fclose(fp);
	fp = NULL;
	fyai_error_check(ctx, !close_rc, out, "export: cannot write %s",
			 conversation_path);
	rc = 0;
out:
	if (fp && fp != stdout)
		fclose(fp);
	fyai_export_calls_cleanup(&calls);
	fyai_export_entries_cleanup(&entries);
	fyai_turn_stack_cleanup(&stack);
	return rc;
}

fy_generic fyai_stats_data(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	struct fy_allocator_usage arena;
	fy_generic out;
	fy_generic cur;
	fy_generic u;
	uint64_t generation;
	unsigned int chunks;
	long long cache_write;
	long long cached;
	long long input;
	long long output;
	long long reasoning;
	long long total;
	double cost;
	double ratio;
	double arena_ratio;
	bool have_arena;
	int calls;

	memset(&arena, 0, sizeof(arena));
	have_arena = ctx->durable_allocator &&
		!fy_allocator_get_usage(ctx->durable_allocator, FY_ALLOC_TAG_DEFAULT,
					&arena);
	generation = ctx->durable_allocator ?
		fy_allocator_generation(ctx->durable_allocator) : 0;
	chunks = ctx->durable_allocator ? fy_allocator_chunk_count(ctx->durable_allocator) : 0;
	input = 0;
	cached = 0;
	cache_write = 0;
	output = 0;
	reasoning = 0;
	total = 0;
	cost = 0.0;
	ratio = 0.0;
	arena_ratio = 0.0;
	calls = 0;

	fyai_turn_foreach(cur, ctx->last_message) {
		u = fy_get(fyai_turn_meta(cur), "usage");
		if (fy_is_invalid(u))
			continue;
		input += fy_get(u, "input", 0);
		cached += fy_get(u, "cached", 0);
		cache_write += fy_get(u, "cache_write", 0);
		output += fy_get(u, "output", 0);
		reasoning += fy_get(u, "reasoning", 0);
		total += fy_get(u, "total", 0);
		cost += fy_get(u, "cost", 0.0);
		calls++;
	}
	if (input)
		ratio = (double)cached * 100.0 / (double)input;
	if (have_arena && arena.total)
		arena_ratio = (double)arena.used * 100.0 / (double)arena.total;

	out = fy_null_filtered_mapping(gb,
			 "calls", calls,
			 "input", input,
			 "cached", cached,
			 "cached_percent", ratio,
			 "cache_write", cache_write,
			 "output", output,
			 "reasoning", reasoning,
			 "total", total,
			 "cost", cost,
			 "arena", have_arena ?
				fy_mapping(
					"used", arena.used,
					"free", arena.free,
					"total", arena.total,
					"used_percent", arena_ratio,
					"chunks", chunks,
					"generation", generation) :
				fy_null);

	return out;
}

static int fyai_emit_stats_markdown(struct fyai_ctx *ctx, fy_generic stats)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_stats_args *args = &cfg->cmd.args.stats;
	fy_generic arena;
	char *md;
	size_t mdlen;
	FILE *mf;

	md = NULL;
	mdlen = 0;
	mf = open_memstream(&md, &mdlen);
	if (!mf)
		return -1;

	fprintf(mf, "| Metric | Value |\n");
	fprintf(mf, "|---|---:|\n");
	fprintf(mf, "| Calls | %lld |\n", fy_get(stats, "calls", 0LL));
	fprintf(mf, "| Input tokens | %lld |\n", fy_get(stats, "input", 0LL));
	fprintf(mf, "| Cached tokens | %lld (%.1f%%) |\n",
		fy_get(stats, "cached", 0LL),
		fy_get(stats, "cached_percent", 0.0));
	fprintf(mf, "| Cache write tokens | %lld |\n",
		fy_get(stats, "cache_write", 0LL));
	fprintf(mf, "| Output tokens | %lld |\n", fy_get(stats, "output", 0LL));
	fprintf(mf, "| Reasoning tokens | %lld |\n",
		fy_get(stats, "reasoning", 0LL));
	fprintf(mf, "| Total tokens | %lld |\n", fy_get(stats, "total", 0LL));
	fprintf(mf, "| Cost | $%.6f |\n", fy_get(stats, "cost", 0.0));

	arena = fy_get(stats, "arena");
	if (!fy_is_invalid(arena)) {
		fprintf(mf, "\n| Arena | Value |\n");
		fprintf(mf, "|---|---:|\n");
		fprintf(mf, "| Used | %lld |\n", fy_get(arena, "used", 0LL));
		fprintf(mf, "| Free | %lld |\n", fy_get(arena, "free", 0LL));
		fprintf(mf, "| Total | %lld |\n", fy_get(arena, "total", 0LL));
		fprintf(mf, "| Used %% | %.1f%% |\n",
			fy_get(arena, "used_percent", 0.0));
		fprintf(mf, "| Chunks | %lld |\n", fy_get(arena, "chunks", 0LL));
		fprintf(mf, "| Generation | %lld |\n",
			fy_get(arena, "generation", 0LL));
	}
	fclose(mf);

	if (args->format == FYAIOF_RAW || fyai_print_markdown(md, cfg))
		fputs(md, stdout);
	free(md);
	return 0;
}

static int fyai_emit_stats_generic(fy_generic stats, bool json)
{
	enum fy_op_emit_flags flags;

	flags = FYOPEF_DISABLE_DIRECTORY | FYOPEF_OUTPUT_TYPE_STDOUT |
		FYOPEF_WIDTH_INF;
	if (json)
		flags |= FYOPEF_MODE_JSON | FYOPEF_STYLE_COMPACT;
	else
		flags |= FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_PRETTY;

	(void)fy_emit(stats, flags, NULL);
	putchar('\n');
	return 0;
}

/*
 * Sum the per-turn `usage` metadata along the conversation chain and report
 * the cumulative token/cost totals. Read-only; no model call.
 */
int fyai_show_stats(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_stats_args *args = &cfg->cmd.args.stats;
	struct fy_generic_builder_cfg gcfg;
	struct fy_generic_builder *gb;
	fy_generic stats;
	int rc;

	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	gb = fy_generic_builder_create(&gcfg);
	if (!gb)
		return -1;

	stats = fyai_stats_data(ctx, gb);
	if (fy_is_invalid(stats)) {
		fy_generic_builder_destroy(gb);
		return -1;
	}

	switch (args->format) {
	case FYAIOF_JSON:
		rc = fyai_emit_stats_generic(stats, true);
		break;
	case FYAIOF_YAML:
		rc = fyai_emit_stats_generic(stats, false);
		break;
	case FYAIOF_RAW:
	case FYAIOF_MARKDOWN:
	default:
		rc = fyai_emit_stats_markdown(ctx, stats);
		break;
	}
	fy_generic_builder_destroy(gb);
	return rc;
}

/* Attach a top comment to @v, returning the (transient) decorated value. */
static fy_generic fyai_decorate(struct fy_generic_builder *gb, fy_generic v,
				const char *text)
{
	return fy_generic_indirect_set_comment(gb, v, fycp_top, fy_value(text));
}

static void fyai_dump_window(const struct fyai_turn_selector_args *ts,
			     size_t count, size_t *lo, size_t *hi)
{
	size_t l, h;

	switch (ts->type) {
	case FYAITST_FIRST:
		*lo = 0;
		*hi = ts->first < count ? ts->first : count;
		break;
	case FYAITST_LAST:
		*lo = ts->last < count ? count - ts->last : 0;
		*hi = count;
		break;
	case FYAITST_RANGE:
		l = (ssize_t)ts->range_lo < 0 ? 0 : ts->range_lo;
		h = ts->range_hi;	/* inclusive */

		*lo = l < count ? l : count;
		*hi = (ssize_t)h < 0 ? *lo : (h + 1 < count ? h + 1 : count);
		if (*hi < *lo)
			*hi = *lo;
		break;
	default:
		*lo = 0;
		*hi = count;
		break;
	}
}

static void fyai_exchange_window(const struct fyai_turn_selector_args *ts,
				 struct fyai_turn_stack *stack,
				 size_t *lo, size_t *hi)
{
	size_t *starts;
	size_t exchanges;
	size_t count;
	size_t l;
	size_t h;
	size_t i;

	count = stack->count;
	if (!count) {
		*lo = 0;
		*hi = 0;
		return;
	}

	starts = calloc(count + 1, sizeof(*starts));
	if (!starts) {
		fyai_dump_window(ts, count, lo, hi);
		return;
	}

	exchanges = 0;
	for (i = 0; i < count; i++)
		if (fyai_turn_has_user_message(stack->items[i]))
			starts[exchanges++] = i;

	if (!exchanges) {
		starts[exchanges++] = 0;
	} else if (starts[0] > 0) {
		starts[0] = 0;
	}
	starts[exchanges] = count;

	switch (ts->type) {
	case FYAITST_FIRST:
		*lo = starts[0];
		*hi = starts[ts->first < exchanges ? ts->first : exchanges];
		break;
	case FYAITST_LAST:
		l = ts->last < exchanges ? exchanges - ts->last : 0;
		*lo = starts[l];
		*hi = count;
		break;
	case FYAITST_RANGE:
		l = ts->range_lo < exchanges ? ts->range_lo : exchanges;
		h = ts->range_hi + 1 < exchanges ? ts->range_hi + 1 : exchanges;
		if (h < l)
			h = l;
		*lo = starts[l];
		*hi = starts[h];
		break;
	default:
		*lo = 0;
		*hi = count;
		break;
	}
	free(starts);
}

/*
 * Emit the canonical conversation or the provider streams, honoring the
 * --first/--last/--range turn window and --decorate. All view generics are
 * built in a transient builder so the durable arena is never written.
 */
/*
 * Emit @text into @mf as a markdown blockquote (each line prefixed with
 * "> "), so markdown renders it with distinct quote styling - used to make
 * user turns stand out from assistant prose.
 */
static void fyai_emit_blockquote(FILE *mf, const char *text)
{
	const char *nl;
	const char *p;

	p = text;

	do {
		nl = strchr(p, '\n');
		if (nl) {
			fprintf(mf, "> %.*s\n", (int)(nl - p), p);
			p = nl + 1;
		} else {
			if (*p)
				fprintf(mf, "> %s\n", p);
		}
	} while (nl);
	fprintf(mf, "\n");
}

/*
 * Emit @text into @mf with each non-empty line italicized (`*line*`).
 * Emphasis cannot span blank lines, so it is applied per line - used to
 * give the system prompt a quiet, distinct look.
 */
static void fyai_emit_italic(FILE *mf, const char *text)
{
	const char *nl;
	const char *p;
	int len;

	p = text;

	do {
		nl = strchr(p, '\n');
		len = nl ? (int)(nl - p) : (int)strlen(p);

		if (len)
			fprintf(mf, "*%.*s*\n", len, p);
		else
			fprintf(mf, "\n");
		p = nl ? nl + 1 : NULL;
	} while (p && *p);
	fprintf(mf, "\n");
}

/*
 * Render a single canonical tool call into @mf for the display view. The
 * arguments are a JSON string; each known tool gets a renderer that surfaces
 * its salient argument (shell -> the command, read/write -> the path) instead
 * of dumping raw JSON. @tgb is the transient builder used to parse the args.
 */
static void fyai_emit_tool_result(FILE *mf, const char *text, int preview_lines,
				  const char *lang);

static bool fyai_tool_result_is_error(const char *text)
{
	while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
		text++;
	return !strncmp(text, "tool error:", 11);
}

/*
 * Render an apply_patch envelope - the V4A "*** Begin Patch / *** Update File:"
 * format the model emits, not a unified diff - as structured markdown. Each file
 * operation becomes a heading; an added file's body is fenced in the file's own
 * language (the leading '+' of each added line stripped), and an update's hunks
 * are fenced as `diff` so the +/-/context lines colour like a diff. A `*** `
 * marker ends the current section, mirroring how the applier itself scans.
 */
static void fyai_emit_patch(FILE *mf, const char *patch)
{
	const char *p;
	const char *nl;
	char *path;
	char *lang;
	bool in_add;
	bool in_upd;
	int len;

	in_add = false;
	in_upd = false;

	for (p = patch; p && *p; p = nl ? nl + 1 : NULL) {
		nl = strchr(p, '\n');
		len = nl ? (int)(nl - p) : (int)strlen(p);

		if (!strncmp(p, "*** ", 4)) {
			if (in_add || in_upd) {
				fprintf(mf, "```\n\n");
				in_add = false;
				in_upd = false;
			}
			if (!strncmp(p, "*** Add File: ", 14)) {
				path = NULL;
				if (asprintf(&path, "%.*s", len - 14, p + 14) < 0)
					path = NULL;
				fprintf(mf, "**add** `%s`\n\n", path ? path : "");
				lang = markdown_lang_for_path(path);
				fprintf(mf, "```%s\n", lang ? lang : "");
				free(lang);
				free(path);
				in_add = true;
			} else if (!strncmp(p, "*** Delete File: ", 17)) {
				fprintf(mf, "**delete** `%.*s`\n\n",
					len - 17, p + 17);
			} else if (!strncmp(p, "*** Update File: ", 17)) {
				fprintf(mf, "**update** `%.*s`\n\n```diff\n",
					len - 17, p + 17);
				in_upd = true;
			}
			/* *** Begin/End Patch and any other marker: skip. */
			continue;
		}

		if (in_add) {
			/* Added lines carry a leading '+'; strip it. */
			if (len > 0 && p[0] == '+')
				fprintf(mf, "%.*s\n", len - 1, p + 1);
			else
				fprintf(mf, "%.*s\n", len, p);
		} else if (in_upd) {
			fprintf(mf, "%.*s\n", len, p);
		}
	}

	if (in_add || in_upd)
		fprintf(mf, "```\n\n");
}

/*
 * Present a login URL. An authorization URL runs to several hundred characters -
 * wider than any terminal - and the interactive transcript commits one row per
 * line with auto-wrap off, so a raw URL is truncated at the last column and the
 * rest is unrecoverable. Render a Markdown link instead: libfymd4c turns it into
 * an OSC 8 hyperlink, so the URL travels in the escape (no cells) and the
 * visible row stays short and clickable. It needs colour to do that: libfymd4c
 * drops the escape under MD_ANSI_FLAG_NO_COLOR and prints the URL as text, which
 * is the wide row again. Under the UI that is settled already - fyai_ui_open()
 * resolves an "auto" colour setting to "on", since the spool that stdout points
 * at is not a terminal even though the UI owns one.
 *
 * The rendered bytes go to stdout like any other output: under the UI that is
 * the spool, drained and committed continuously - including while this caller
 * waits for the browser redirect. Without Markdown, or off a terminal (a pipe, a
 * redirect, batch use), print the plain URL, which wraps normally there.
 */
void fyai_print_login_url(struct fyai_ctx *ctx, const char *lead,
			  const char *label, const char *url)
{
	struct response_buffer out = { 0 };
	char *md;

	if (!url || !*url)
		return;
	if (!lead)
		lead = "Open this link:";
	if (!label || !*label)
		label = "open the sign-in page";
	md = fy_sprintfa("**%s** [%s](%s)\n", lead, label, url);
	if (md && ctx && ctx->cfg->markdown && markdown_available(ctx->cfg) &&
	    (fyai_ui_active(ctx) || ctx->stdout_tty) &&
	    !markdown_render(ctx->cfg, md, strlen(md), &out,
			     markdown_color_enabled(ctx->cfg->color),
			     ctx->cfg->theme_variant) &&
	    out.data && out.len) {
		fwrite(out.data, 1, out.len, stdout);
		fflush(stdout);
		free(out.data);
		return;
	}
	free(out.data);
	printf("%s\n%s\n", lead, url);
	fflush(stdout);
}

void fyai_emit_tool_call(FILE *mf, struct fy_generic_builder *gb,
				const char *name, fy_generic args,
				int preview_lines)
{
	const char *path;
	const char *cmd;
	const char *c;
	fy_generic gpath;
	fy_generic gc;
	char *lang;
	char *s;

	if (fy_equal(name, "shell")) {
		cmd = fy_get(args, "command", "");
		/*
		 * The model describes the purpose of the command. Show this
		 * description in brackets before the command. A delegation
		 * shows its sub-agent name in the same position.
		 */
		gc = fy_get(args, "description");
		c = fy_castp(&gc, "");
		fprintf(mf, "**shell**");
		if (*c)
			fprintf(mf, " [%s]", c);
		fprintf(mf, "\n\n```sh\n%s\n```\n\n", cmd);
		return;
	}
	if (fy_equal(name, "read_file")) {
		fprintf(mf, "**read** `%s`\n\n", fy_cast(
			fy_get(args, "path", ""), ""));
		return;
	}
	if (fy_equal(name, "write_file")) {
		/*
		 * Hold the path/content generics in locals and read them with
		 * fy_castp: a short (inline) string lives in the fy_generic word,
		 * so a pointer from fy_cast on the fy_get temporary would dangle
		 * once that temporary dies - and these are used past that point.
		 */
		gpath = fy_get(args, "path");
		gc = fy_get(args, "content");
		path = fy_castp(&gpath, "");
		c = fy_castp(&gc, "");
		fprintf(mf, "**write** `%s` (%zu byte%s)\n\n",
			path, strlen(c), strlen(c) == 1 ? "" : "s");
		if (*c) {
			lang = markdown_lang_for_path(path);
			fyai_emit_tool_result(mf, c, preview_lines, lang);
			free(lang);
		}
		return;
	}
	if (fy_equal(name, "apply_patch")) {
		gc = fy_get(args, "patch");
		c = fy_castp(&gc, "");
		if (*c && preview_lines < 0)
			fyai_emit_patch(mf, c);
		else if (*c && preview_lines > 0)
			fyai_emit_tool_result(mf, c, preview_lines, "diff");
		else
			fprintf(mf, "**patch**\n\n");
		return;
	}
	if (fy_equal(name, "agent")) {
		gpath = fy_get(args, "name");
		gc = fy_get(args, "description");
		path = fy_castp(&gpath, "");
		c = fy_castp(&gc, "");
		if (*path)
			fprintf(mf, "**agent** [%s] %s\n\n", path,
				*c ? c : "delegated task");
		else
			fprintf(mf, "**agent** %s\n\n",
				*c ? c : "delegated task");
		return;
	}
	if (fy_equal(name, "ask_user")) {
		fprintf(mf, "**❓ %s**\n\n",
			fy_cast(fy_get(args, "question", ""), ""));
		return;
	}
	/* Unknown tool: fall back to name + JSON arguments. */
	s = NULL;
	(void)fy_emit(gb, args,
		FYOPEF_OUTPUT_TYPE_STRING | FYOPEF_MODE_JSON |
		FYOPEF_WIDTH_INF, &s);
	fprintf(mf, "**%s** `%s`\n\n", name, s ? s : "");
	free(s);
}

/*
 * Render the tool call carried by a stored chat-shape assistant message
 * (function.name + JSON arguments). The args are parsed into @tgb.
 */
static void fyai_emit_tool_call_chat(FILE *mf, struct fy_generic_builder *tgb,
				     fy_generic call, int preview_lines)
{
	const char *name;
	const char *args_str;
	fy_generic args;
	fy_generic fn;

	fn = fy_get(call, "function");
	name = fy_get(fn, "name", "?");
	args_str = fy_get(fn, "arguments", "");

	args = parse_json_string(tgb, args_str);
	fyai_emit_tool_call(mf, tgb, name, args, preview_lines);
}

/*
 * Classification of a single canonical message, shared by the offline
 * `history` view and the live turn rendering so both agree on roles, tool
 * relatedness and inter-turn separation.
 */
struct fyai_msg_class {
	fy_generic msg;
	fy_generic role;
	fy_generic type;	/* Responses native item type, "" if chat-shaped */
	fy_generic content;
	fy_generic tc;
	bool is_str;
	bool has_text;
	bool is_tool_msg;
	bool has_tc;
	bool tool_related;
	bool is_assistant;
	bool is_native;
	bool skip;		/* nothing to render (e.g. reasoning items) */
};

/*
 * The message's string content as a C string. fy_castp takes the *address* of
 * the stored generic, so for short strings (kept inline in the fy_generic word)
 * it returns a pointer into that stable storage - unlike fy_cast, which would
 * return a pointer into a by-value temporary that dangles once the holding
 * struct goes out of scope. Callers must keep @c alive while using the result.
 */
static const char *fyai_msg_text(const struct fyai_msg_class *c)
{
	return c->is_str ? fy_castp(&c->content, "") : "";
}

static struct fyai_msg_class fyai_classify_message(fy_generic m)
{
	struct fyai_msg_class c;
	const char *type;
	bool is_call;
	bool is_msg;
	bool is_out;

	memset(&c, 0, sizeof(c));
	c.msg = m;
	c.type = fy_get(m, "type");
	type = fy_castp(&c.type, "");

	/*
	 * Responses native output items (function_call, function_call_output,
	 * reasoning, message, builtin shell_call/_output) carry a `type` rather
	 * than a chat `role`. Classify them so the display view renders a
	 * Responses-produced history the same as a Chat-produced one.
	 */
	if (*type) {
		is_call = fyai_item_type_is_call(c.type);
		is_out = fyai_item_type_is_call_output(c.type);
		is_msg = fy_equal(c.type, "message");

		c.is_native = true;
		c.has_tc = is_call;
		c.is_tool_msg = is_out;
		c.tool_related = is_call || is_out;
		c.is_assistant = is_msg;
		/*
		 * Reasoning items are skipped here: they are non-canonical and
		 * rendered uniformly from the turn's provider_stream instead
		 * (see fyai_emit_turn_reasoning), so a final-answer turn shows
		 * reasoning the same as a tool turn.
		 */
		c.skip = !(is_call || is_out || is_msg);
		return c;
	}

	c.role = fy_get(m, "role");
	c.content = fy_get(m, "content");
	c.tc = fy_get(m, "tool_calls");
	c.is_str = fy_is_string(c.content);
	c.has_text = c.is_str && *fyai_msg_text(&c);
	c.is_tool_msg = fy_equal(c.role, "tool");
	c.has_tc = fy_is_sequence(c.tc) && !fy_empty(c.tc);
	c.tool_related = c.is_tool_msg || (c.has_tc && !c.has_text);
	c.is_assistant = fy_equal(c.role, "assistant");
	return c;
}

/*
 * Render a single classified message into @mf as markdown. Tool results are
 * collapsed to a short preview (@preview_lines lines, <=0 means none) with a
 * "... +N more" trailer and a failure marker; user turns become blockquotes,
 * the system prompt italic, assistant prose verbatim, and tool calls the
 * invoked command. Shared by the `history` verb and the live loop so both
 * render identically. The inter-turn rule is the caller's concern.
 */
/*
 * Render command output as a short preview - up to @preview_lines lines, with a
 * "... +N lines" trailer when there is more - and flag failures (our tool
 * errors carry a "tool error:" marker). Shared by chat tool messages and the
 * Responses function_call_output item.
 */
static void fyai_emit_tool_result(FILE *mf, const char *text, int preview_lines,
				  const char *lang)
{
	const char *nl;
	const char *p;
	size_t limit;
	size_t shown;
	size_t total;
	int len;

	/*
	 * Zero suppresses the body, a negative value emits it in full, and a
	 * positive value limits the source preview. Rendered-row bounding is
	 * applied separately by the terminal renderer.
	 */
	if (!preview_lines)
		return;
	limit = preview_lines > 0 ? (size_t)preview_lines : SIZE_MAX;
	total = 0;
	shown = 0;

	for (p = text; *p; p++)
		if (*p == '\n')
			total++;
	if (*text && text[strlen(text) - 1] != '\n')
		total++;

	if (total) {
		if (fyai_tool_result_is_error(text))
			lang = NULL;
		fprintf(mf, "```%s\n", lang ? lang : "");
		p = text;
		while (*p && shown < limit) {
			nl = strchr(p, '\n');
			len = nl ? (int)(nl - p) : (int)strlen(p);
			fprintf(mf, "%.*s\n", len, p);
			shown++;
			if (!nl)
				break;
			p = nl + 1;
		}
		fprintf(mf, "```\n\n");
		if (total > shown)
			fprintf(mf, "_⋯ %zu more line%s_\n\n",
				total - shown,
				total - shown == 1 ? "" : "s");
	}
}

/*
 * Presentation policy belongs to the tool, not to the result's data shape.
 * The canonical messages and fragment source remain complete.
 */
int fyai_tool_preview_lines(const struct fyai_cfg *cfg, const char *name)
{
	if (!strcmp(cfg->tool_detail, "none"))
		return 0;
	if (!strcmp(cfg->tool_detail, "full"))
		return -1;
	if (!strcmp(cfg->tool_detail, "brief"))
		return cfg->tool_preview_lines;
	if (!name)
		return cfg->tool_preview_lines;
	if (!strcmp(name, "read_file") || !strcmp(name, "write_file"))
		return 0;
	if (!strcmp(name, "agent"))
		return 0;
	if (!strcmp(name, "apply_patch"))
		return -1;
	return cfg->tool_preview_lines;
}

/*
 * Render a structured shell result - the builtin shell_call output, which is a
 * sequence of { stdout, stderr, outcome } mappings (one per command) rather
 * than a flat string. Each entry gets a failure marker for a non-zero exit or
 * signal, a stdout preview, and a stderr preview when present, all through the
 * shared fyai_emit_tool_result() previewer. @out may be the sequence or a
 * single such mapping.
 */
static void fyai_emit_shell_output(FILE *mf, fy_generic out, int preview_lines)
{
	fy_generic outcome;
	fy_generic item;
	const char *se;
	const char *so;

	if (fy_is_sequence(out)) {
		fy_foreach(item, out)
			fyai_emit_shell_output(mf, item, preview_lines);
		return;
	}

	outcome = fy_get(out, "outcome");
	if (fy_equal(fy_get(outcome, "type", "exit"), "signal"))
		fprintf(mf, "**✗ signal %lld**\n\n",
			fy_get(outcome, "signal", 0LL));
	else if (fy_get(outcome, "exit_code", 0LL) != 0)
		fprintf(mf, "**✗ exit %lld**\n\n",
			fy_get(outcome, "exit_code", 0LL));

	so = fy_get(out, "stdout", "");
	se = fy_get(out, "stderr", "");
	if (*so)
		fyai_emit_tool_result(mf, so, preview_lines, NULL);
	if (*se) {
		fprintf(mf, "_stderr_\n\n");
		fyai_emit_tool_result(mf, se, preview_lines, NULL);
	}
}

/*
 * Live counterpart to fyai_emit_shell_output(): render a structured builtin
 * shell result straight to stdout, each stdout/stderr stream as its own
 * decoration-free fenced block bounded to @preview_lines rendered rows. Failure
 * markers stay Markdown (rendered small and unbounded). @out is the sequence or
 * a single { stdout, stderr, outcome } mapping.
 */
static void fyai_print_shell_output(struct fyai_cfg *cfg, fy_generic out,
				    int preview_lines)
{
	char marker[64];
	fy_generic outcome;
	fy_generic item;
	const char *se;
	const char *so;

	if (fy_is_sequence(out)) {
		fy_foreach(item, out)
			fyai_print_shell_output(cfg, item, preview_lines);
		return;
	}

	outcome = fy_get(out, "outcome");
	if (fy_equal(fy_get(outcome, "type", "exit"), "signal")) {
		snprintf(marker, sizeof(marker), "**✗ signal %lld**",
			 fy_get(outcome, "signal", 0LL));
		fyai_print_markdown(marker, cfg);
	} else if (fy_get(outcome, "exit_code", 0LL) != 0) {
		snprintf(marker, sizeof(marker), "**✗ exit %lld**",
			 fy_get(outcome, "exit_code", 0LL));
		fyai_print_markdown(marker, cfg);
	}

	so = fy_get(out, "stdout", "");
	se = fy_get(out, "stderr", "");
	if (*so)
		fyai_print_fenced(cfg, so, strlen(so), NULL, fy_invalid,
				  preview_lines);
	if (*se) {
		fyai_print_markdown("_stderr_", cfg);
		fyai_print_fenced(cfg, se, strlen(se), NULL, fy_invalid,
				  preview_lines);
	}
}

/*
 * Join the human-readable text of a Responses `reasoning` item: the
 * summary[].text parts (type summary_text), with content[].text (reasoning_text)
 * as a fallback. The item's `encrypted_content` is an opaque provider blob, not
 * text, and is deliberately never read here. Returns "" when only an encrypted
 * blob is present.
 */
static fy_generic fyai_reasoning_text(struct fy_generic_builder *tgb,
				      fy_generic m)
{
	const char *key;
	fy_generic chunks;
	fy_generic parts;
	fy_generic part;
	fy_generic t;

	chunks = fy_seq_empty;

	parts = fy_get(m, "summary");
	key = "summary_text";
	if (!fy_is_sequence(parts) || !fy_len(parts)) {
		parts = fy_get(m, "content");
		key = "reasoning_text";
	}

	fy_foreach(part, parts) {
		t = fy_get(part, "type");
		if (fy_not_equal(t, key) && fy_not_equal(t, "text"))
			continue;
		chunks = fy_append(tgb, chunks, fy_get(part, "text", ""));
	}

	return fyai_join_strings(tgb, chunks);
}

/*
 * Render a Responses native output item (function_call, shell_call,
 * function_call_output, shell_call_output, message, reasoning). Counterpart to
 * the chat-shape branches in fyai_emit_message_md().
 */
static void fyai_emit_native_item(FILE *mf, struct fy_generic_builder *tgb,
				  const struct fyai_msg_class *c,
				  int preview_lines, const char *result_lang,
				  bool thinking)
{
	const char *cmd;
	const char *r;
	const char *args_str;
	fy_generic content;
	fy_generic args;
	fy_generic part;
	fy_generic out;
	fy_generic m;
	fy_generic t;

	m = c->msg;

	if (fy_equal(c->type, "reasoning")) {
		if (!thinking)
			return;
		r = fy_cast(fyai_reasoning_text(tgb, m), "");
		if (*r) {
			fprintf(mf, "**💭 reasoning**\n\n");
			fyai_emit_italic(mf, r);
		} else if (fy_is_valid(fy_get(m, "encrypted_content"))) {
			fprintf(mf, "_💭 reasoning (encrypted)_\n\n");
		}
		return;
	}

	if (fy_equal(c->type, "function_call")) {
		args_str = fy_get(m, "arguments", "");
		args = parse_json_string(tgb, args_str);
		fyai_emit_tool_call(mf, tgb, fy_get(m, "name", "?"), args,
				    preview_lines);
		return;
	}
	if (fy_equal(c->type, "shell_call")) {
		cmd = fy_cast(fy_get_at_path(tgb, m, "action", "commands", 0), "");
		fyai_emit_tool_call(mf, tgb, "shell", fy_mapping("command", cmd),
				    preview_lines);
		return;
	}
	if (fyai_item_type_is_call_output(c->type)) {
		out = fy_get(m, "output");
		if (fy_is_string(out))
			fyai_emit_tool_result(mf, fy_castp(&out, ""),
					      preview_lines, result_lang);
		else if (fy_is_valid(out) &&
			 !fy_is_null(out))
			fyai_emit_shell_output(mf, out, preview_lines);
		return;
	}
	if (fy_equal(c->type, "message")) {
		content = fy_get(m, "content");
		if (fy_is_string(content)) {
			fprintf(mf, "%s\n\n", fy_cast(content, ""));
			return;
		}
		fy_foreach(part, content) {
			t = fy_get(part, "type");
			if (fy_not_equal(t, "output_text") && fy_not_equal(t, "text"))
				continue;
			fprintf(mf, "%s", fy_get(part, "text", ""));
		}
		fprintf(mf, "\n\n");
		return;
	}
}

static void fyai_emit_message_md(FILE *mf, struct fy_generic_builder *tgb,
				 const struct fyai_msg_class *c,
				 int preview_lines, const char *result_lang,
				 bool thinking)
{
	fy_generic part;
	const char *text;
	char *s;

	text = fyai_msg_text(c);

	if (c->is_native) {
		fyai_emit_native_item(mf, tgb, c, preview_lines, result_lang,
				      thinking);
		return;
	}

	if (c->is_tool_msg && c->is_str) {
		fyai_emit_tool_result(mf, text, preview_lines, result_lang);
	} else if (c->is_tool_msg && fy_is_valid(c->content) &&
		   !fy_is_null(c->content)) {
		/* Structured builtin shell result (stdout/stderr/outcome). */
		fyai_emit_shell_output(mf, c->content, preview_lines);
	} else if (c->is_str && fy_equal(c->role, "system")) {
		fprintf(mf, "**System**\n\n");
		fyai_emit_italic(mf, text);
	} else if (c->is_str && fy_equal(c->role, "user")) {
		fyai_emit_blockquote(mf, text);
	} else if (c->has_text) {
		fprintf(mf, "%s\n\n", text);
	} else if (!c->has_tc && fy_is_valid(c->content) &&
		   !fy_is_null(c->content)) {
		s = NULL;
		(void)fy_emit(tgb, c->content,
			FYOPEF_OUTPUT_TYPE_STRING |
			FYOPEF_MODE_YAML_1_2 |
			FYOPEF_STYLE_PRETTY |
			FYOPEF_WIDTH_INF, &s);
		fprintf(mf, "```yaml\n%s\n```\n\n", s ? s : "");
		free(s);
	}

	if (c->has_tc) {
		fy_foreach(part, c->tc)
			fyai_emit_tool_call_chat(mf, tgb, part, preview_lines);
	}
}

static fy_generic fyai_turn_provider(fy_generic turn);

/*
 * Render a turn's reasoning trace for the offline `history` view from its
 * provider_stream (reasoning is non-canonical, so it is not in `messages`).
 * Handles Responses `reasoning` items (summary text; the encrypted_content blob
 * is never shown) and the Chat `reasoning_content` field. Returns true if it
 * emitted anything.
 */
static bool fyai_emit_turn_reasoning(FILE *mf, struct fy_generic_builder *tgb,
				     fy_generic turn, bool thinking)
{
	const char *prov;
	const char *rc;
	const char *r;
	fy_generic items;
	fy_generic it;
	fy_generic ps;
	fy_generic tp;
	bool emitted;

	ps = fy_get(turn, "provider_stream");
	tp = fyai_turn_provider(turn);
	prov = fy_castp(&tp, "");
	emitted = false;

	if (!prov || !thinking)
		return false;

	items = fy_get(ps, prov);
	fy_foreach(it, items) {
		if (fy_equal(fy_get(it, "type"), "reasoning")) {
			r = fy_cast(fyai_reasoning_text(tgb, it), "");
			if (*r) {
				fprintf(mf, "**💭 reasoning**\n\n");
				fyai_emit_italic(mf, r);
				emitted = true;
			} else if (fy_is_valid(fy_get(it, "encrypted_content"))) {
				fprintf(mf, "_💭 reasoning (encrypted)_\n\n");
				emitted = true;
			}
			continue;
		}

		/* Chat Completions: reasoning_content on the assistant msg. */
		rc = fy_get(it, "reasoning_content", "");
		if (*rc) {
			fprintf(mf, "**💭 reasoning**\n\n");
			fyai_emit_italic(mf, rc);
			emitted = true;
		}
	}

	return emitted;
}

/*
 * The single tool-result rendering path shared by the live loop and the
 * `history` view: a string result is a decoration-free fenced block (highlighted
 * with @lang unless it is a tool error, which stays plain), a structured
 * builtin-shell result is rendered per stream, both bounded to @preview_lines
 * rendered rows. @content is the result generic (string or shell mapping/seq).
 */
/* Render the configured tool-output separator (display/tool_separator) before a
 * tool result, as themed markdown. Empty (the default) emits nothing. */
static void fyai_print_tool_separator(struct fyai_cfg *cfg)
{
	if (cfg->tool_separator && *cfg->tool_separator)
		fyai_print_markdown(cfg->tool_separator, cfg);
}

void fyai_render_tool_result(struct fyai_cfg *cfg, fy_generic content,
			     const char *lang, int preview_lines)
{
	const char *s = fy_castp(&content, "");
	size_t max_lines;

	if (!preview_lines)
		return;
	max_lines = preview_lines < 0 ? 0 : (size_t)preview_lines;
	fyai_print_tool_separator(cfg);
	if (fy_is_string(content)) {
		if (fyai_tool_result_is_error(s))
			lang = NULL;
		fyai_print_fenced(cfg, s, strlen(s), lang, fy_invalid,
				  max_lines);
	} else if (fy_is_valid(content) &&
		   !fy_is_null(content)) {
		fyai_print_shell_output(cfg, content,
					preview_lines < 0 ? 0 : preview_lines);
	}
}

/*
 * Render one live tool exchange (the call header plus the result preview)
 * during execution. The call header uses the same fyai_emit_tool_call() emitter
 * as the `history` verb; the result goes through the shared
 * fyai_render_tool_result() path. The tool call is normalized to (name, args)
 * here so both API modes (Responses items and Chat function calls, including the
 * built-in shell_call) share one path.
 */
static void fyai_render_tool_exchange_common(struct fyai_ctx *ctx,
					     fy_generic tool_call,
					     fy_generic tool_result,
					     bool render_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder_cfg gcfg;
	struct fy_generic_builder *tgb;
	const char *args_text;
	const char *cmd;
	const char *name;
	const char *res_str;
	int preview_lines;
	fy_generic args;
	char *lang;
	char *md;
	size_t mdlen;
	FILE *mf;

	md = NULL;
	mdlen = 0;
	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	tgb = fy_generic_builder_create(&gcfg);
	if (!tgb)
		return;

	if (fy_equal(fy_get(tool_call, "type"), "shell_call")) {
		name = "shell";
		cmd = fy_cast(fy_get_at_path(tool_call, "action", "commands", 0), "");
		args = fy_mapping("command", cmd);
	} else {
		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			name = fy_get(tool_call, "name", "");
			args_text = fy_get(tool_call, "arguments", "");
			break;
		case FYAI_API_CHAT_COMPLETIONS:
			name = fy_get(fy_get(tool_call, "function"), "name", "");
			args_text = fy_get(fy_get(tool_call, "function"), "arguments", "");
			break;
		case FYAI_API_MESSAGES:
			/* normalized to Responses-style function_call items */
			name = fy_get(tool_call, "name", "");
			args_text = fy_get(tool_call, "arguments", "");
			break;
		default:
			assert(0);
			__builtin_unreachable();
			break;
		}

		args = parse_json_string(tgb, args_text);
	}
	if (!*name)
		name = "tool";
	preview_lines = fyai_tool_preview_lines(cfg, name);

	mf = open_memstream(&md, &mdlen);
	if (!mf) {
		fy_generic_builder_destroy(tgb);
		return;
	}

	/* The tool-call header renders in full (never row-bounded). */
	if (render_call)
		fyai_emit_tool_call(mf, tgb, name, args, preview_lines);
	fclose(mf);
	/* Mark only shell command blocks. */
	if (!fy_str_empty(md) && fyai_print_markdown_flags(md, ctx->cfg,
			fy_equal(name, "shell") ? FYMD_RF_CODE_MARKER : 0))
		fputs(md, stdout);
	free(md);

	/*
	 * A read_file result is the file's contents, so highlight it with the
	 * language inferred from the requested path; the shared renderer draws it
	 * frameless and row-bounded (see fyai_render_tool_result).
	 */
	res_str = fy_castp(&tool_result, "");
	lang = NULL;
	if (fy_equal(name, "read_file") && *res_str &&
	    !fyai_tool_result_is_error(res_str))
		lang = markdown_lang_for_path(fy_cast(fy_get(args, "path", ""), ""));
	fyai_render_tool_result(cfg, tool_result, lang, preview_lines);
	free(lang);

	fy_generic_builder_destroy(tgb);
}

void fyai_render_tool_exchange(struct fyai_ctx *ctx, fy_generic tool_call,
			       fy_generic tool_result)
{
	fyai_render_tool_exchange_common(ctx, tool_call, tool_result, true);
}

void fyai_render_tool_result_exchange(struct fyai_ctx *ctx,
				      fy_generic tool_call,
				      fy_generic tool_result)
{
	fyai_render_tool_exchange_common(ctx, tool_call, tool_result, false);
}

/*
 * Append the canonical Markdown for a tool exchange to the one active
 * assistant output. Execution and terminal rendering are intentionally not
 * involved: this exact source is what is persisted and replayed by history.
 */
int fyai_record_tool_exchange(struct fyai_ctx *ctx, fy_generic tool_call,
			      fy_generic tool_result)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder_cfg gcfg = {0};
	struct fy_generic_builder *tgb;
	const char *args_text, *cmd, *name, *res_str;
	int preview_lines;
	fy_generic args;
	char *lang = NULL;
	char *md = NULL;
	size_t mdlen = 0;
	size_t start, end;
	FILE *mf;

	gcfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	tgb = fy_generic_builder_create(&gcfg);
	if (!tgb)
		return -1;
	if (fy_equal(fy_get(tool_call, "type"), "shell_call")) {
		name = "shell";
		cmd = fy_cast(fy_get_at_path(tool_call, "action", "commands", 0), "");
		args = fy_mapping("command", cmd);
	} else {
		if (cfg->api_mode == FYAI_API_CHAT_COMPLETIONS) {
			name = fy_get(fy_get(tool_call, "function"), "name", "");
			args_text = fy_get(fy_get(tool_call, "function"),
					   "arguments", "");
		} else {
			name = fy_get(tool_call, "name", "");
			args_text = fy_get(tool_call, "arguments", "");
		}
		args = parse_json_string(tgb, args_text);
	}
	if (!*name)
		name = "tool";
	preview_lines = fyai_tool_preview_lines(cfg, name);
	res_str = fy_castp(&tool_result, "");
	if (fy_equal(name, "read_file") && *res_str &&
	    !fyai_tool_result_is_error(res_str))
		lang = markdown_lang_for_path(fy_cast(fy_get(args, "path", ""), ""));

	mf = open_memstream(&md, &mdlen);
	fyai_error_check(ctx, mf, err, "could not format tool display output");
	fyai_emit_tool_call(mf, tgb, name, args, preview_lines);
	if (cfg->tool_separator && *cfg->tool_separator)
		fprintf(mf, "%s\n\n", cfg->tool_separator);
	fyai_error_check(ctx, !fclose(mf), err_closed,
			 "could not finish tool call display output");
	mf = NULL;
	fyai_error_check(ctx, !fyai_output_append(ctx, md, mdlen), err,
			 "could not append tool call display output");
	free(md);
	md = NULL;
	mdlen = 0;

	start = strlen(fyai_output_markdown(ctx, NULL));
	mf = open_memstream(&md, &mdlen);
	fyai_error_check(ctx, mf, err, "could not format tool result output");
	if (fy_is_string(tool_result))
		fyai_emit_tool_result(mf, res_str, -1, lang);
	else
		fyai_emit_shell_output(mf, tool_result, -1);
	fyai_error_check(ctx, !fclose(mf), err_closed,
			 "could not finish tool display output");
	mf = NULL;
	fyai_error_check(ctx, !fyai_output_append(ctx, md, mdlen), err,
			 "could not append tool display output");
	end = start + mdlen;
	fyai_error_check(ctx,
		!fyai_output_add_fragment(ctx, "tool_result", start, end,
					  lang, name), err,
		"could not record tool result fragment");
	free(md);
	free(lang);
	fy_generic_builder_destroy(tgb);
	return 0;
err:
	if (mf)
		fclose(mf);
err_closed:
	free(md);
	free(lang);
	fy_generic_builder_destroy(tgb);
	return -1;
}

/*
 * A read_file call and its result are separate canonical messages linked by a
 * call id (Responses `call_id`, Chat `tool_call_id`). Unlike the live loop,
 * the offline `history` walk sees them apart, so to fence a read result in the
 * file's language it first records id -> requested path for every read_file
 * call, then looks the path up when the matching output is rendered.
 */
struct fyai_read_map {
	struct fyai_read_ent {
		char *id;
		char *path;
	} *ents;
	size_t n;
	size_t cap;
};

static void fyai_read_map_add(struct fyai_read_map *rm, const char *id,
			      const char *path)
{
	struct fyai_read_ent *e;
	size_t nc;

	if (!id || !*id || !path)
		return;
	if (rm->n == rm->cap) {
		nc = rm->cap ? rm->cap * 2 : 8;
		e = realloc(rm->ents, nc * sizeof(*e));
		if (!e)
			return;
		rm->ents = e;
		rm->cap = nc;
	}
	e = &rm->ents[rm->n];
	e->id = strdup(id);
	e->path = strdup(path);
	if (e->id && e->path)
		rm->n++;
	else {
		free(e->id);
		free(e->path);
	}
}

static const char *fyai_read_map_path(const struct fyai_read_map *rm,
				      const char *id)
{
	size_t i;

	if (!id || !*id)
		return NULL;
	for (i = 0; i < rm->n; i++)
		if (!strcmp(rm->ents[i].id, id))
			return rm->ents[i].path;
	return NULL;
}

static void fyai_read_map_free(struct fyai_read_map *rm)
{
	size_t i;

	for (i = 0; i < rm->n; i++) {
		free(rm->ents[i].id);
		free(rm->ents[i].path);
	}
	free(rm->ents);
	rm->ents = NULL;
	rm->n = 0;
	rm->cap = 0;
}

/*
 * Record any read_file tool call carried by message @m (Responses native
 * function_call, or a Chat assistant tool_calls[] entry) as id -> path. The id
 * and path generics are held in locals so fy_castp reads stable storage - a
 * short (inline) string would dangle if read from the fy_get temporary before
 * fyai_read_map_add() copies it.
 */
static void fyai_read_map_scan(struct fyai_read_map *rm,
			       struct fy_generic_builder *tgb, fy_generic m)
{
	fy_generic tc;
	fy_generic call;
	fy_generic fn;
	fy_generic args;
	fy_generic gid;
	fy_generic gpath;

	if (fy_equal(fy_get(m, "type"), "function_call")) {
		if (fy_not_equal(fy_get(m, "name"), "read_file"))
			return;
		args = parse_json_string(tgb, fy_get(m, "arguments", ""));
		gid = fy_get(m, "call_id");
		gpath = fy_get(args, "path");
		fyai_read_map_add(rm, fy_castp(&gid, ""), fy_castp(&gpath, ""));
		return;
	}

	tc = fy_get(m, "tool_calls");
	if (!fy_is_sequence(tc))
		return;
	fy_foreach(call, tc) {
		fn = fy_get(call, "function");
		if (fy_not_equal(fy_get(fn, "name"), "read_file"))
			continue;
		args = parse_json_string(tgb, fy_get(fn, "arguments", ""));
		gid = fy_get(call, "id");
		gpath = fy_get(args, "path");
		fyai_read_map_add(rm, fy_castp(&gid, ""), fy_castp(&gpath, ""));
	}
}

/*
 * Fence language for a tool-output message @c that answers a read_file call:
 * the file's contents highlighted by the recorded path's extension. Returns a
 * newly allocated language name (caller frees) or NULL when @c is not a
 * read_file output, the result is a tool error, or the extension is unknown.
 */
static char *fyai_read_result_lang(const struct fyai_read_map *rm,
				   const struct fyai_msg_class *c)
{
	fy_generic gid;
	fy_generic gout;
	const char *id;
	const char *out;
	const char *path;

	if (c->is_native) {
		if (fy_not_equal(c->type, "function_call_output"))
			return NULL;
		gid = fy_get(c->msg, "call_id");
		gout = fy_get(c->msg, "output");
	} else if (c->is_tool_msg) {
		gid = fy_get(c->msg, "tool_call_id");
		gout = c->content;
	} else {
		return NULL;
	}

	if (!fy_is_string(gout))
		return NULL;
	out = fy_castp(&gout, "");
	if (!*out || fyai_tool_result_is_error(out))
		return NULL;

	id = fy_castp(&gid, "");
	path = fyai_read_map_path(rm, id);
	if (!path)
		return NULL;
	return markdown_lang_for_path(path);
}

/*
 * True when @c is a tool result (a chat tool message or a Responses
 * function_call_output/shell_call_output), returning its result content generic
 * (string or shell mapping/sequence) in *out. Lets the history view route the
 * result through the shared frameless fyai_render_tool_result() path, exactly as
 * the live loop does, so tool output renders identically in both.
 */
static bool fyai_msg_is_tool_result(const struct fyai_msg_class *c,
				    fy_generic *out)
{
	if (c->is_native) {
		if (fy_not_equal(c->type, "function_call_output") &&
		    fy_not_equal(c->type, "shell_call_output"))
			return false;
		*out = fy_get(c->msg, "output");
		return true;
	}
	if (c->is_tool_msg) {
		*out = c->content;
		return true;
	}
	return false;
}

static bool fyai_display_outputs_complete(const struct fyai_turn_stack *stack,
					   size_t lo, size_t hi)
{
	struct fyai_msg_class c;
	fy_generic outputs, output, fragments, fragment, msgs, msg, result;
	size_t users = 0, assistants = 0;
	size_t user_outputs = 0, assistant_outputs = 0;
	size_t tool_results = 0, tool_fragments = 0;
	size_t i;

	for (i = lo; i < hi; i++) {
		msgs = fy_get(stack->items[i], "messages", fy_seq_empty);
		fy_foreach(msg, msgs) {
			if (fy_equal(fy_get(msg, "role"), "user"))
				users++;
			c = fyai_classify_message(msg);
			if (fyai_msg_is_tool_result(&c, &result))
				tool_results++;
		}
		outputs = fy_get(stack->items[i], "display_outputs", fy_seq_empty);
		fy_foreach(output, outputs) {
			if (fy_equal(fy_get(output, "tag"), "user"))
				user_outputs++;
			else if (fy_equal(fy_get(output, "tag"), "assistant"))
				assistant_outputs++;
			fragments = fy_get(output, "fragments", fy_seq_empty);
			fy_foreach(fragment, fragments)
				if (fy_equal(fy_get(fragment, "kind"),
					     "tool_result"))
					tool_fragments++;
		}
	}
	/* Each selected user exchange must have both its user card and the
	 * consolidated assistant document. A system-only window is replayable
	 * whenever it has any stored output. */
	assistants = users;
	if (users)
		return user_outputs == users &&
		       assistant_outputs == assistants &&
		       tool_fragments == tool_results;
	for (i = lo; i < hi; i++)
		if (fy_len(fy_get(stack->items[i], "display_outputs",
				  fy_seq_empty)))
			return true;
	return false;
}

static int fyai_display_markdown_range(struct fyai_cfg *cfg, const char *md,
				       size_t start, size_t end)
{
	char *slice;
	int rc;

	if (end <= start)
		return 0;
	slice = strndup(md + start, end - start);
	if (!slice)
		return -1;
	rc = fyai_print_markdown(slice, cfg);
	if (rc)
		fputs(slice, stdout);
	free(slice);
	return 0;
}

static int fyai_display_assistant_output(struct fyai_ctx *ctx,
					 fy_generic output,
					 const char *md,
					 const fy_generic *tool_results,
					 size_t tool_result_count,
					 size_t *tool_result_pos)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic fragments, fragment, content, glang, gtool;
	const char *lang;
	const char *tool;
	int preview_lines;
	long long llstart, llend;
	size_t start, end, pos, len;

	fragments = fy_get(output, "fragments", fy_seq_empty);
	if (!fy_len(fragments))
		return fyai_render_display_output(ctx, "assistant", md);

	len = strlen(md);
	pos = 0;
	fy_foreach(fragment, fragments) {
		llstart = fy_get(fragment, "start", -1LL);
		llend = fy_get(fragment, "end", -1LL);
		if (llstart < 0 || llend < llstart ||
		    (unsigned long long)llend > len ||
		    (size_t)llstart < pos)
			return -1;
		start = (size_t)llstart;
		end = (size_t)llend;
		if (fyai_display_markdown_range(cfg, md, pos, start))
			return -1;
		if (fy_equal(fy_get(fragment, "kind"), "tool_result")) {
			if (*tool_result_pos >= tool_result_count)
				return -1;
			content = tool_results[(*tool_result_pos)++];
			glang = fy_get(fragment, "lang");
			lang = fy_is_string(glang) ?
				fy_castp(&glang, "") : NULL;
			gtool = fy_get(fragment, "tool");
			tool = fy_is_string(gtool) ?
				fy_castp(&gtool, "") : NULL;
			/*
			 * Pre-tool-metadata fragments can still identify the
			 * two important shapes without parsing rendered Markdown.
			 */
			if (!tool && lang && *lang)
				tool = "read_file";
			else if (!tool && !fy_is_string(content))
				tool = "shell";
			preview_lines = fyai_tool_preview_lines(cfg, tool);
			fyai_render_tool_result(cfg, content, lang,
						preview_lines);
		} else if (fyai_display_markdown_range(cfg, md, start, end)) {
			return -1;
		}
		pos = end;
	}
	return fyai_display_markdown_range(cfg, md, pos, len);
}

static int fyai_display_stored_outputs(struct fyai_ctx *ctx,
				       const struct fyai_turn_stack *stack,
				       size_t lo, size_t hi)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_display_args *args = &cfg->cmd.args.display;
	fy_generic outputs, output;
	struct fyai_msg_class c;
	fy_generic msgs, msg, result;
	fy_generic *tool_results = NULL;
	const char *tag, *md;
	size_t i;
	size_t tool_result_count = 0, tool_result_pos = 0;
	bool emitted = false;

	for (i = lo; i < hi; i++) {
		msgs = fy_get(stack->items[i], "messages", fy_seq_empty);
		fy_foreach(msg, msgs) {
			c = fyai_classify_message(msg);
			if (fyai_msg_is_tool_result(&c, &result))
				tool_result_count++;
		}
	}
	if (tool_result_count) {
		size_t n = 0;

		tool_results = calloc(tool_result_count, sizeof(*tool_results));
		if (!tool_results)
			return -1;
		for (i = lo; i < hi; i++) {
			msgs = fy_get(stack->items[i], "messages", fy_seq_empty);
			fy_foreach(msg, msgs) {
				c = fyai_classify_message(msg);
				if (fyai_msg_is_tool_result(&c, &result))
					tool_results[n++] = result;
			}
		}
	}

	for (i = lo; i < hi; i++) {
		outputs = fy_get(stack->items[i], "display_outputs", fy_seq_empty);
		fy_foreach(output, outputs) {
			tag = fy_get(output, "tag", "assistant");
			md = fy_get(output, "markdown", "");
			if (!cfg->transcript_system && fy_equal(tag, "system"))
				continue;
			if (emitted &&
			    (fy_equal(tag, "user") || fy_equal(tag, "system")) &&
			    cfg->turn_separator &&
			    *cfg->turn_separator) {
				if (args->raw)
					printf("%s\n\n", cfg->turn_separator);
				else
					fyai_print_markdown(cfg->turn_separator,
							   cfg);
			}
			if (args->raw)
				fputs(md, stdout);
			else if (fy_equal(tag, "assistant")) {
				if (fyai_display_assistant_output(ctx, output, md,
						tool_results, tool_result_count,
						&tool_result_pos)) {
					free(tool_results);
					return -1;
				}
			}
			else
				(void)fyai_render_display_output(ctx, tag, md);
			emitted = true;
		}
	}
	if (ctx->stdout_tty && !args->raw)
		putchar('\n');
	free(tool_results);
	return 0;
}

struct fyai_display_render {
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	struct fyai_display_args *args;
	struct fy_generic_builder *tgb;
	const struct fyai_turn_stack *stack;
	struct fyai_read_map *reads;
	FILE *mf;
	char *md;
	size_t mdlen;
	bool prev_tool;
	bool emitted;
};

static void fyai_display_scan_reads(struct fyai_display_render *view,
				    size_t lo, size_t hi)
{
	fy_generic msgs;
	fy_generic m;
	size_t i;

	for (i = lo; i < hi; i++) {
		msgs = fy_get(view->stack->items[i], "messages", fy_seq_empty);
		fy_foreach(m, msgs)
			fyai_read_map_scan(view->reads, view->tgb, m);
	}
}

/* Flush pending markdown and start a fresh buffer for an inline result. */
static int fyai_display_flush_reopen(struct fyai_display_render *view)
{
	int rc;

	rc = fclose(view->mf);
	view->mf = NULL;
	fyai_error_check(view->ctx, !rc, out,
			 "could not finish the display buffer");
	if (view->md && fyai_print_markdown(view->md, view->cfg))
		fputs(view->md, stdout);
	free(view->md);
	view->md = NULL;
	view->mdlen = 0;
	view->mf = open_memstream(&view->md, &view->mdlen);
	fyai_error_check(view->ctx, view->mf, out,
			 "could not create the display buffer");
	return 0;
out:
	return -1;
}

static int fyai_display_reasoning(struct fyai_display_render *view,
				  fy_generic turn, size_t message_count)
{
	char *rmd;
	size_t rlen;
	FILE *rf;
	bool any;
	int rc;

	if (!message_count)
		return 0;
	rmd = NULL;
	rlen = 0;
	rf = open_memstream(&rmd, &rlen);
	fyai_error_check(view->ctx, rf, out,
			 "could not create the reasoning display buffer");
	any = fyai_emit_turn_reasoning(rf, view->tgb, turn,
				       view->cfg->thinking);
	rc = fclose(rf);
	rf = NULL;
	fyai_error_check(view->ctx, !rc, out,
			 "could not finish the reasoning display buffer");
	if (any) {
		if (view->emitted)
			fprintf(view->mf, "%s\n\n", view->cfg->turn_separator);
		fputs(rmd, view->mf);
		view->prev_tool = true;
		view->emitted = true;
	}
	free(rmd);
	return 0;
out:
	if (rf)
		fclose(rf);
	free(rmd);
	return -1;
}

static int fyai_display_message(struct fyai_display_render *view,
				fy_generic message)
{
	struct fyai_msg_class c;
	fy_generic result;
	char *rlang;
	int rc;

	c = fyai_classify_message(message);
	if (c.skip || (!view->cfg->transcript_system &&
		       fy_equal(c.role, "system")))
		return 0;

	if (c.is_str && fy_equal(c.role, "user") && !view->args->raw &&
	    view->cfg->markdown && view->ctx->stdout_tty) {
		rc = fyai_display_flush_reopen(view);
		fyai_error_check(view->ctx, !rc, out,
				 "could not flush the display buffer");
		if (view->emitted)
			putchar('\n');
		fyai_print_user_turn(view->ctx, fyai_msg_text(&c), false);
		view->prev_tool = true;
		view->emitted = true;
		return 0;
	}

	if (!view->args->raw && view->cfg->markdown &&
	    fyai_msg_is_tool_result(&c, &result)) {
		rc = fyai_display_flush_reopen(view);
		fyai_error_check(view->ctx, !rc, out,
				 "could not flush the display buffer");
		rlang = fyai_read_result_lang(view->reads, &c);
		fyai_render_tool_result(view->cfg, result, rlang,
					view->cfg->tool_preview_lines);
		free(rlang);
		view->prev_tool = c.tool_related;
		view->emitted = true;
		return 0;
	}

	if (view->emitted &&
	    !(view->prev_tool && (c.tool_related || c.is_assistant)))
		fprintf(view->mf, "%s\n\n", view->cfg->turn_separator);
	rlang = fyai_read_result_lang(view->reads, &c);
	fyai_emit_message_md(view->mf, view->tgb, &c,
				     view->cfg->tool_preview_lines, rlang,
				     view->cfg->thinking);
	free(rlang);
	view->prev_tool = c.tool_related;
	view->emitted = true;
	return 0;
out:
	return -1;
}

static int fyai_display_message_list(struct fyai_display_render *view,
				     fy_generic messages)
{
	fy_generic message;
	int rc;

	fy_foreach(message, messages) {
		rc = fyai_display_message(view, message);
		fyai_error_check(view->ctx, !rc, out,
				 "could not render a display message");
	}
	return 0;
out:
	return -1;
}

static int fyai_display_turns(struct fyai_display_render *view,
				      size_t lo, size_t hi)
{
	fy_generic msgs;
	size_t i;
	int rc;

	for (i = lo; i < hi; i++) {
		msgs = fy_get(view->stack->items[i], "messages", fy_seq_empty);
		rc = fyai_display_reasoning(view, view->stack->items[i],
					    fy_len(msgs));
		fyai_error_check(view->ctx, !rc, out,
				 "could not render turn reasoning");
		rc = fyai_display_message_list(view, msgs);
		fyai_error_check(view->ctx, !rc, out,
				 "could not render turn messages");
	}
	return 0;
out:
	return -1;
}

static int fyai_display_finish(struct fyai_display_render *view)
{
	int rc;

	rc = fclose(view->mf);
	view->mf = NULL;
	fyai_error_check(view->ctx, !rc, out,
			 "could not finish the display buffer");
	if (view->md && (view->args->raw ||
				 fyai_print_markdown(view->md, view->cfg)))
		fputs(view->md, stdout);
	free(view->md);
	view->md = NULL;
	if (view->ctx->stdout_tty && !view->args->raw)
		putchar('\n');
	return 0;
out:
	return -1;
}

/*
 * Human-digestible rendering of the canonical conversation: each message
 * becomes prose rendered through markdown. Unlike `dump`, this is
 * not a faithful serialization - tool results (command output) are reduced
 * to a one-line size summary rather than reproduced verbatim, and assistant
 * tool calls are shown as the invoked command. Honors --first/--last/--range
 * as user-visible exchanges: one user message plus the assistant/tool records
 * that follow it.
 */
int fyai_display_view(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_display_args *args = &cfg->cmd.args.display;
	const char *saved_tool_detail = cfg->tool_detail;
	struct fy_generic_builder_cfg gcfg;
	struct fy_generic_builder *tgb;
	struct fyai_turn_stack stack;
	struct fyai_read_map reads = { 0 };
	size_t lo;
	size_t hi;
	int rc;
	int close_rc;
	struct fyai_display_render view;

	tgb = NULL;
	memset(&stack, 0, sizeof(stack));
	memset(&view, 0, sizeof(view));
	rc = -1;
	if (args->tool_detail)
		cfg->tool_detail = args->tool_detail;
	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	tgb = fy_generic_builder_create(&gcfg);
	fyai_error_check(ctx, tgb, err_out,
			 "could not create display storage");

	rc = fyai_turn_stack_init(&stack, ctx->last_message, fy_invalid);
	fyai_error_check(ctx, !rc, err_out,
			 "could not read the conversation");

	fyai_exchange_window(&args->turn_sel, &stack, &lo, &hi);
	if (fyai_display_outputs_complete(&stack, lo, hi)) {
		rc = fyai_display_stored_outputs(ctx, &stack, lo, hi);
		goto err_out;
	}

	view.ctx = ctx;
	view.cfg = cfg;
	view.args = args;
	view.tgb = tgb;
	view.stack = &stack;
	view.reads = &reads;
	view.mf = open_memstream(&view.md, &view.mdlen);
	fyai_error_check(ctx, view.mf, err_out,
			 "could not create the display buffer");
	fyai_display_scan_reads(&view, lo, hi);
	rc = fyai_display_turns(&view, lo, hi);
	fyai_error_check(ctx, !rc, err_out,
			 "could not render the conversation");
	rc = fyai_display_finish(&view);
	fyai_error_check(ctx, !rc, err_out,
			 "could not finish the conversation display");
	rc = 0;

err_out:
	cfg->tool_detail = saved_tool_detail;
	if (view.mf) {
		close_rc = fclose(view.mf);
		view.mf = NULL;
		if (close_rc)
			fyai_error(ctx, "could not close the display buffer");
	}
	free(view.md);
	fyai_read_map_free(&reads);
	fyai_turn_stack_cleanup(&stack);
	if (tgb)
		fy_generic_builder_destroy(tgb);
	return rc;
}

int fyai_dump_view(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_dump_args *args = &cfg->cmd.args.dump;
	struct fy_generic_builder_cfg gcfg;
	struct fy_generic_builder *tgb;
	struct fyai_turn_stack stack;
	const char *provider;
	const char *role;
	fy_generic msgs;
	fy_generic out;
	fy_generic ps;
	fy_generic m;
	fy_generic tp;
	const char *note;
	size_t count;
	size_t lo;
	size_t hi;
	size_t i;
	bool providers;
	int rc;

	tgb = NULL;
	memset(&stack, 0, sizeof(stack));
	rc = -1;
	providers = args->provider_stream;

	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	tgb = fy_generic_builder_create(&gcfg);
	if (!tgb)
		goto err_out;

	/* The anchored view dumps the full turn graph, no selection. */
	if (args->anchors) {
		emit_generic_to_stdout_anchored("conversation",
			fy_is_valid(ctx->last_message) ?
			ctx->last_message : fy_null, true, true);
		rc = 0;
		goto err_out;
	}

	if (fyai_turn_stack_init(&stack, ctx->last_message, fy_invalid))
		goto err_out;
	count = stack.count;

	fyai_dump_window(&args->turn_sel, count, &lo, &hi);

	out = fy_seq_empty;
	for (i = lo; i < hi; i++) {
		tp = fyai_turn_provider(stack.items[i]);
		provider = fy_castp(&tp, "");

		if (providers) {
			ps = fy_get(stack.items[i], "provider_stream");

			if (fy_is_invalid(ps) ||
			    fy_is_null(ps))
				continue;
			if (args->decorate) {
				note = fy_sprintfa("turn %zu", i);
				ps = fyai_decorate(tgb, ps, note);
			}
			out = fy_append(tgb, out, ps);
			continue;
		}

		/* canonical: flatten this turn's messages */
		msgs = fy_get(stack.items[i], "messages", fy_seq_empty);

		fy_foreach(m, msgs) {
			if (args->decorate) {
				role = fy_get(m, "role", "");

				if (provider)
					note = fy_sprintfa(
						"turn %zu  role=%s  provider=%s",
						i, role, provider);
				else
					note = fy_sprintfa(
						"turn %zu  role=%s", i, role);
				m = fyai_decorate(tgb, m, note);
			}
			out = fy_append(tgb, out, m);
		}
	}

	printf("\n%s:\n", providers ? "provider-streams" : "conversation");
	(void)fy_emit(tgb, out,
		FYOPEF_DISABLE_DIRECTORY | FYOPEF_OUTPUT_TYPE_STDOUT |
		FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_PRETTY |
		FYOPEF_WIDTH_INF | FYOPEF_OUTPUT_COMMENTS, NULL);
	putchar('\n');

	rc = 0;

err_out:
	fyai_turn_stack_cleanup(&stack);
	if (tgb)
		fy_generic_builder_destroy(tgb);
	return rc;
}

fy_generic fyai_list_turns_data(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb)
{
	struct fyai_turn_stack stack;
	fy_generic out;
	const char *prov;
	const char *role;
	const char *api;
	fy_generic meta;
	fy_generic msgs;
	fy_generic m;
	fy_generic m0;
	fy_generic tp;
	fy_generic type;
	long long tokens;
	size_t count;
	size_t i;

	out = fy_seq_empty;
	if (fyai_turn_stack_init(&stack, ctx->last_message, fy_invalid))
		return fy_invalid;
	count = stack.count;

	if (!count)
		goto out;

	for (i = 0; i < count; i++) {
		msgs = fy_get(stack.items[i], "messages", fy_seq_empty);
		m0 = fy_invalid;
		fy_foreach(m, msgs) {
			m0 = m;
			break;
		}

		role = fy_get(m0, "role", "");
		if (!*role) {
			type = fy_get(m0, "type");
			if (fyai_item_type_is_call(type))
				role = "assistant";
			else if (fyai_item_type_is_call_output(type))
				role = "tool";
			else if (fy_equal(type, "message"))
				role = "assistant";
			else
				role = fy_castp(&type, "");
		}

		if (!*role)
			role = "?";

		tp = fyai_turn_provider(stack.items[i]);
		prov = fy_castp(&tp, "");
		meta = fyai_turn_meta(stack.items[i]);
		tokens = fy_get(fy_get(meta, "usage"), "total", 0LL);
		api = fy_get(meta, "api", "-");

		out = fy_append(gb, out,
			fy_mapping(gb,
				   "index", i,
				   "role", fy_value(gb, role),
				   "provider", fy_value(gb, prov ? prov : "-"),
				   "api", fy_value(gb, api),
				   "tokens", tokens));
	}

out:
	fyai_turn_stack_cleanup(&stack);
	return out;
}

/*
 * Walk the root ref log from the current head back along each root's prev link,
 * newest first: one row per root update - turn commits and turnless config
 * changes alike. The ref column is the content-addressed root value (our commit
 * id); kind distinguishes a head-advancing "turn" from a "config"-only update
 * (head unchanged from the predecessor). Capped so a very long history cannot
 * produce unbounded output. Pre-chain roots (no prev) simply end the walk.
 */
#define FYAI_REFLOG_MAX 500

static bool branch_previous_head_matches(const struct fyai_branch *b)
{
	struct fyai_branch previous;

	return fyai_branch_decode(b->prev, &previous) &&
	       b->head.v == previous.head.v;
}

fy_generic fyai_list_reflog_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb)
{
	struct fyai_branch b;
	fy_generic out, entry;
	const char *kind, *name;
	char ref[256];
	long long idx;

	out = fy_seq_empty;
	name = fyai_ctx_branch(ctx);
	entry = ctx->branch_prev;

	for (idx = 0; idx < FYAI_REFLOG_MAX && fy_is_valid(entry);
	     idx++) {
		if (!fyai_branch_decode(entry, &b))
			break;

		/* Compare heads only when the operation was not stored. */
		if (fy_is_valid(b.op)) {
			kind = fy_castp(&b.op, "");
		} else if (fy_is_valid(b.prev)) {
			kind = branch_previous_head_matches(&b) ? "config" : "turn";
		} else {
			kind = fy_is_valid(b.head) ? "turn" : "config";
		}

		/*
		 * The reference is symbolic. A raw arena address would not
		 * survive gc, which relocates objects; "<branch>@{N}" stays
		 * correct and is what fyai_resolve_ref() accepts back.
		 */
		snprintf(ref, sizeof(ref), "%s@{%lld}", name, idx);

		out = fy_append(gb, out,
			fy_mapping(gb,
				   "index", idx,
				   "ref", fy_value(gb, ref),
				   "created", fy_value(gb,
					fy_get(entry, "created", 0LL)),
				   "model", fy_value(gb,
					fy_get(b.config, "model", "-")),
				   "kind", fy_value(gb, kind),
				   "from", fy_value(gb,
					fy_get(entry, "from", ""))));
		/* Validate the next link before following it. */
		if (!fyai_branch_entry_contained(ctx->durable_allocator,
						 b.prev, 1))
			break;
		entry = b.prev;
	}
	return out;
}

fy_generic fyai_list_exchanges_data(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb)
{
	struct fyai_turn_stack stack;
	size_t *starts;
	fy_generic out;
	const char *prov;
	const char *api;
	fy_generic meta;
	fy_generic tp;
	long long tokens;
	size_t count;
	size_t exchanges;
	size_t i;
	size_t j;

	out = fy_seq_empty;
	starts = NULL;
	if (fyai_turn_stack_init(&stack, ctx->last_message, fy_invalid))
		return fy_invalid;
	count = stack.count;

	if (!count)
		goto out;

	starts = calloc(count + 1, sizeof(*starts));
	if (!starts) {
		out = fy_invalid;
		goto out;
	}

	exchanges = 0;
	for (i = 0; i < count; i++)
		if (fyai_turn_has_user_message(stack.items[i]))
			starts[exchanges++] = i;
	if (!exchanges) {
		starts[exchanges++] = 0;
	} else if (starts[0] > 0) {
		starts[0] = 0;
	}
	starts[exchanges] = count;

	for (i = 0; i < exchanges; i++) {
		prov = "-";
		api = "-";
		tokens = 0;
		for (j = starts[i]; j < starts[i + 1]; j++) {
			tp = fyai_turn_provider(stack.items[j]);
			if (fy_is_valid(tp))
				prov = fy_castp(&tp, "");
			meta = fyai_turn_meta(stack.items[j]);
			api = fy_get(meta, "api", api);
			tokens += fy_get(fy_get(meta, "usage"), "total", 0LL);
		}

		out = fy_append(gb, out,
			fy_mapping(gb,
				   "index", i,
				   "provider", fy_value(gb, prov ? prov : "-"),
				   "api", fy_value(gb, api),
				   "tokens", tokens));
	}

out:
	free(starts);
	fyai_turn_stack_cleanup(&stack);
	return out;
}

/* One compact line per internal turn: index, first-message role, provider, tokens. */
int fyai_list_turns(struct fyai_ctx *ctx)
{
	fy_generic turns;
	fy_generic item;
	size_t n;

	turns = fyai_list_turns_data(ctx, ctx->transient_gb);
	n = fy_len(turns);
	if (!n) {
		printf("no turns\n");
		return 0;
	}

	fy_foreach(item, turns) {
		printf("%3lld  %-9s  %-12s  %-15s  tokens=%lld\n",
		       fy_get(item, "index", 0LL), fy_get(item, "role", ""),
		       fy_get(item, "provider", ""), fy_get(item, "api", ""),
		       fy_get(item, "tokens", 0LL));
	}
	return 0;
}


/*
 * On entering the interactive loop, print a short recap of where the
 * conversation stands: the turn count, the per-turn settings recorded on the
 * most recent turn (provider/api/temperature/reasoning), and a one-line preview
 * of the last assistant reply. Chrome goes to stderr so it never pollutes a
 * piped answer stream; coloured only when stderr is a tty.
 */
void fyai_interactive_recap(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *prov;
	const char *api;
	const char *eff;
	const char *sum;
	const char *b;
	const char *d;
	const char *r;
	const char *t;
	fy_generic preview;
	fy_generic meta;
	fy_generic msgs;
	fy_generic cur;
	fy_generic last;
	fy_generic temp;
	fy_generic m;
	fy_generic tp;
	fy_generic role;
	fy_generic type;
	size_t turns;
	size_t len;
	size_t i;
	size_t n;
	bool color;

	color = ansi_color_on(cfg->color, STDERR_FILENO);
	b = color ? FYAI_ANSI_BOLD : "";
	d = color ? FYAI_ANSI_DIM : "";
	r = color ? FYAI_ANSI_RESET : "";
	last = ctx->last_message;
	preview = fy_invalid;
	turns = 0;

	if (fy_is_invalid(last) || fy_is_null(last)) {
		fprintf(stderr, "%sfyai%s interactive - new conversation. "
			"Ctrl-G to edit in $EDITOR, Ctrl-D to exit.\n",
			b, r);
		return;
	}

	fyai_turn_foreach(cur, last)
		if (fyai_turn_has_user_message(cur))
			turns++;

	meta = fyai_turn_meta(last);
	api = fy_get(meta, "api", "");
	tp = fy_get(meta, "provider");
	if (fy_is_invalid(tp))
		tp = fyai_turn_provider(last);
	prov = fy_castp(&tp, "");
	eff = fy_get(meta, "reasoning_effort", "");
	sum = fy_get(meta, "reasoning_summary", "");

	fprintf(stderr, "%sfyai%s interactive - %zu exchange%s\n",
		b, r, turns, turns == 1 ? "" : "s");
	fprintf(stderr, "  %sprovider%s %s", d, r,
		*prov ? prov : "(default)");
	if (*api)
		fprintf(stderr, "  %sapi%s %s", d, r, api);
	temp = fy_get(meta, "temperature");
	if (fy_is_valid(temp) && !fy_is_null(temp))
		fprintf(stderr, "  %stemp%s %g", d, r,
			fy_cast(temp, (double)0.0));
	if (*eff)
		fprintf(stderr, "  %sreasoning%s %s%s%s", d, r, eff,
			*sum ? "/" : "", sum);
	fputc('\n', stderr);

	/* Last assistant reply on the most recent turn, first line only. */
	msgs = fy_get(last, "messages");
	n = fy_len(msgs);
	for (i = n; i-- > 0; ) {
		m = fy_get(msgs, i);
		role = fy_get(m, "role");
		type = fy_get(m, "type");
		if (fy_equal(role, "assistant") ||
		    fy_equal(type, "message")) {
			preview = fyai_item_text(ctx, m);
			break;
		}
	}
	if (fy_is_string(preview)) {
		t = fy_cast(preview, "");
		len = strcspn(t, "\n");
		if (len > 78)
			len = 78;
		if (len)
			fprintf(stderr, "  %s\xe2\x86\xb3 %.*s%s%s\n", d,
				(int)len, t, len == 78 ? "..." : "", r);
	}
	fprintf(stderr, "  %sCtrl-G edit in $EDITOR, Ctrl-D exit%s\n", d, r);
}

/*
 * After linenoise accepts an interactive prompt it leaves the raw "> ..." input
 * line on screen. Erase it and re-display the just-entered text as a user turn:
 * a full-width reverse card rendered by libfymd4c. Only meaningful on a tty
 * with markdown on; otherwise there is nothing to redraw.
 */
/*
 * Emit a blank reverse-card "fence" row: the card background from the styling,
 * then EL (\033[K) to fill the row to the true terminal edge, and a reset. The
 * colour comes from the shipped styling YAML (elements.reverse), read back via
 * markdown_reverse_pair() - no escapes are hard-coded here. @on/@off are that
 * pair; EL and the reset are terminal control, not styling.
 */
static void fyai_bubble_fence(struct fyai_ctx *ctx, const char *on,
			      const char *off)
{
	char *line;
	size_t on_len;
	size_t off_len;

	if (fyai_ui_active(ctx)) {
		on_len = strlen(on);
		off_len = strlen(off);
		line = malloc(on_len + 3 + off_len + 1);
		if (!line)
			return;
		memcpy(line, on, on_len);
		memcpy(line + on_len, "\033[K", 3);
		memcpy(line + on_len + 3, off, off_len);
		line[on_len + 3 + off_len] = '\n';
		(void)fyai_ui_commit(ctx, line, on_len + 3 + off_len + 1);
		free(line);
		return;
	}
	fputs(on, stdout);
	fputs("\033[K", stdout);
	fputs(off, stdout);
	fputc('\n', stdout);
}

static void fyai_print_user_turn(struct fyai_ctx *ctx, const char *line,
				 bool erase_prompt)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct response_buffer rb = {0};
	const char *on;
	const char *off;
	char *quoted;
	size_t quotedlen;
	size_t end;
	bool fenced;
	bool line_start;
	int rc;
	FILE *mf;

	quoted = NULL;
	quotedlen = 0;

	if (!cfg->markdown || !ctx->stdout_tty)
		return;

	/*
	 * In the interactive echo path linenoiseEditStop() has already erased the
	 * whole prompt block and parked the cursor where it began, so the bubble
	 * draws straight over it - no erase needed here (the offline display path
	 * passes erase_prompt=false too).
	 */
	(void)erase_prompt;

	/* Render the input as a blockquote so the bubble keeps the "> " rail. */
	mf = open_memstream(&quoted, &quotedlen);
	if (mf) {
		fyai_emit_blockquote(mf, line);
		fclose(mf);
	}

	rc = quoted ? markdown_render_reverse(cfg, quoted, quotedlen, &rb, true,
					      cfg->theme_variant) : -1;
	if (rc != 0) {
		/* renderer failed or no width: plain fallback. */
		printf("> %s\n", line);
		free(quoted);
		free(rb.data);
		return;
	}
	free(quoted);

	/* Drop renderer padding rows, including ANSI-only SGR/EL rows. */
	end = terminal_trim_blank_rows(rb.data, rb.len);
	line_start = terminal_text_at_line_start(rb.data, end);
	fenced = markdown_reverse_pair(cfg, &on, &off);
	if (fyai_ui_active(ctx)) {
		if (fenced)
			fyai_bubble_fence(ctx, on, off);
		if (end) {
			(void)fyai_ui_commit(ctx, rb.data, end);
			if (!line_start)
				(void)fyai_ui_commit(ctx, "\n", 1);
		}
		if (fenced)
			fyai_bubble_fence(ctx, on, off);
		(void)fyai_ui_commit(ctx, "\n", 1);
		free(rb.data);
		return;
	}

	/* One blank card row fences the content top and bottom (styling
	 * permitting; without a loaded styling the card renders unfenced). */
	if (fenced)
		fyai_bubble_fence(ctx, on, off);
	fwrite(rb.data, 1, end, stdout);
	if (!line_start)
		fputc('\n', stdout);
	if (fenced)
		fyai_bubble_fence(ctx, on, off);
	fputc('\n', stdout);	/* separate the bubble from the answer */
	fflush(stdout);
	free(rb.data);
}

void fyai_echo_user_turn(struct fyai_ctx *ctx, const char *line)
{
	if (ctx && ctx->cfg->markdown && ctx->stdout_tty)
		(void)fyai_render_display_output(ctx, "user", line);
}

int fyai_render_display_output(struct fyai_ctx *ctx, const char *tag,
			       const char *markdown)
{
	struct fyai_cfg *cfg;
	char *quoted = NULL;
	size_t quotedlen = 0;
	char *styled;
	FILE *mf;

	if (!ctx || !tag || !markdown)
		return -1;
	cfg = ctx->cfg;
	if (!strcmp(tag, "user")) {
		if (cfg->markdown && ctx->stdout_tty) {
			fyai_print_user_turn(ctx, markdown, false);
			return 0;
		}
		mf = open_memstream(&quoted, &quotedlen);
		if (!mf)
			return -1;
		fyai_emit_blockquote(mf, markdown);
		fclose(mf);
		fputs(quoted, stdout);
		free(quoted);
		return 0;
	}
	if (!strcmp(tag, "system")) {
		styled = fy_sprintfa("**System**\n\n*%s*\n\n", markdown);
		if (!styled)
			return -1;
		if (fyai_print_markdown(styled, cfg))
			fputs(styled, stdout);
		return 0;
	}
	if (fyai_print_markdown(markdown, cfg))
		fputs(markdown, stdout);
	return 0;
}
