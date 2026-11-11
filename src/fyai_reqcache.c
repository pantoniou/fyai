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
	/*
	 * A turn that the exchange made in the transient builder is stored as
	 * another value, with the display of a call. The text then holds the
	 * turns up to the newest turn of the chain that the arena holds,
	 * @anchor, which the next exchange has too, and the first @anchored_n
	 * messages after it, whose text is @check: they must be what the
	 * exchange stored.
	 */
	bool anchored;
	uint64_t anchor;
	size_t anchored_n;
	char *check;
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
	/*
	 * The request that made @head and @tail, with an empty input, in a
	 * builder of the cache. The next exchange has other values, so it
	 * compares by content: an equal request has the same text.
	 */
	struct fy_generic_builder *probe_gb;
	fy_generic probe_copy;
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
			 struct fyai_buf *t, fy_generic wire, size_t from,
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
			rc = fyai_buf_append_data(t, RC_SEP, RC_SEPLEN);
			fyai_error_check(ctx, !rc, err,
					 "could not keep the request text");
		}
		rc = fyai_buf_append(t, text);
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
	rc->probe_copy = fy_invalid;
	rc->probe_word = 0;
	free(rc->check);
	rc->check = NULL;
	rc->anchored = false;
	rc->valid = false;
	rc->owner = NULL;
}

void fyai_request_cache_release(struct fyai_ctx *ctx)
{
	struct fyai_request_cache *rc = ctx->request_cache;

	if (!rc)
		return;
	rc_forget_all(rc);
	if (rc->probe_gb)
		fy_generic_builder_destroy(rc->probe_gb);
	free(rc);
	ctx->request_cache = NULL;
}

static fy_generic rc_turn_messages(fy_generic turn)
{
	return fy_get(turn, "messages", fy_seq_empty);
}

/* The text of the first @count messages of the turns on @stack, or NULL. */
static char *rc_stack_text(struct fyai_ctx *ctx,
			   const struct fyai_turn_stack *stack, size_t count)
{
	fy_generic list, *items, seq;
	const char *text;
	char *copy = NULL;
	size_t i, j, n, k = 0;

	items = malloc((count + 1) * sizeof(*items));
	if (!items)
		return NULL;
	for (i = 0; i < stack->count && k < count; i++) {
		list = rc_turn_messages(stack->items[i]);
		n = fy_len(list);
		for (j = 0; j < n && k < count; j++)
			items[k++] = fy_get_at(list, j);
	}
	if (k == count) {
		seq = fy_gb_sequence_create(ctx->transient_gb, count, items);
		if (fy_is_valid(seq)) {
			text = emit_request_body(ctx->transient_gb, seq);
			copy = text ? strdup(text) : NULL;
		}
	}
	free(items);
	return copy;
}

/*
 * True when the turns after @stop at @turn begin with the @rc->anchored_n
 * messages that the exchange sent, as the exchange stored them.
 */
static bool rc_check(struct fyai_ctx *ctx, const struct fyai_request_cache *rc,
		     fy_generic turn, fy_generic stop)
{
	struct fyai_turn_stack stack = { 0 };
	char *text = NULL;
	bool same = false;

	if (!rc->check || fyai_turn_stack_init(&stack, turn, stop))
		return false;
	text = rc_stack_text(ctx, &stack, rc->anchored_n);
	same = text && !strcmp(text, rc->check);
	free(text);
	fyai_turn_stack_cleanup(&stack);
	return same;
}

/*
 * The transient builder that holds the request values is going away. The text
 * that its word made, the head and the tail of the request, is forgotten: a
 * new object can take the address of an old one, and the old text would then
 * serve another request.
 *
 * The turns are not in that builder. The exchange stored them in the arena,
 * which does not move, and the next exchange has the turn that the text ends
 * at in its chain: the text and the turn stay. A turn that the arena does not
 * hold is forgotten with everything else.
 *
 * A line that runs beside a turn uses a scratch builder. Its end must not
 * forget the text of the turn: a request in flight reads that text.
 */
void fyai_request_cache_forget(struct fyai_ctx *ctx)
{
	struct fyai_request_cache *rc = ctx->request_cache;
	struct fyai_turn_stack stack = { 0 };
	fy_generic cur, anchor = fy_invalid, turn;
	char *check = NULL;
	size_t i, n = 0;

	if (!rc || (rc->owner && rc->owner != ctx->transient_gb))
		return;
	if (!rc->valid || !rc->owner || !rc->body || !ctx->gb) {
		rc_forget_all(rc);
		return;
	}
	turn.v = rc->turn;
	/*
	 * When the last turn is in the arena, the word of the turn and the text
	 * stay as they are. A line with a tool call stores its last turn as
	 * another value, so the word names a value of the transient builder. In
	 * that case the text is anchored at the newest turn of the chain that the
	 * arena holds: the next exchange finds that turn again. The messages
	 * after it are not in the arena, so their text is kept in @check and
	 * compared with what the exchange stored before the text is reused.
	 */
	if (!fy_generic_builder_contains_out_of_place(ctx->gb, turn)) {
		/* The newest turn of its chain that the arena holds. */
		fyai_turn_foreach(cur, turn) {
			if (fy_generic_builder_contains_out_of_place(ctx->gb, cur)) {
				anchor = cur;
				break;
			}
		}
		if (fy_is_invalid(anchor) ||
		    fyai_turn_stack_init(&stack, turn, anchor) || !stack.count)
			goto drop;
		for (i = 0; i + 1 < stack.count; i++)
			n += fy_len(rc_turn_messages(stack.items[i]));
		n += rc->skip;
		check = rc_stack_text(ctx, &stack, n);
		if (!check)
			goto drop;
		fyai_turn_stack_cleanup(&stack);
		free(rc->check);
		rc->check = check;
		rc->anchored = true;
		rc->anchor = anchor.v;
		rc->anchored_n = n;
		/* The word of a turn that the builder held can name another. */
		rc->turn = 0;
	}
	/*
	 * The head and the tail stay: the next request compares its own with
	 * @probe_copy. Their word names a value of the builder that goes.
	 */
	rc->probe_word = 0;
	rc->owner = NULL;
	return;
drop:
	fyai_turn_stack_cleanup(&stack);
	free(check);
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

/*
 * The messages of the turns on @stack, the first @skip messages left out, and
 * in @counts the number of messages that each turn gave. The messages that
 * are left out can span the first turns.
 */
static fy_generic rc_messages(struct fyai_ctx *ctx,
			      const struct fyai_turn_stack *stack, size_t skip,
			      size_t *counts)
{
	fy_generic list, *items, stream;
	size_t i, j, n, off, take, total = 0, k = 0;

	off = skip;
	for (i = 0; i < stack->count; i++) {
		n = fy_len(rc_turn_messages(stack->items[i]));
		take = off < n ? off : n;
		counts[i] = n - take;
		total += counts[i];
		off -= take;
	}
	if (!total)
		return fy_seq_empty;

	items = malloc(total * sizeof(*items));
	fyai_error_check(ctx, items, err, "could not collect the turn messages");
	off = skip;
	for (i = 0; i < stack->count; i++) {
		list = rc_turn_messages(stack->items[i]);
		n = fy_len(list);
		take = off < n ? off : n;
		for (j = take; j < n; j++)
			items[k++] = fy_get_at(list, j);
		off -= take;
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
			const size_t *counts, size_t from,
			size_t *indexp, size_t *skipp)
{
	fy_generic last;
	size_t i;

	for (i = 0; i < stack->count; i++) {
		if (from < counts[i]) {
			*indexp = i;
			*skipp = fy_len(rc_turn_messages(stack->items[i])) -
				 counts[i] + from;
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
			struct fyai_buf *out, size_t prefix,
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
		rv = fyai_buf_append(out, ", ");
		fyai_error_check(ctx, !rv, err, "could not keep the request text");
	}
	rv = fyai_buf_append(out, key_text);
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	rv = fyai_buf_append(out, ": ");
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	if (value_text) {
		rv = fyai_buf_append(out, value_text);
		fyai_error_check(ctx, !rv, err, "could not keep the request text");
		return 0;
	}
	text = emit_request_body(gb, value);
	fyai_error_check(ctx, text, err,
			 "could not serialize the model request");
	rc->emitted += strlen(text);
	rv = fyai_buf_append(out, text);
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
	struct fyai_buf head = { 0 }, tail = { 0 };
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
	if (rc->head && (word == rc->probe_word ||
			 (fy_is_valid(rc->probe_copy) &&
			  fy_equal(request, rc->probe_copy)))) {
		rc->probe_word = word;
		*headp = rc->head;
		*tailp = rc->tail;
		return 0;
	}
	if (!fy_is_mapping(request))
		return 1;

	rv = fyai_buf_append(&head, "{");
	fyai_error_check(ctx, !rv, err, "could not keep the request text");
	fy_foreach_key_value(key, value, request) {
		if (!found && fy_equal(value, probe)) {
			rv = rc_emit_pair(ctx, rc, &head, 1, key, "[", fy_invalid);
			if (rv)
				goto err;
			rv = fyai_buf_append(&tail, "]");
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
	rv = fyai_buf_append(&tail, "}");
	fyai_error_check(ctx, !rv, err, "could not keep the request text");

	if (!rc->probe_gb)
		rc->probe_gb = fy_generic_builder_create(NULL);
	else
		fy_generic_builder_reset(rc->probe_gb);
	fyai_error_check(ctx, rc->probe_gb, err,
			 "could not keep the request cache");
	rc->probe_copy = fy_gb_internalize(rc->probe_gb, request);
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
	struct fyai_buf added = { 0 }, tail = { 0 };
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
			skip = rc->skip;
			stop = fy_get(cur, "previous", fy_invalid);
			break;
		}
		/* An exchange that ended stored its last turns as other values. */
		if (!hit && rc->anchored && !rc->owner) {
			fyai_turn_foreach(cur, turn) {
				if (cur.v != rc->anchor)
					continue;
				stop = cur;
				hit = rc_check(ctx, rc, turn, stop);
				skip = rc->anchored_n;
				break;
			}
		}
		if (!hit)
			stop = fy_invalid;
	}
	if (hit)
		old_len = rc->stable_len;
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
	rc_position(&stack, counts, from, &index, &new_skip);
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
