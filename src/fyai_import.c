/*
 * fyai_import.c - read the textual conversation export back into the arena
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "fyai_auth.h"
#include "fyai_branch.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_textual.h"
#include "fyai_turn.h"
#include "utils.h"

/* Remove the presentation heading from a message body. */
static char *fyai_import_strip_heading(char *body)
{
	char *nl;

	while (*body == '\n')
		body++;
	if (body[0] != '#')
		return body;
	nl = strchr(body, '\n');
	if (!nl)
		return body + strlen(body);
	body = nl + 1;
	while (*body == '\n')
		body++;
	return body;
}

/* Trim the blank run a directive leaves before the next one. */
static void fyai_import_trim_tail(char *body)
{
	size_t len;

	len = strlen(body);
	while (len && (body[len - 1] == '\n' || body[len - 1] == '\r'))
		len--;
	body[len] = '\0';
}

/* Parsed turn and publish boundaries. */
struct fyai_import_turn {
	fy_generic messages;		/* sequence of message generics */
	/* Re-issue a compaction instead of restoring its provider output. */
	bool compact;
	fy_generic instructions;
};

struct fyai_import_publish {
	fy_generic config;		/* fy_invalid when unchanged */
	struct fyai_import_turn *turns;
	size_t turn_count;
	size_t turn_capacity;
};

struct fyai_import {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	struct fyai_import_publish *publishes;
	size_t count;
	size_t capacity;
	fy_generic config;		/* running configuration */
	fy_generic pending;		/* results owed to the next turn */
	unsigned int call_seq;
	bool ignore_compact;
	bool request_ready;
};

/* Remove one escape 'x' from an opener at column zero. */
static void fyai_import_unescape(char *text)
{
	char *out;
	const char *p;
	const char *run;
	const char *tail;
	bool at_column_zero;

	out = text;
	at_column_zero = true;
	for (p = text; *p; ) {
		if (!at_column_zero ||
		    strncmp(p, FYAI_TEXTUAL_PREFIX, FYAI_TEXTUAL_PREFIX_LEN)) {
			at_column_zero = *p == '\n';
			*out++ = *p++;
			continue;
		}
		run = p + FYAI_TEXTUAL_PREFIX_LEN;
		tail = run;
		while (*tail == 'x')
			tail++;
		if (tail == run || *tail != '-' ||
		    strncmp(tail + 1, FYAI_TEXTUAL_TAIL, FYAI_TEXTUAL_TAIL_LEN)) {
			at_column_zero = false;
			*out++ = *p++;
			continue;
		}
		/* Drop exactly one 'x', and the separator with the last. */
		memcpy(out, FYAI_TEXTUAL_PREFIX, FYAI_TEXTUAL_PREFIX_LEN);
		out += FYAI_TEXTUAL_PREFIX_LEN;
		memcpy(out, run, (size_t)(tail - run) - 1);
		out += (size_t)(tail - run) - 1;
		if (tail - run > 1)
			*out++ = '-';
		memcpy(out, FYAI_TEXTUAL_TAIL, FYAI_TEXTUAL_TAIL_LEN);
		out += FYAI_TEXTUAL_TAIL_LEN;
		p = tail + 1 + FYAI_TEXTUAL_TAIL_LEN;
		at_column_zero = false;
	}
	*out = '\0';
}

static struct fyai_import_publish *fyai_import_publish_add(struct fyai_import *im)
{
	struct fyai_import_publish *items;
	size_t capacity;

	if (im->count == im->capacity) {
		capacity = im->capacity ? im->capacity * 2 : 8;
		items = realloc(im->publishes, capacity * sizeof(*items));
		fyai_error_check(im->ctx, items, err, "import: out of memory");
		im->publishes = items;
		im->capacity = capacity;
	}
	items = &im->publishes[im->count++];
	memset(items, 0, sizeof(*items));
	items->config = fy_invalid;
	return items;
err:
	return NULL;
}

static struct fyai_import_turn *fyai_import_turn_add(struct fyai_import *im,
					     struct fyai_import_publish *pu)
{
	struct fyai_import_turn *items;
	size_t capacity;

	if (pu->turn_count == pu->turn_capacity) {
		capacity = pu->turn_capacity ? pu->turn_capacity * 2 : 8;
		items = realloc(pu->turns, capacity * sizeof(*items));
		fyai_error_check(im->ctx, items, err, "import: out of memory");
		pu->turns = items;
		pu->turn_capacity = capacity;
	}
	items = &pu->turns[pu->turn_count++];
	memset(items, 0, sizeof(*items));
	items->messages = fy_seq_empty;
	items->instructions = fy_invalid;
	return items;
err:
	return NULL;
}

/* Apply one slash-path delta; a null value removes the key. */
static fy_generic fyai_import_apply_update(struct fyai_import *im,
					   fy_generic config, fy_generic update)
{
	fy_generic key;
	fy_generic val;
	size_t i, n;

	if (fy_generic_is_invalid(config))
		config = fy_gb_mapping(im->gb);
	n = fy_generic_mapping_get_pair_count(update);
	for (i = 0; i < n; i++) {
		key = fy_generic_mapping_get_at_key(update, i);
		val = fy_generic_mapping_get_at_value(update, i);
		if (!fy_generic_is_string(key))
			continue;
		if (fy_generic_is_null_type(val))
			config = fy_delete_at_pathstr(im->gb, config,
						      fy_castp(&key, ""));
		else
			config = fy_set_at_pathstr(im->gb, config,
						   fy_castp(&key, ""), val);
	}
	return config;
}

static int fyai_import_message_add(struct fyai_import *im,
				   struct fyai_import_turn *turn,
				   fy_generic message)
{
	turn->messages = fy_append(im->gb, turn->messages, message);
	return fy_generic_is_valid(turn->messages) ? 0 : -1;
}

/*
 * A call becomes two canonical messages: the call in this turn, and its output
 * in the next one, because the turn ends when the model stops to run the tool.
 */
static int fyai_import_tool_call(struct fyai_import *im,
				 struct fyai_import_turn *turn,
				 fy_generic directive)
{
	fy_generic args;
	fy_generic name;
	fy_generic result;
	fy_generic call;
	fy_generic args_text;
	char call_id[64];
	int rc;

	name = fy_get(directive, "tool");
	args = fy_get(directive, "arguments", fy_invalid);
	if (fy_generic_is_invalid(args))
		args = fy_gb_mapping(im->gb);
	/* Canonical state keeps tool arguments as the JSON text the provider
	 * sent, so the export's mapping is emitted back to that shape. */
	args_text = fy_gb_emit(im->gb, args,
			       FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_JSON |
			       FYOPEF_STYLE_COMPACT | FYOPEF_WIDTH_INF, NULL);
	fyai_error_check(im->ctx, fy_generic_is_valid(args_text), err,
			 "import: cannot encode tool arguments");
	/*
	 * The export stores no provider tool-call ID, so import mints one. It
	 * only has to be unique inside this conversation: canonical state uses
	 * it to pair a call with its output and nothing else reads it.
	 */
	snprintf(call_id, sizeof(call_id), "import_call_%u", ++im->call_seq);
	call = fy_mapping(im->gb, "type", "function_call",
			  "call_id", call_id,
			  "name", name,
			  "arguments", args_text);
	rc = fyai_import_message_add(im, turn, call);
	fyai_error_check(im->ctx, !rc, err, "import: cannot add a tool call");
	result = fy_get(directive, "result", fy_invalid);
	if (fy_generic_is_invalid(result))
		return 0;
	im->pending = fy_append(im->gb, im->pending,
		fy_mapping(im->gb, "type", "function_call_output",
			   "call_id", call_id, "output", result));
	fyai_error_check(im->ctx, fy_generic_is_valid(im->pending), err,
			 "import: cannot add a tool result");
	return 0;
err:
	return -1;
}

/* Read the complete input stream. */
static char *fyai_import_read_stream(struct fyai_ctx *ctx, FILE *fp)
{
	char *data;
	char *grown;
	size_t len;
	size_t cap;
	size_t n;

	cap = 65536;
	len = 0;
	data = malloc(cap);
	fyai_error_check(ctx, data, err, "import: out of memory");
	for (;;) {
		n = fread(data + len, 1, cap - len - 1, fp);
		fyai_error_check(ctx, n || !ferror(fp), err,
				 "import: cannot read input");
		len += n;
		if (len + 1 < cap)
			break;
		cap *= 2;
		grown = realloc(data, cap);
		fyai_error_check(ctx, grown, err, "import: out of memory");
		data = grown;
	}
	data[len] = '\0';
	return data;
err:
	free(data);
	return NULL;
}

/* Directive text between the opener and the terminator, as a YAML mapping. */
static fy_generic fyai_import_parse_directive(struct fyai_import *im,
					      const char *text, size_t len)
{
	fy_generic_sized_string input;

	input.data = text;
	input.size = len;
	return fy_parse(im->gb, input,
			FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
}

static void fyai_import_cleanup(struct fyai_import *im)
{
	size_t i;

	for (i = 0; i < im->count; i++)
		free(im->publishes[i].turns);
	free(im->publishes);
}

static bool fyai_import_is_directive(const char *line)
{
	size_t len;

	len = sizeof(FYAI_TEXTUAL_OPEN) - 1;
	return !strncmp(line, FYAI_TEXTUAL_OPEN, len) &&
	       (line[len] == '\n' || line[len] == '\0');
}

/*
 * Parse the document into publishes, turns and messages. Prose that follows a
 * message directive is its body, up to the next column-zero opener.
 */
static int fyai_import_parse(struct fyai_import *im, char *text)
{
	struct fyai_ctx *ctx = im->ctx;
	struct fyai_import_publish *pu;
	struct fyai_import_turn *turn;
	fy_generic directive;
	fy_generic kind;
	fy_generic update;
	char *line;
	char *next;
	char *close;
	char *body;
	char *body_end;
	const char *kind_text;
	const char *role;
	bool pending_body;
	int rc;

	pu = NULL;
	turn = NULL;
	body = NULL;
	pending_body = false;
	role = NULL;
	for (line = text; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next)
			next++;
		if (!fyai_import_is_directive(line)) {
			if (!body && pending_body)
				body = line;
			continue;
		}
		/* A directive ends the body that was being collected. */
		if (pending_body && body) {
			/* Terminate the prose before the next directive. */
			body_end = line > text && line[-1] == '\n' ? line - 1 : line;
			*body_end = '\0';
			body = fyai_import_strip_heading(body);
			fyai_import_trim_tail(body);
			fyai_import_unescape(body);
			rc = fyai_import_message_add(im, turn,
				fy_mapping(im->gb, "role", role,
					   "content", body));
			fyai_error_check(ctx, !rc, err, "import: out of memory");
		}
		pending_body = false;
		body = NULL;
		close = strstr(line, "\n" FYAI_TEXTUAL_CLOSE "\n");
		fyai_error_check(ctx, close, err,
				 "import: a directive has no terminator");
		directive = fyai_import_parse_directive(im,
			line + sizeof(FYAI_TEXTUAL_OPEN) - 1,
			(size_t)(close - (line + sizeof(FYAI_TEXTUAL_OPEN) - 1)));
		fyai_error_check(ctx, fy_generic_is_mapping(directive), err,
				 "import: a directive is not a YAML mapping");
		next = close + strlen("\n" FYAI_TEXTUAL_CLOSE "\n");
		kind = fy_get(directive, "kind", fy_invalid);
		fyai_error_check(ctx, fy_generic_is_string(kind), err,
				 "import: a directive has no kind");
		kind_text = fy_castp(&kind, "");
		if (fy_equal(kind, "conversation"))
			continue;
		if (fy_equal(kind, "publish")) {
			pu = fyai_import_publish_add(im);
			fyai_error_check(ctx, pu, err, "import: out of memory");
			turn = NULL;
			continue;
		}
		fyai_error_check(ctx, pu, err,
				 "import: %s appears before any publish", kind_text);
		if (fy_equal(kind, "config")) {
			im->config = fy_get(directive, "config", fy_invalid);
			pu->config = im->config;
			continue;
		}
		if (fy_equal(kind, "config-update")) {
			update = fy_get(directive, "config-update", fy_invalid);
			fyai_error_check(ctx, fy_generic_is_mapping(update), err,
				"import: config-update is not a mapping");
			im->config = fyai_import_apply_update(im, im->config,
							      update);
			pu->config = im->config;
			continue;
		}
		if (fy_equal(kind, "compact")) {
			turn = fyai_import_turn_add(im, pu);
			fyai_error_check(ctx, turn, err, "import: out of memory");
			turn->compact = true;
			turn->instructions = fy_get(directive, "instructions",
						    fy_invalid);
			turn->messages = im->pending;
			im->pending = fy_seq_empty;
			turn = NULL;
			continue;
		}
		if (fy_equal(kind, "turn")) {
			turn = fyai_import_turn_add(im, pu);
			fyai_error_check(ctx, turn, err, "import: out of memory");
			/* Results owed by the previous turn open this one. */
			turn->messages = im->pending;
			im->pending = fy_seq_empty;
			continue;
		}
		fyai_error_check(ctx, turn, err,
				 "import: %s appears outside a turn", kind_text);
		if (fy_equal(kind, "tool_call")) {
			rc = fyai_import_tool_call(im, turn, directive);
			fyai_error_check(ctx, !rc, err,
					 "import: cannot read a tool call");
			continue;
		}
		if (fy_equal(kind, "message")) {
			kind = fy_get(directive, "role", fy_invalid);
			fyai_error_check(ctx, fy_generic_is_string(kind), err,
					 "import: a message has no role");
			role = fy_castp(&kind, "");
			pending_body = true;
			continue;
		}
		fyai_error_check(ctx, false, err,
				 "import: unknown directive kind '%s'", kind_text);
	}
	if (pending_body && body && turn) {
		body = fyai_import_strip_heading(body);
		fyai_import_trim_tail(body);
		fyai_import_unescape(body);
		rc = fyai_import_message_add(im, turn,
			fy_mapping(im->gb, "role", role, "content", body));
		fyai_error_check(ctx, !rc, err, "import: out of memory");
	}
	return 0;
err:
	return -1;
}

/* Build the request machinery the first time a compaction needs it. */
static int fyai_import_request_setup(struct fyai_import *im)
{
	struct fyai_ctx *ctx = im->ctx;
	struct fy_generic_builder *gb;
	int rc;

	if (im->request_ready)
		return 0;
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err,
			 "import: cannot create the request builder");
	rc = asprintf(&ctx->user_agent, "%s/%s", "fyai", VERSION);
	fyai_error_check(ctx, rc != -1, err,
			 "import: cannot build the user agent");
	rc = fyai_curl_easy_reinit(ctx);
	fyai_error_check(ctx, !rc, err, "import: cannot initialize curl");
	rc = fyai_auth_resolve(ctx);
	fyai_error_check(ctx, !rc, err, "import: cannot resolve authentication");
	rc = fyai_request_state_apply(ctx);
	fyai_error_check(ctx, !rc, err, "import: cannot prepare the request");
	im->request_ready = true;
	return 0;
err:
	return -1;
}

/* Materialize the branch and re-issue its compaction. */
static int fyai_import_compact(struct fyai_import *im,
			       struct fyai_import_publish *pu,
			       struct fyai_import_turn *turn,
			       fy_generic head, fy_generic *headp)
{
	struct fyai_ctx *ctx = im->ctx;
	const char *hint;
	int rc;

	/* A document without a compaction does not need request state. */
	rc = fyai_import_request_setup(im);
	fyai_error_check(ctx, !rc, err,
			 "import: cannot prepare the compaction request");
	ctx->last_message = head;
	fyai_branch_op_set(ctx, "import", NULL);
	rc = fyai_publish_root(ctx, pu->config, fy_invalid, head);
	fyai_error_check(ctx, !rc, err,
			 "import: cannot materialize before compaction");
	/* Use the current model and endpoint. */
	hint = fy_generic_is_string(turn->instructions) ?
			fy_castp(&turn->instructions, "") : NULL;
	rc = fyai_session_compact(ctx, hint);
	fyai_error_check(ctx, !rc, err, "import: the compaction failed");
	*headp = ctx->last_message;
	return 0;
err:
	return -1;
}

/* Replay all publish boundaries. */
static int fyai_import_replay(struct fyai_import *im)
{
	struct fyai_ctx *ctx = im->ctx;
	struct fyai_import_publish *pu;
	fy_generic head;
	fy_generic turn;
	size_t i, j;
	int rc;

	head = fy_invalid;
	for (i = 0; i < im->count; i++) {
		pu = &im->publishes[i];
		for (j = 0; j < pu->turn_count; j++) {
			if (pu->turns[j].compact) {
				/* Keep the conversation before the compaction. */
				if (im->ignore_compact)
					continue;
				rc = fyai_import_compact(im, pu, &pu->turns[j],
							 head, &head);
				fyai_error_check(ctx, !rc, err,
					"import: cannot compact");
				continue;
			}
			/* Build the imported turn in the durable builder. */
			turn = fy_gb_internalize(im->gb,
				fy_mapping(im->gb,
					"previous", fy_generic_is_valid(head) ?
							head : fy_null,
					"messages", pu->turns[j].messages,
					"metadata", fy_mapping(im->gb,
						"imported", true)));
			fyai_error_check(ctx, fy_generic_is_valid(turn), err,
					 "import: cannot build a turn");
			head = turn;
		}
		fyai_branch_op_set(ctx, "import", NULL);
		rc = fyai_publish_root(ctx, pu->config, fy_invalid, head);
		fyai_error_check(ctx, !rc, err,
				 "import: cannot publish a boundary");
	}
	return 0;
err:
	return -1;
}

int fyai_import_view(struct fyai_ctx *ctx, const char *path)
{
	struct fyai_import im;
	const char *name;
	FILE *fp;
	char *text;
	int rc;

	memset(&im, 0, sizeof(im));
	text = NULL;
	fp = NULL;
	rc = -1;
	im.ctx = ctx;
	im.config = fy_invalid;
	im.pending = fy_seq_empty;
	im.gb = ctx->gb;
	im.ignore_compact = ctx->cfg->cmd.args.import.ignore_compact;
	name = path ? path : "standard input";
	/* Do not append an import to an explicitly selected conversation. */
	if (ctx->cfg->branch_explicit)
		fyai_error_check(ctx,
			fy_generic_is_invalid(ctx->last_message) ||
			fy_generic_is_null_type(ctx->last_message), out,
			"import: branch '%s' already holds a conversation",
			fyai_ctx_branch(ctx));
	if (path) {
		fp = fopen(path, "rb");
		fyai_error_check(ctx, fp, out, "import: cannot read %s", path);
	} else
		fp = stdin;
	text = fyai_import_read_stream(ctx, fp);
	if (fp != stdin)
		fclose(fp);
	fp = NULL;
	fyai_error_check(ctx, text, out, "import: cannot read %s", name);
	rc = fyai_import_parse(&im, text);
	fyai_error_check(ctx, !rc, out, "import: cannot read %s", name);
	fyai_error_check(ctx, im.count, out,
			 "import: %s holds no publish boundary", name);
	rc = fyai_import_replay(&im);
	fyai_error_check(ctx, !rc, out, "import: cannot replay %s", name);
	rc = 0;
out:
	free(text);
	fyai_import_cleanup(&im);
	return rc;
}
