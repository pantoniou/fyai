/*
 * fyai_flow_test.c - unit tests for the output separation manager
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_flow.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(flow, nothing_leads_the_first_unit, flow_first_unit)
FYAI_TEST_ENTRY(flow, a_tool_group_is_fenced_once, flow_tool_group)
FYAI_TEST_ENTRY(flow, the_fence_is_configured, flow_fence_configured)
FYAI_TEST_ENTRY(flow, a_turn_break_is_a_row, flow_turn_separator)
FYAI_TEST_ENTRY(flow, reasoning_closes_into_the_answer, flow_section_separator)
FYAI_TEST_ENTRY(flow, every_transition_is_bounded, flow_all_transitions)

static int failures;

static void expect_rows(const char *what, unsigned got, unsigned want)
{
	if (got == want)
		return;
	fprintf(stderr, "FAIL %s\n  got:  %u rows\n  want: %u rows\n",
		what, got, want);
	failures++;
}

static void expect_true(const char *what, bool cond)
{
	if (cond)
		return;
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

/* A configuration with only the fields the manager reads. */
static void flow_cfg(struct fyai_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->turn_separator = DEFAULT_TURN_SEPARATOR;
	cfg->tool_separator = DEFAULT_TOOL_SEPARATOR;
	cfg->section_separator = DEFAULT_SECTION_SEPARATOR;
	cfg->tool_group_fence = DEFAULT_TOOL_GROUP_FENCE;
	cfg->user_card_fence = DEFAULT_USER_CARD_FENCE;
}

/* Separation between @prev and @next on a fresh medium. */
static struct fyai_flow_sep sep_between(struct fyai_cfg *cfg,
					enum fyai_flow_unit prev,
					enum fyai_flow_unit next)
{
	struct fyai_flow f;

	fyai_flow_reset(&f, cfg);
	if (prev != FYAI_FLOW_NONE)
		fyai_flow_emitted(&f, prev, true);
	return fyai_flow_before(&f, next);
}

int flow_first_unit(void)
{
	struct fyai_flow_sep sep;
	struct fyai_cfg cfg;

	failures = 0;
	flow_cfg(&cfg);
	/* Nothing is presented yet, thus nothing goes before the first unit. */
	sep = sep_between(&cfg, FYAI_FLOW_NONE, FYAI_FLOW_PROSE);
	expect_rows("prose opens a medium", fyai_flow_sep_rows(&sep), 0);
	sep = sep_between(&cfg, FYAI_FLOW_NONE, FYAI_FLOW_TOOL_HEAD);
	expect_rows("a tool opens a medium", fyai_flow_sep_rows(&sep), 0);
	sep = sep_between(&cfg, FYAI_FLOW_NONE, FYAI_FLOW_USER_CARD);
	expect_rows("a card opens a medium", fyai_flow_sep_rows(&sep), 0);
	return failures;
}

int flow_tool_group(void)
{
	struct fyai_flow_sep sep;
	struct fyai_cfg cfg;

	failures = 0;
	flow_cfg(&cfg);
	/* The group is fenced from the prose around it. */
	sep = sep_between(&cfg, FYAI_FLOW_PROSE, FYAI_FLOW_TOOL_HEAD);
	expect_rows("prose into a tool", fyai_flow_sep_rows(&sep), 1);
	sep = sep_between(&cfg, FYAI_FLOW_TOOL_RESULT, FYAI_FLOW_PROSE);
	expect_rows("a tool into prose", fyai_flow_sep_rows(&sep), 1);
	/* It is never fenced against itself. */
	sep = sep_between(&cfg, FYAI_FLOW_TOOL_HEAD, FYAI_FLOW_TOOL_BODY);
	expect_rows("a head into its body", fyai_flow_sep_rows(&sep), 0);
	sep = sep_between(&cfg, FYAI_FLOW_TOOL_BODY, FYAI_FLOW_TOOL_RESULT);
	expect_rows("a body into its result", fyai_flow_sep_rows(&sep), 0);
	/* A title row opens a call. One call is fenced from the next. */
	sep = sep_between(&cfg, FYAI_FLOW_TOOL_RESULT, FYAI_FLOW_TOOL_HEAD);
	expect_rows("one call after another", fyai_flow_sep_rows(&sep), 1);
	sep = sep_between(&cfg, FYAI_FLOW_TOOL_HEAD, FYAI_FLOW_TOOL_HEAD);
	expect_rows("a call with no output", fyai_flow_sep_rows(&sep), 1);
	return failures;
}

int flow_fence_configured(void)
{
	struct fyai_flow_sep sep;
	struct fyai_cfg cfg;

	failures = 0;
	flow_cfg(&cfg);
	cfg.tool_group_fence = 0;
	sep = sep_between(&cfg, FYAI_FLOW_PROSE, FYAI_FLOW_TOOL_HEAD);
	expect_rows("the fence is off", fyai_flow_sep_rows(&sep), 0);
	cfg.tool_group_fence = 2;
	sep = sep_between(&cfg, FYAI_FLOW_PROSE, FYAI_FLOW_TOOL_HEAD);
	expect_rows("the fence is two rows", fyai_flow_sep_rows(&sep), 2);
	cfg.user_card_fence = 0;
	sep = sep_between(&cfg, FYAI_FLOW_USER_CARD, FYAI_FLOW_PROSE);
	expect_rows("the answer is under the card",
		    fyai_flow_sep_rows(&sep), 0);
	return failures;
}

int flow_turn_separator(void)
{
	struct fyai_flow_sep sep;
	struct fyai_cfg cfg;

	failures = 0;
	flow_cfg(&cfg);
	/*
	 * A turn break is a row. The transcript view draws the configured
	 * rule; a live session draws no rule.
	 */
	sep = sep_between(&cfg, FYAI_FLOW_PROSE, FYAI_FLOW_USER_CARD);
	expect_true("the manager draws no rule", !sep.markdown);
	expect_rows("a turn break takes one row",
		    fyai_flow_sep_rows(&sep), 1);
	sep = sep_between(&cfg, FYAI_FLOW_PROSE, FYAI_FLOW_SYSTEM);
	expect_true("a system turn draws no rule either", !sep.markdown);
	return failures;
}

int flow_section_separator(void)
{
	struct fyai_flow_sep sep;
	struct fyai_cfg cfg;

	failures = 0;
	flow_cfg(&cfg);
	sep = sep_between(&cfg, FYAI_FLOW_REASONING, FYAI_FLOW_PROSE);
	expect_rows("reasoning closes with a blank row",
		    fyai_flow_sep_rows(&sep), 1);
	cfg.section_separator = "***";
	sep = sep_between(&cfg, FYAI_FLOW_REASONING, FYAI_FLOW_PROSE);
	expect_true("the configured break is carried",
		    sep.markdown && !strcmp(sep.markdown, "***"));
	/* Reasoning continues into itself without a break. */
	sep = sep_between(&cfg, FYAI_FLOW_REASONING, FYAI_FLOW_REASONING);
	expect_rows("reasoning continues", fyai_flow_sep_rows(&sep), 0);
	return failures;
}

int flow_all_transitions(void)
{
	struct fyai_flow_sep sep;
	struct fyai_cfg cfg;
	int prev;
	int next;

	failures = 0;
	flow_cfg(&cfg);
	/*
	 * Every pair answers, and no pair asks for a run of rows. An
	 * unbounded entry pushes the exchange off the screen.
	 */
	for (prev = FYAI_FLOW_NONE; prev <= FYAI_FLOW_TURN_BREAK; prev++) {
		for (next = FYAI_FLOW_NONE; next <= FYAI_FLOW_TURN_BREAK;
		     next++) {
			sep = sep_between(&cfg, (enum fyai_flow_unit)prev,
					  (enum fyai_flow_unit)next);
			expect_true("a transition is bounded",
				    fyai_flow_sep_rows(&sep) <= 2);
		}
	}
	/* Streamed prose arrives in chunks. Do not break it up. */
	sep = sep_between(&cfg, FYAI_FLOW_PROSE, FYAI_FLOW_PROSE);
	expect_rows("prose continues", fyai_flow_sep_rows(&sep), 0);
	return failures;
}
