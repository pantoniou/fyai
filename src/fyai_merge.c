/*
 * fyai_merge.c - joining two branches
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_merge.h"
#include "fyai_storage.h"
#include "fyai_turn.h"

#define FYAI_MERGE_REFLOG_MAX 4096

struct fyai_turn_time {
	fy_generic_value turn;
	fy_generic_value when;
};

static int turn_time_cmp(const void *a, const void *b)
{
	const struct fyai_turn_time *x = a, *y = b;

	return x->turn < y->turn ? -1 : x->turn > y->turn ? 1 : 0;
}

int fyai_turn_times_collect(struct fyai_ctx *ctx, fy_generic entry,
			    struct fyai_turn_times *tt)
{
	struct fyai_branch b;
	struct fyai_turn_time *items, *grown;
	size_t i, j, n, cap;
	unsigned int step;

	tt->items = NULL;
	tt->count = 0;

	cap = 64;
	items = malloc(cap * sizeof(*items));
	fyai_error_check(ctx, items, err_out,
			 "out of memory timing the turns");

	/*
	 * Walk oldest-last. Each entry says what the head was and when, so the
	 * *last* writer for a given turn - the oldest entry naming it - is when
	 * that turn first appeared.
	 */
	n = 0;
	for (step = 0; step < FYAI_MERGE_REFLOG_MAX; step++) {
		if (!fyai_branch_entry_contained(ctx->durable_allocator,
						 entry, 1))
			break;
		if (!fyai_branch_decode(entry, &b))
			break;
		if (fy_generic_is_valid(b.head)) {
			if (n == cap) {
				cap *= 2;
				grown = realloc(items, cap * sizeof(*items));
				fyai_error_check(ctx, grown, err_free,
						 "out of memory timing the turns");
				items = grown;
			}
			items[n].turn = b.head.v;
			items[n].when = fy_get(b.entry, "created", 0ULL);
			n++;
		}
		entry = b.prev;
	}

	/* Keep the oldest time for each turn, then sort for lookup. */
	if (n > 1) {
		for (i = 0; i < n; i++) {
			for (j = n; j-- > i + 1; ) {
				if (items[j].turn == items[i].turn) {
					if (items[j].when < items[i].when)
						items[i].when = items[j].when;
					items[j] = items[--n];
					j = n;
				}
			}
		}
		qsort(items, n, sizeof(*items), turn_time_cmp);
	}

	tt->items = items;
	tt->count = n;
	return 0;

err_free:
	free(items);
err_out:
	return -1;
}

void fyai_turn_times_cleanup(struct fyai_turn_times *tt)
{
	free(tt->items);
	tt->items = NULL;
	tt->count = 0;
}

uint64_t fyai_turn_time_lookup(const struct fyai_turn_times *tt,
			       fy_generic turn)
{
	struct fyai_turn_time key, *hit;

	if (!tt->count)
		return 0;
	key.turn = turn.v;
	key.when = 0;
	hit = bsearch(&key, tt->items, tt->count, sizeof(*tt->items),
		      turn_time_cmp);
	return hit ? hit->when : 0;
}

static int generic_value_cmp(const void *a, const void *b)
{
	fy_generic_value x, y;

	x = *(const fy_generic_value *)a;
	y = *(const fy_generic_value *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

fy_generic fyai_merge_base(struct fyai_ctx *ctx, fy_generic a, fy_generic b)
{
	struct fyai_turn_stack sb;
	fy_generic_value *seen, v;
	fy_generic cur;
	int rc;
	size_t i;

	rc = fyai_turn_stack_init(&sb, b, fy_invalid);
	fyai_error_check(ctx, !rc, err_out,
			 "out of memory finding the common turn");
	if (!sb.count)
		goto err_cleanup;

	seen = malloc(sb.count * sizeof(*seen));
	fyai_error_check(ctx, seen, err_cleanup,
			 "out of memory finding the common turn");
	for (i = 0; i < sb.count; i++)
		seen[i] = sb.items[i].v;
	qsort(seen, sb.count, sizeof(*seen), generic_value_cmp);

	/* Find the newest turn shared by both branches. */
	cur = fy_invalid;
	fyai_turn_foreach(cur, a) {
		v = cur.v;
		if (bsearch(&v, seen, sb.count, sizeof(v), generic_value_cmp))
			break;
	}
	if (fy_generic_is_valid(cur) && fy_generic_is_null_type(cur))
		cur = fy_invalid;

	free(seen);
	fyai_turn_stack_cleanup(&sb);
	return cur;

err_cleanup:
	fyai_turn_stack_cleanup(&sb);
err_out:
	return fy_invalid;
}

struct fyai_exchange {
	size_t start;		/* index into the owning stack */
	size_t end;		/* one past the last turn */
	uint64_t when;		/* zero when the ref log no longer knows */
	int side;		/* 0 target, 1 source: a stable tie-break */
	size_t seq;		/* original order within that side */
};

/* Split @stack into user-led exchanges. */
static size_t exchanges_split(const struct fyai_turn_stack *stack,
			      const struct fyai_turn_times *tt, int side,
			      struct fyai_exchange *out)
{
	size_t i, j, n;

	n = 0;
	for (i = 0; i < stack->count; i++) {
		if (!fyai_turn_has_user_message(stack->items[i]) && n)
			continue;
		if (n)
			out[n - 1].end = i;
		out[n].start = i;
		out[n].end = stack->count;
		out[n].side = side;
		out[n].seq = n;
		out[n].when = 0;
		n++;
	}

	/* Use the time of the last timed turn in each exchange. */
	for (i = 0; i < n; i++) {
		for (j = out[i].end; j-- > out[i].start; ) {
			out[i].when = fyai_turn_time_lookup(tt,
							    stack->items[j]);
			if (out[i].when)
				break;
		}
	}
	return n;
}

static int exchange_cmp(const void *a, const void *b)
{
	const struct fyai_exchange *x = a, *y = b;
	/* An untimed exchange predates the retained ref log. */
	if (!x->when != !y->when)
		return x->when ? 1 : -1;
	if (x->when != y->when)
		return x->when < y->when ? -1 : 1;
	if (x->side != y->side)
		return x->side - y->side;
	return x->seq < y->seq ? -1 : x->seq > y->seq ? 1 : 0;
}

/* Re-chain @turn onto @prev without changing its other fields. */
static fy_generic turn_rechain(struct fyai_ctx *ctx, fy_generic turn,
			       fy_generic prev)
{
	return fy_assoc(ctx->gb, turn, "previous",
			fy_generic_is_valid(prev) ? prev : fy_null);
}

/* Replay [@start, @end) and keep only the first system turn. */
static int replay(struct fyai_ctx *ctx, fy_generic *head,
		  const struct fyai_turn_stack *stack, size_t start,
		  size_t end, bool *have_system)
{
	size_t i;

	for (i = start; i < end; i++) {
		if (fyai_turn_is_system_only(stack->items[i])) {
			if (*have_system)
				continue;
			*have_system = true;
		}
		*head = turn_rechain(ctx, stack->items[i], *head);
		if (!fy_generic_is_valid(*head))
			return -1;
	}
	return 0;
}

int fyai_branch_join(struct fyai_ctx *ctx, const char *source,
		     enum fyai_join_mode mode, bool allow_unrelated)
{
	struct fyai_turn_times ours_t, theirs_t;
	struct fyai_turn_stack ours, theirs;
	struct fyai_exchange *ex;
	struct fyai_branch sb, ob;
	fy_generic base, head;
	const struct fyai_turn_stack *stack;
	size_t n, i, total;
	bool found, unrelated, have_system;
	int rc, ret;

	ret = -1;
	ex = NULL;
	memset(&ours, 0, sizeof(ours));
	memset(&theirs, 0, sizeof(theirs));
	memset(&ours_t, 0, sizeof(ours_t));
	memset(&theirs_t, 0, sizeof(theirs_t));

	fyai_error_check(ctx, strcmp(source, fyai_ctx_branch(ctx)), out,
			 "cannot join '%s' with itself", source);
	found = fyai_branch_lookup(ctx->arena_branches, source, &sb);
	fyai_error_check(ctx, found, out, "no such branch '%s'", source);
	fyai_error_check(ctx, fy_generic_is_valid(sb.head), out,
			 "branch '%s' has no turns to join", source);
	fyai_branch_lookup(ctx->arena_branches, fyai_ctx_branch(ctx), &ob);

	base = fyai_merge_base(ctx, ctx->last_message, sb.head);
	unrelated = fy_generic_is_invalid(base);
	fyai_error_check(ctx, !unrelated || allow_unrelated, out,
			 "'%s' and '%s' share no turn; pass "
			 "--allow-unrelated to join them anyway",
			 fyai_ctx_branch(ctx), source);

	rc = fyai_turn_stack_init(&ours, ctx->last_message, base);
	fyai_error_check(ctx, !rc, out, "out of memory reading our turns");
	rc = fyai_turn_stack_init(&theirs, sb.head, base);
	fyai_error_check(ctx, !rc, out, "out of memory reading their turns");
	fyai_error_check(ctx, theirs.count, out,
			 "'%s' has nothing that '%s' does not already have",
			 source, fyai_ctx_branch(ctx));

	have_system = fy_generic_is_valid(base);
	head = base;

	if (mode == FYAI_JOIN_REBASE) {
		rc = replay(ctx, &head, &theirs, 0, theirs.count,
			    &have_system);
		fyai_error_check(ctx, !rc, out,
				 "could not replay their turns");
		rc = replay(ctx, &head, &ours, 0, ours.count, &have_system);
		fyai_error_check(ctx, !rc, out, "could not replay our turns");
	} else {
		rc = fyai_turn_times_collect(ctx, ob.entry, &ours_t);
		fyai_error_check(ctx, !rc, out, "could not time our turns");
		rc = fyai_turn_times_collect(ctx, sb.entry, &theirs_t);
		fyai_error_check(ctx, !rc, out, "could not time their turns");

		total = ours.count + theirs.count;
		ex = malloc((total + 2) * sizeof(*ex));
		fyai_error_check(ctx, ex, out,
				 "out of memory ordering the exchanges");
		n = exchanges_split(&ours, &ours_t, 0, ex);
		n += exchanges_split(&theirs, &theirs_t, 1, ex + n);
		qsort(ex, n, sizeof(*ex), exchange_cmp);

		for (i = 0; i < n; i++) {
			stack = ex[i].side ? &theirs : &ours;
			rc = replay(ctx, &head, stack, ex[i].start,
				    ex[i].end, &have_system);
			fyai_error_check(ctx, !rc, out,
					 "could not replay the turns");
		}
	}

	head = fy_gb_internalize(ctx->gb, head);
	fyai_error_check(ctx, fy_generic_is_valid(head), out,
			 "could not store the joined conversation");

	ctx->last_message = head;
	fyai_branch_op_set(ctx, mode == FYAI_JOIN_REBASE ?
			   FYAI_BRANCH_OP_REBASE : FYAI_BRANCH_OP_MERGE,
			   source);
	rc = fyai_publish_state(ctx);
	fyai_error_check(ctx, !rc, out,
			 "could not publish the joined conversation");

	printf("%s %s into %s (%zu turn%s added%s); previous head is %s@{1}\n",
	       mode == FYAI_JOIN_REBASE ? "rebased" : "merged", source,
	       fyai_ctx_branch(ctx), theirs.count,
	       theirs.count == 1 ? "" : "s",
	       unrelated ? ", unrelated" : "", fyai_ctx_branch(ctx));
	ret = 0;
out:
	free(ex);
	fyai_turn_times_cleanup(&ours_t);
	fyai_turn_times_cleanup(&theirs_t);
	fyai_turn_stack_cleanup(&ours);
	fyai_turn_stack_cleanup(&theirs);
	return ret;
}

/* Replay unpublished turns after a concurrent update to the same branch. */
int fyai_join_onto_head(struct fyai_ctx *ctx, fy_generic theirs,
			fy_generic *outp)
{
	struct fyai_turn_stack ours;
	fy_generic base, head;
	bool have_system;
	int ret;

	*outp = fy_invalid;
	base = fyai_merge_base(ctx, ctx->last_message, theirs);
	ret = fyai_turn_stack_init(&ours, ctx->last_message, base);
	fyai_error_check(ctx, !ret, err_out,
			 "out of memory replaying this turn");

	ret = -1;
	head = theirs;
	have_system = true;	/* theirs already opens with one */
	ret = replay(ctx, &head, &ours, 0, ours.count, &have_system);
	fyai_error_check(ctx, !ret, out, "could not replay this turn");
	head = fy_gb_internalize(ctx->gb, head);
	fyai_error_check(ctx, fy_generic_is_valid(head), out,
			 "could not store the replayed turn");
	*outp = head;
	ret = 0;
out:
	fyai_turn_stack_cleanup(&ours);
	return ret;

err_out:
	return -1;
}
