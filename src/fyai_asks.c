/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_ask.h"
#include "fyai_asks.h"
#include "fyai_ui.h"
#include "fyai_wait.h"
#include "utils.h"

/*
 * A question that waits. It keeps the questions in a builder of its own: the
 * turn that asked them ends before the user answers.
 */
struct fyai_ask_pending {
	struct fyai_ask_pending *next;
	struct fyai_ctx *ctx;
	char *name;
	struct fy_generic_builder *gb;
	fy_generic questions;
};

static bool asks_name_valid(const char *name)
{
	size_t i;

	if (!name || !*name || strlen(name) > FYAI_ASKS_NAME_MAX)
		return false;
	for (i = 0; name[i]; i++)
		if (!isalnum((unsigned char)name[i]) && !strchr("-/_", name[i]))
			return false;
	return true;
}

static struct fyai_ask_pending *asks_find(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_ask_pending *p;

	for (p = ctx->asks; p; p = p->next)
		if (!strcmp(p->name, name))
			return p;
	return NULL;
}

static void asks_free(struct fyai_ask_pending *p)
{
	if (!p)
		return;
	fy_generic_builder_destroy(p->gb);
	free(p->name);
	free(p);
}

/* Take @p off the list of the context. */
static void asks_unlink(struct fyai_ctx *ctx, struct fyai_ask_pending *p)
{
	struct fyai_ask_pending **pp;

	for (pp = &ctx->asks; *pp; pp = &(*pp)->next) {
		if (*pp != p)
			continue;
		*pp = p->next;
		return;
	}
}

/* Whether @p is still a question of the context. */
static bool asks_live(struct fyai_ctx *ctx, const struct fyai_ask_pending *p)
{
	struct fyai_ask_pending *q;

	for (q = ctx->asks; q; q = q->next)
		if (q == p)
			return true;
	return false;
}

/*
 * What the model is told when the question ends: how, and the answers. This
 * function asks the model nothing; the owner of the loop starts the turn.
 */
static char *asks_report(struct fyai_ask_pending *p,
			 const struct fyai_ask_reply *replies, size_t n)
{
	fy_generic result;
	const char *json;

	if (!replies)
		return strdup(fy_sprintfa("[question '%s' declined]\n%s", p->name,
					  "the user did not provide an answer"));
	result = fyai_ask_result(p->gb, p->questions, replies, n);
	json = emit_json_string(p->gb, result);
	if (!json)
		return NULL;
	return strdup(fy_sprintfa("[question '%s' answered]\n%s", p->name, json));
}

static void asks_done(void *user, const struct fyai_ask_reply *replies,
		      size_t n)
{
	struct fyai_ask_pending *p = user;
	struct fyai_ctx *ctx = p->ctx;
	char *text;

	if (!asks_live(ctx, p))
		return;
	text = asks_report(p, replies, n);
	asks_unlink(ctx, p);
	if (!text) {
		fyai_error(ctx, "ask_user: cannot report the answers of '%s'",
			   p->name);
		text = strdup(fy_sprintfa("[question '%s' declined]", p->name));
	}
	asks_free(p);
	if (!text)
		return;
	(void)fyai_event_inject(ctx, text);
	fyai_waiters_kick(ctx);
}

bool fyai_asks_background(fy_generic args)
{
	fy_generic gname = fy_get(args, "name", fy_invalid);

	return fy_get(args, "background", false) ||
	       *fy_castp(&gname, "");
}

/* Write a name for a question that was given none, unused by any open one. */
static void asks_default_name(struct fyai_ctx *ctx, char *buf, size_t size)
{
	do
		snprintf(buf, size, "ask-%u", ++ctx->asks_seq);
	while (asks_find(ctx, buf));
}

char *fyai_asks_start(struct fyai_ctx *ctx, const char *name,
		      fy_generic questions)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fyai_ask_pending *p;
	char made[FYAI_ASKS_NAME_MAX + 1];
	int rc;

	if (!name || !*name) {
		asks_default_name(ctx, made, sizeof(made));
		name = made;
	}
	if (!asks_name_valid(name))
		return strdup(fy_sprintfa("tool error: '%s' is not a usable "
					  "question name; use letters, digits, "
					  "'-' or '_'", name));
	if (asks_find(ctx, name))
		return strdup(fy_sprintfa("tool error: a question named '%s' "
					  "is already open; use another name",
					  name));
	p = calloc(1, sizeof(*p));
	fyai_error_check(ctx, p, err_out, "ask_user: cannot keep question '%s'",
			 name);
	p->ctx = ctx;
	p->name = strdup(name);
	fyai_error_check(ctx, p->name, err_free,
			 "ask_user: cannot keep the name of question '%s'", name);
	p->gb = fy_generic_builder_create(&cfg);
	fyai_error_check(ctx, p->gb, err_free,
			 "ask_user: cannot create the builder of question '%s'",
			 name);
	p->questions = fy_gb_internalize(p->gb, questions);
	fyai_error_check(ctx, fy_is_valid(p->questions), err_free,
			 "ask_user: cannot keep the questions of '%s'", name);
	p->next = ctx->asks;
	ctx->asks = p;
	rc = fyai_ui_ask(ctx, p->questions, NULL, asks_done, p);
	if (rc) {
		asks_unlink(ctx, p);
		asks_free(p);
		return strdup("tool error: the questions could not be asked");
	}
	return strdup(fy_sprintfa("[question '%s' asked]\nIt does not hold "
				  "your turn. Keep working; you are told when "
				  "the user has answered. Use wait to hold a "
				  "call for it.", name));

err_free:
	asks_free(p);
err_out:
	return NULL;
}

bool fyai_asks_exists(struct fyai_ctx *ctx, const char *name)
{
	return asks_find(ctx, name) != NULL;
}

bool fyai_asks_cancel(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_ask_pending *p = asks_find(ctx, name);

	if (!p)
		return false;
	fyai_ui_ask_withdraw(ctx, p);
	asks_unlink(ctx, p);
	asks_free(p);
	fyai_waiters_kick(ctx);
	return true;
}

fy_generic fyai_asks_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	struct fyai_ask_pending *p;
	fy_generic rows, headers, q;

	rows = fy_sequence(gb);
	for (p = ctx->asks; p; p = p->next) {
		headers = fy_sequence(gb);
		fy_foreach(q, p->questions)
			headers = fy_append(gb, headers,
					    fy_get(q, "header", fy_invalid));
		rows = fy_append(gb, rows, fy_mapping(gb, "name", p->name,
						      "headers", headers));
	}
	return rows;
}

void fyai_asks_release(struct fyai_ctx *ctx)
{
	struct fyai_ask_pending *p, *next;

	if (!ctx)
		return;
	for (p = ctx->asks; p; p = next) {
		next = p->next;
		fyai_ui_ask_withdraw(ctx, p);
		asks_free(p);
	}
	ctx->asks = NULL;
}

/* The questions of the parent stay with the parent's user interface. */
void fyai_asks_abandon(struct fyai_ctx *ctx)
{
	struct fyai_ask_pending *p, *next;

	if (!ctx)
		return;
	for (p = ctx->asks; p; p = next) {
		next = p->next;
		asks_free(p);
	}
	ctx->asks = NULL;
}
