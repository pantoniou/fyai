/*
 * fyai_ask_test.c - tests for the questions of the ask_user tool
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_ask.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(ask, normalize_defaults, ask_normalize_defaults)
FYAI_TEST_ENTRY(ask, normalize_rejects, ask_normalize_rejects)
FYAI_TEST_ENTRY(ask, result_shape, ask_result_shape)
FYAI_TEST_ENTRY(ask, reply_from_line, ask_reply_from_line)
FYAI_TEST_ENTRY(ask, text_and_echo, ask_text_and_echo)

static struct fy_generic_builder *ask_builder(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&cfg);
}

/* An option of the tool arguments. */
static fy_generic ask_opt(struct fy_generic_builder *gb, const char *label)
{
	return fy_mapping(gb, "label", label, "description",
			  fy_sprintfa("The %s choice", label));
}

/* One question with the options "a", "b" and "c". */
static fy_generic ask_q(struct fy_generic_builder *gb, const char *header,
			bool multi)
{
	return fy_mapping(gb, "header", header, "question",
			  fy_sprintfa("Which %s?", header),
			  "multi_select", multi,
			  "options", fy_sequence(gb, ask_opt(gb, "a"),
						 ask_opt(gb, "b"),
						 ask_opt(gb, "c")));
}

int ask_normalize_defaults(void)
{
	struct fy_generic_builder *gb = ask_builder();
	fy_generic args, qs, q, o;
	char why[160];

	FYAI_TCHECK(gb != NULL);
	args = fy_mapping(gb, "questions",
			  fy_sequence(gb, ask_q(gb, "Scope", false),
				      ask_q(gb, "Style", true)));
	qs = fyai_ask_normalize(gb, args, why, sizeof(why));
	FYAI_TCHECK(fy_is_sequence(qs) && fy_len(qs) == 2);
	q = fy_get_at(qs, 0);
	FYAI_TCHECK(fy_equal(fy_get(q, "id", fy_invalid), "q1"));
	FYAI_TCHECK(fy_equal(fy_get(fy_get_at(qs, 1), "id", fy_invalid), "q2"));
	FYAI_TCHECK(!fy_get(q, "multi_select", true));
	FYAI_TCHECK(fy_get(fy_get_at(qs, 1), "multi_select", false));
	o = fy_get_at(fy_get(q, "options", fy_invalid), 1);
	/* The id of an option is its label, unless it names one. */
	FYAI_TCHECK(fy_equal(fy_get(o, "id", fy_invalid), "b"));
	FYAI_TCHECK(fy_equal(fy_get(o, "description", fy_invalid),
			     "The b choice"));
	fy_generic_builder_destroy(gb);
	printf("ok - the questions get ids and the options get theirs\n");
	return 0;
}

/* Whether @args are refused, with a cause that contains @needle. */
static bool ask_refused(struct fy_generic_builder *gb, fy_generic args,
			const char *needle)
{
	char why[160];
	fy_generic qs;

	qs = fyai_ask_normalize(gb, args, why, sizeof(why));
	return fy_is_invalid(qs) && strstr(why, needle);
}

int ask_normalize_rejects(void)
{
	struct fy_generic_builder *gb = ask_builder();
	fy_generic q, five, two, bad, dup, named;

	FYAI_TCHECK(gb != NULL);
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "question", "old form?"),
				"1 to 4 questions"));
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions", fy_sequence(gb)),
				"1 to 4 questions"));
	q = ask_q(gb, "Scope", false);
	five = fy_sequence(gb, q, q, q, q, q);
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions", five),
				"1 to 4 questions"));
	/* A header of thirteen characters is too long. */
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions",
		fy_sequence(gb, ask_q(gb, "ABCDEFGHIJKLM", false))), "header"));
	two = fy_mapping(gb, "header", "H", "question", "Q?", "options",
			 fy_sequence(gb, ask_opt(gb, "a")));
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions",
		fy_sequence(gb, two)), "2 to 4 options"));
	/* A preview belongs to a question of one choice. */
	bad = fy_mapping(gb, "header", "H", "question", "Q?",
			 "multi_select", true, "options", fy_sequence(gb,
			 fy_mapping(gb, "label", "a", "description", "d",
				    "preview", "# p"), ask_opt(gb, "b")));
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions",
		fy_sequence(gb, bad)), "preview"));
	dup = fy_mapping(gb, "header", "H", "question", "Q?", "options",
			 fy_sequence(gb, ask_opt(gb, "a"), ask_opt(gb, "a")));
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions",
		fy_sequence(gb, dup)), "repeats"));
	named = ask_q(gb, "Scope", false);
	named = fy_assoc(gb, named, "id", "same");
	FYAI_TCHECK(ask_refused(gb, fy_mapping(gb, "questions",
		fy_sequence(gb, named, named)), "repeats"));
	fy_generic_builder_destroy(gb);
	printf("ok - questions that cannot be asked are refused with a cause\n");
	return 0;
}

int ask_result_shape(void)
{
	struct fy_generic_builder *gb = ask_builder();
	struct fyai_ask_reply replies[2] = { { 0, NULL }, { 0, NULL } };
	fy_generic qs, res, a, sel;
	char why[160];

	FYAI_TCHECK(gb != NULL);
	qs = fyai_ask_normalize(gb, fy_mapping(gb, "questions", fy_sequence(gb,
		ask_q(gb, "Scope", false), ask_q(gb, "Style", true))),
		why, sizeof(why));
	FYAI_TCHECK(fy_is_sequence(qs));
	replies[0].selected = 1u << 2;
	replies[1].selected = (1u << 0) | (1u << 1);
	replies[1].other = strdup("own style");
	FYAI_TCHECK(replies[1].other != NULL);
	res = fyai_ask_result(gb, qs, replies, 2);
	FYAI_TCHECK(fy_equal(fy_get(res, "status", fy_invalid), "answered"));
	a = fy_get_at(fy_get(res, "answers", fy_invalid), 0);
	FYAI_TCHECK(fy_equal(fy_get(a, "id", fy_invalid), "q1"));
	sel = fy_get(a, "selected", fy_invalid);
	FYAI_TCHECK(fy_len(sel) == 1 && fy_equal(fy_get_at(sel, 0), "c"));
	FYAI_TCHECK(!fy_is_valid(fy_get(a, "other", fy_invalid)));
	a = fy_get_at(fy_get(res, "answers", fy_invalid), 1);
	sel = fy_get(a, "selected", fy_invalid);
	FYAI_TCHECK(fy_len(sel) == 2 && fy_equal(fy_get_at(sel, 1), "b"));
	FYAI_TCHECK(fy_equal(fy_get(a, "other", fy_invalid), "own style"));
	fyai_ask_replies_free(replies, 2);

	/* A NULL set of replies is the user declining. */
	res = fyai_ask_result(gb, qs, NULL, 0);
	FYAI_TCHECK(fy_equal(fy_get(res, "status", fy_invalid), "declined"));
	fy_generic_builder_destroy(gb);
	printf("ok - the result names the options chosen and the text typed\n");
	return 0;
}

/* Read @line for question @q; the reply is left in @r. */
static void ask_line(fy_generic q, const char *line, struct fyai_ask_reply *r)
{
	int rc;

	rc = fyai_ask_reply_from_line(q, line, r);
	FYAI_TCHECK(!rc);
}

int ask_reply_from_line(void)
{
	struct fy_generic_builder *gb = ask_builder();
	struct fyai_ask_reply r;
	fy_generic one, many;
	char why[160];
	fy_generic qs;

	FYAI_TCHECK(gb != NULL);
	qs = fyai_ask_normalize(gb, fy_mapping(gb, "questions", fy_sequence(gb,
		ask_q(gb, "Scope", false), ask_q(gb, "Style", true))),
		why, sizeof(why));
	FYAI_TCHECK(fy_is_sequence(qs));
	one = fy_get_at(qs, 0);
	many = fy_get_at(qs, 1);

	ask_line(one, " 2 ", &r);
	FYAI_TCHECK(r.selected == (1u << 1) && !r.other);
	/* A question of one choice takes one number. */
	ask_line(one, "1,2", &r);
	FYAI_TCHECK(!r.selected && r.other && !strcmp(r.other, "1,2"));
	fyai_ask_replies_free(&r, 1);
	ask_line(many, "1, 3", &r);
	FYAI_TCHECK(r.selected == ((1u << 0) | (1u << 2)) && !r.other);
	/* A number past the options is text. */
	ask_line(many, "4", &r);
	FYAI_TCHECK(!r.selected && r.other && !strcmp(r.other, "4"));
	fyai_ask_replies_free(&r, 1);
	ask_line(one, "something else", &r);
	FYAI_TCHECK(!r.selected && r.other &&
		    !strcmp(r.other, "something else"));
	FYAI_TCHECK(fyai_ask_reply_given(&r));
	fyai_ask_replies_free(&r, 1);
	ask_line(one, "  \t", &r);
	FYAI_TCHECK(!fyai_ask_reply_given(&r));
	fy_generic_builder_destroy(gb);
	printf("ok - a line is option numbers or the answer as typed\n");
	return 0;
}

int ask_text_and_echo(void)
{
	struct fy_generic_builder *gb = ask_builder();
	struct fyai_ask_reply r = { 1u << 1, NULL };
	fy_generic qs;
	char why[160];
	char *text, *echo;

	FYAI_TCHECK(gb != NULL);
	qs = fyai_ask_normalize(gb, fy_mapping(gb, "questions", fy_sequence(gb,
		ask_q(gb, "Style", true))), why, sizeof(why));
	FYAI_TCHECK(fy_is_sequence(qs));
	text = fyai_ask_text(fy_get_at(qs, 0));
	FYAI_TCHECK(text != NULL);
	FYAI_TCHECK(strstr(text, "? Which Style?"));
	FYAI_TCHECK(strstr(text, "2) b - The b choice"));
	FYAI_TCHECK(strstr(text, "separated by commas"));
	free(text);
	echo = fyai_ask_echo(qs, &r, 1);
	FYAI_TCHECK(echo != NULL);
	FYAI_TCHECK(!strcmp(echo, "? Which Style?\n> b\n"));
	free(echo);
	echo = fyai_ask_echo(qs, NULL, 0);
	FYAI_TCHECK(echo && strstr(echo, "(no answer)"));
	free(echo);
	fy_generic_builder_destroy(gb);
	printf("ok - the text and the echo name the options by their labels\n");
	return 0;
}
