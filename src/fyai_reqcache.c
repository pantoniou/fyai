/* SPDX-License-Identifier: MIT */
/*
 * fyai_reqcache.c - the request body of the last request, kept for the next one
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_grammar.h"
#include "fyai_prof.h"
#include "fyai_provider.h"
#include "fyai_reqcache.h"
#include "fyai_turn.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* The configuration and the catalogue that the kept text was made with. */
struct rc_key {
	uint64_t config;
	uint64_t catalog;
	bool set;
};

/*
 * What the last request of the exchange left. The next request extends it: it
 * starts at the message @skip of the turn @turn, and the stable text of the
 * conversation before that message is in @body.
 */
struct fyai_request_cache {
	struct rc_key key;
	bool valid;
	const void *owner;	/* the transient builder that holds the turns */
	uint64_t turn;		/* word of the turn that holds the first new message */
	size_t skip;		/* messages of that turn that the text holds */
	char *instructions;	/* the system prompt of the conversation */
	/*
	 * The text of the request before the items of its input, up to the
	 * bracket that opens them, and after them, from the bracket that closes
	 * them. A request that builds the same value, which has the same word in
	 * the transient builder, has the same text.
	 */
	char *head;
	size_t head_len;
	char *tail;
	uint64_t probe_word;
	size_t emitted;		/* bytes that the emitter made for this request */
	char *body;		/* the last body, which the caller reads */
	size_t stable_off;	/* the stable items in @body */
	size_t stable_len;
	unsigned long hits;
	unsigned long misses;
};

/* What emit_request_body() writes between two items of an array. */
#define RC_SEP		", "
#define RC_SEPLEN	(sizeof(RC_SEP) - 1)

/*
 * Append the JSON text of the items @from up to @to of @wire, with RC_SEP
 * between two items.
 */
static int rc_emit_items(struct fyai_ctx *ctx, struct fyai_request_cache *cache,
			 struct response_buffer *t, fy_generic wire, size_t from,
			 size_t to)
{
	const char *text;
	size_t i;
	int rc;

	for (i = from; i < to; i++) {
		text = emit_request_body(ctx->transient_gb, fy_get_at(wire, i));
		fyai_error_check(ctx, text, err,
				 "could not serialize an item of the request");
		cache->emitted += strlen(text);
		if (t->len) {
			rc = response_buffer_append_data(t, RC_SEP, RC_SEPLEN);
			fyai_error_check(ctx, !rc, err,
					 "could not keep the request text");
		}
		rc = response_buffer_append(t, text);
		fyai_error_check(ctx, !rc, err, "could not keep the request text");
	}
	return 0;
err:
	return -1;
}

/*
 * Any change of the configuration or the catalogue clears the cache: the
 * model, the endpoint, the system prompt and the way an item is written all
 * come from them.
 */
static void rc_key_take(struct fyai_ctx *ctx, struct rc_key *k)
{
	k->config = ctx->cfg->config_doc.v;
	k->catalog = ctx->cfg->catalog.v;
	k->set = true;
}

static bool rc_key_same(struct fyai_ctx *ctx, const struct rc_key *k)
{
	return k->set && k->config == ctx->cfg->config_doc.v &&
	       k->catalog == ctx->cfg->catalog.v;
}

static void rc_forget_body(struct fyai_request_cache *rc)
{
	free(rc->body);
	rc->body = NULL;
	rc->stable_off = 0;
	rc->stable_len = 0;
}

/* Forget the kept text. */
static void rc_forget_all(struct fyai_request_cache *rc)
{
	rc_forget_body(rc);
	free(rc->instructions);
	rc->instructions = NULL;
	free(rc->head);
	rc->head = NULL;
	free(rc->tail);
	rc->tail = NULL;
	rc->valid = false;
	rc->owner = NULL;
}

void fyai_request_cache_release(struct fyai_ctx *ctx)
{
	struct fyai_request_cache *rc = ctx->request_cache;

	if (!rc)
		return;
	rc_forget_all(rc);
	free(rc);
	ctx->request_cache = NULL;
}

/*
 * Forget the kept text. It names a turn by the word of its storage, and the
 * transient builder that holds the storage is going away: a new object can
 * take the address of an old one, and the old text would then serve another
 * turn. The cache serves the requests of one exchange.
 *
 * A line that runs beside a turn uses a scratch builder. Its end must not
 * forget the text of the turn: a request in flight reads that text.
 */
void fyai_request_cache_forget(struct fyai_ctx *ctx)
{
	struct fyai_request_cache *rc = ctx->request_cache;

	if (rc && (!rc->owner || rc->owner == ctx->transient_gb))
		rc_forget_all(rc);
}

void fyai_request_cache_stats(const struct fyai_ctx *ctx,
			      unsigned long *hits, unsigned long *misses)
{
	const struct fyai_request_cache *rc = ctx->request_cache;

	*hits = rc ? rc->hits : 0;
	*misses = rc ? rc->misses : 0;
}

bool fyai_request_cache_usable(struct fyai_ctx *ctx, fy_generic previous)
{
	struct fyai_cfg *cfg = ctx->cfg;

	if (!cfg->request_cache || cfg->debug || cfg->conversation_logging ||
	    cfg->chatgpt_auth)
		return false;
	if (fy_is_valid(previous) && !fy_is_null(previous))
		return false;
	if (ctx->request_cache && ctx->request_cache->owner &&
	    ctx->request_cache->owner != ctx->transient_gb)
		return false;
	return fyai_grammar_input_splits(ctx);
}

static fy_generic rc_turn_messages(fy_generic turn)
{
	return fy_get(turn, "messages", fy_seq_empty);
}

/*
 * The messages of the turns on @stack, the first @skip messages of the first
 * turn left out, and in @counts the number of messages that each turn gave.
 */
static fy_generic rc_messages(struct fyai_ctx *ctx,
			      const struct fyai_turn_stack *stack, size_t skip,
			      size_t *counts)
{
	fy_generic list, *items, stream;
	size_t i, j, n, off, total = 0, k = 0;

	off = skip;
	for (i = 0; i < stack->count; i++) {
		n = fy_len(rc_turn_messages(stack->items[i]));
		if (off > n)
			off = n;
		counts[i] = n - off;
		total += counts[i];
		off = 0;
	}
	if (!total)
		return fy_seq_empty;

	items = malloc(total * sizeof(*items));
	fyai_error_check(ctx, items, err, "could not collect the turn messages");
	off = skip;
	for (i = 0; i < stack->count; i++) {
		list = rc_turn_messages(stack->items[i]);
		n = fy_len(list);
		if (off > n)
			off = n;
		for (j = off; j < n; j++)
			items[k++] = fy_get_at(list, j);
		off = 0;
	}
	stream = fy_gb_sequence_create(ctx->transient_gb, total, items);
	free(items);
	return stream;
err:
	return fy_invalid;
}

/* The turns from @turn down to @stop, which is not one of them. */
static int rc_stack(struct fyai_ctx *ctx, fy_generic turn, fy_generic stop,
		    struct fyai_turn_stack *stack)
{
	int rc;

	rc = fyai_turn_stack_init(stack, turn, stop);
	fyai_error_check(ctx, !rc, err, "could not allocate the turn stack");
	return 0;
err:
	return -1;
}

/*
 * Find the message @from of the messages on @stack: the index of its turn and
 * its index in that turn. @skip is the number of messages that the first turn
 * gave up. After the last message, the position is the end of the last turn.
 */
static void rc_position(const struct fyai_turn_stack *stack,
			const size_t *counts, size_t skip, size_t from,
			size_t *indexp, size_t *skipp)
{
	fy_generic last;
	size_t i;

	for (i = 0; i < stack->count; i++) {
		if (from < counts[i]) {
			*indexp = i;
			*skipp = (i == 0 ? skip : 0) + from;
			return;
		}
		from -= counts[i];
	}
	last = stack->items[stack->count - 1];
	*indexp = stack->count - 1;
	*skipp = fy_len(rc_turn_messages(last));
}

/*
 * Append the text of the pair @key and @value to @out, after a comma when @out
 * holds a pair already. @prefix is what @out must hold before any pair.
 */
static int rc_emit_pair(struct fyai_ctx *ctx, struct fyai_request_cache *rc,
			struct response_buffer *out, size_t prefix,
			fy_generic key, const char *value_text, fy_generic value)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	const char *key_text, *text;
	int rv;

	key_text = emit_request_body(gb, key);
	fyai_error_check(ctx, key_text, err,
			 "could not serialize the model request");
	rc->emitted += strlen(key_text);
	if (out->len > prefix) {
		rv = response_buffer_append(out, ", ");
		fyai_error_check(ctx, !rv, err, "could not keep the request text");
	}
	rv = response_buffer_append(out, key_text);
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	rv = response_buffer_append(out, ": ");
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	if (value_text) {
		rv = response_buffer_append(out, value_text);
		fyai_error_check(ctx, !rv, err, "could not keep the request text");
		return 0;
	}
	text = emit_request_body(gb, value);
	fyai_error_check(ctx, text, err,
			 "could not serialize the model request");
	rc->emitted += strlen(text);
	rv = response_buffer_append(out, text);
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	return 0;
err:
	return -1;
}

/*
 * The text of the request with a one item input, cut at the items: *headp is
 * the text up to the bracket that opens the input, and *tailp the text from the
 * bracket that closes it. The transient builder dedups, so a request that is
 * the same value as the last one has the same word and the same text: the tools
 * and the options are emitted one time for the exchange. A request that is not
 * the same is emitted one time, pair by pair, and cut at the pair whose value is
 * the input. Returns 1 when no pair is.
 */
static int rc_split_request(struct fyai_ctx *ctx, struct fyai_request_cache *rc,
			    struct fyai_grammar_request *req,
			    const char **headp, const char **tailp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct response_buffer head = { 0 }, tail = { 0 };
	fy_generic request, probe, key, value;
	bool found = false;
	uint64_t word;
	int rv;

	probe = fy_sequence(gb, 0LL);
	req->input = probe;
	request = fyai_grammar_build_request(ctx, req);
	fyai_error_check(ctx, fy_is_valid(request), err,
			 "could not build the model request");
	word = request.v;
	if (rc->head && word == rc->probe_word) {
		*headp = rc->head;
		*tailp = rc->tail;
		return 0;
	}
	if (!fy_is_mapping(request))
		return 1;

	rv = response_buffer_append(&head, "{");
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	fy_foreach_key_value(key, value, request) {
		if (!found && fy_equal(value, probe)) {
			rv = rc_emit_pair(ctx, rc, &head, 1, key, "[", fy_invalid);
			if (rv)
				goto err;
			rv = response_buffer_append(&tail, "]");
			fyai_error_check(ctx, !rv, err,
					 "could not keep the request text");
			found = true;
			continue;
		}
		rv = found ? rc_emit_pair(ctx, rc, &tail, 0, key, NULL, value) :
			     rc_emit_pair(ctx, rc, &head, 1, key, NULL, value);
		if (rv)
			goto err;
	}
	if (!found) {
		free(head.data);
		free(tail.data);
		return 1;
	}
	rv = response_buffer_append(&tail, "}");
	fyai_error_check(ctx, !rv, err, "could not keep the request text");

	free(rc->head);
	free(rc->tail);
	rc->head = head.data;
	rc->head_len = head.len;
	rc->tail = tail.data;
	rc->probe_word = word;
	*headp = rc->head;
	*tailp = rc->tail;
	return 0;
err:
	free(head.data);
	free(tail.data);
	return -1;
}

int fyai_request_cache_body(struct fyai_ctx *ctx, fy_generic turn,
			    struct fyai_grammar_request *req,
			    const char **bodyp)
{
	struct fyai_request_cache *rc = ctx->request_cache;
	struct fyai_turn_stack stack = { 0 };
	struct response_buffer added = { 0 }, tail = { 0 };
	const char *head, *tail_text, *instructions;
	char *body = NULL, *kept = NULL, *copy;
	size_t *counts = NULL;
	fy_generic cur, stop = fy_invalid, wire, messages, m;
	size_t stable = 0, from = 0, nwire, skip = 0, new_skip = 0;
	size_t index, old_len = 0, stable_len, len;
	bool hit = false;
	int rv;

	if (fy_is_invalid(turn) || fy_is_null(turn))
		return 0;

	if (!rc) {
		rc = calloc(1, sizeof(*rc));
		fyai_error_check(ctx, rc, err, "could not keep the request cache");
		ctx->request_cache = rc;
	}
	if (!rc_key_same(ctx, &rc->key)) {
		if (rc->key.set)
			fyai_diag_tracef("request-cache", "the key changed");
		rc_forget_all(rc);
		rc_key_take(ctx, &rc->key);
	}

	/*
	 * Walk back from the last turn to the turn where the last request
	 * ended. Only the turns from there on are new.
	 */
	if (rc->valid) {
		fyai_turn_foreach(cur, turn) {
			if (cur.v != rc->turn)
				continue;
			hit = true;
			stop = fy_get(cur, "previous", fy_invalid);
			break;
		}
	}
	if (hit) {
		skip = rc->skip;
		old_len = rc->stable_len;
	}
	rv = rc_stack(ctx, turn, stop, &stack);
	if (rv)
		goto err;
	if (!stack.count)
		goto none;

	counts = calloc(stack.count, sizeof(*counts));
	fyai_error_check(ctx, counts, err, "could not keep the request cache");
	messages = rc_messages(ctx, &stack, skip, counts);
	fyai_error_check(ctx, fy_is_valid(messages), err,
			 "could not collect the turn messages");

	/* The conversation names its own system prompt, once, at its start. */
	if (hit) {
		instructions = rc->instructions;
	} else {
		instructions = req->instructions;
		fy_foreach(m, messages) {
			if (fy_equal(fy_get(m, "role"), "system")) {
				instructions = fy_get(m, "content", instructions);
				break;
			}
		}
	}
	req->instructions = instructions;

	wire = fyai_grammar_input_split(ctx, messages, &stable, &from);
	fyai_error_check(ctx, fy_is_valid(wire), err,
			 "could not build the request input");
	nwire = fy_len(wire);
	if (stable > nwire)
		stable = nwire;

	rc->emitted = 0;
	rv = rc_split_request(ctx, rc, req, &head, &tail_text);
	if (rv < 0)
		goto err;
	if (rv)
		goto none;

	/* The items that become stable, and the items that stay open. */
	rv = rc_emit_items(ctx, rc, &added, wire, 0, stable);
	if (rv)
		goto err;
	rv = rc_emit_items(ctx, rc, &tail, wire, stable, nwire);
	if (rv)
		goto err;

	stable_len = old_len + (old_len && added.len ? RC_SEPLEN : 0) + added.len;
	len = rc->head_len + stable_len + (stable_len && tail.len ? RC_SEPLEN : 0) +
	      tail.len + strlen(tail_text);
	body = malloc(len + 1);
	fyai_error_check(ctx, body, err, "could not keep the request text");
	copy = body;
	memcpy(copy, head, rc->head_len);
	copy += rc->head_len;
	if (old_len) {
		memcpy(copy, rc->body + rc->stable_off, old_len);
		copy += old_len;
	}
	if (old_len && added.len) {
		memcpy(copy, RC_SEP, RC_SEPLEN);
		copy += RC_SEPLEN;
	}
	if (added.len) {
		memcpy(copy, added.data, added.len);
		copy += added.len;
	}
	if (stable_len && tail.len) {
		memcpy(copy, RC_SEP, RC_SEPLEN);
		copy += RC_SEPLEN;
	}
	if (tail.len) {
		memcpy(copy, tail.data, tail.len);
		copy += tail.len;
	}
	strcpy(copy, tail_text);

	/* Where the next request starts: the first message that is not stable. */
	rc_position(&stack, counts, skip, from, &index, &new_skip);
	if (!hit) {
		kept = strdup(instructions ? instructions : "");
		fyai_error_check(ctx, kept, err, "could not keep the request cache");
		free(rc->instructions);
		rc->instructions = kept;
	}
	rc_forget_body(rc);
	rc->body = body;
	body = NULL;
	rc->stable_off = rc->head_len;
	rc->stable_len = stable_len;
	rc->turn = stack.items[index].v;
	rc->skip = new_skip;
	rc->valid = true;
	rc->owner = ctx->transient_gb;
	if (hit)
		rc->hits++;
	else
		rc->misses++;
	fyai_prof_count(hit ? "request_cache_hit" : "request_cache_miss", 1);
	fyai_diag_tracef("request-cache", "%s: %zu bytes, %zu stable, %zu emitted",
			 hit ? "hit" : "miss", len, stable_len, rc->emitted);

	free(added.data);
	free(tail.data);
	free(counts);
	fyai_turn_stack_cleanup(&stack);
	*bodyp = rc->body;
	return 1;

none:
	free(added.data);
	free(tail.data);
	free(counts);
	free(body);
	fyai_turn_stack_cleanup(&stack);
	return 0;
err:
	if (rc)
		rc_forget_all(rc);
	free(added.data);
	free(tail.data);
	free(counts);
	free(body);
	fyai_turn_stack_cleanup(&stack);
	return -1;
}
