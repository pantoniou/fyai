/*
 * fyai_todo_test.c - tests for the branch-stored todo list
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_todo.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(todo, normalize, todo_normalize)
FYAI_TEST_ENTRY(todo, normalize_rejects, todo_normalize_rejects)
FYAI_TEST_ENTRY(todo, panel, todo_panel)
FYAI_TEST_ENTRY(todo, format, todo_format)
FYAI_TEST_ENTRY(todo, all_done, todo_all_done)

static struct fy_generic_builder *todo_builder(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&cfg);
}

/* One item of the tool arguments. */
static fy_generic todo_item(struct fy_generic_builder *gb, const char *content,
			    const char *status, const char *priority)
{
	return fy_mapping(gb, "content", content, "status", status,
			  "priority", priority);
}

static fy_generic todo_args(struct fy_generic_builder *gb)
{
	return fy_mapping(gb, "todos", fy_sequence(gb,
		todo_item(gb, "First task", "in_progress", "high"),
		todo_item(gb, "Second task", "pending", "low")));
}

int todo_normalize(void)
{
	struct fy_generic_builder *gb = todo_builder();
	fy_generic todos, item;
	char why[1024];
	int rc = 0;

	todos = fyai_todo_normalize(gb, todo_args(gb), why, sizeof(why));
	if (fy_is_invalid(todos)) {
		fprintf(stderr, "normalize: %s\n", why);
		rc = 1;
		goto out;
	}
	FYAI_TCHECK(fy_len(todos) == 2);
	item = fy_get_at(todos, 0);
	FYAI_TCHECK(fy_equal(fy_get(item, "content", fy_invalid), "First task"));
	FYAI_TCHECK(fy_equal(fy_get(item, "status", fy_invalid), "in_progress"));
	FYAI_TCHECK(fy_equal(fy_get(item, "priority", fy_invalid), "high"));
out:
	fy_generic_builder_destroy(gb);
	return rc;
}

int todo_normalize_rejects(void)
{
	struct fy_generic_builder *gb = todo_builder();
	fy_generic args;
	char why[1024];
	int rc = 0;

	/* An empty list replaces nothing: clear through todo clear instead. */
	args = fy_mapping(gb, "todos", fy_seq_empty);
	if (fy_is_valid(fyai_todo_normalize(gb, args, why, sizeof(why)))) {
		fprintf(stderr, "empty list was kept\n");
		rc = 1;
		goto out;
	}
	/* An unknown state is not a state. */
	args = fy_mapping(gb, "todos", fy_sequence(gb,
		todo_item(gb, "Task", "doing", "high")));
	if (fy_is_valid(fyai_todo_normalize(gb, args, why, sizeof(why)))) {
		fprintf(stderr, "bogus status was kept\n");
		rc = 1;
		goto out;
	}
	FYAI_TCHECK(strstr(why, "doing") != NULL);
	/* An unknown priority is not a priority. */
	args = fy_mapping(gb, "todos", fy_sequence(gb,
		todo_item(gb, "Task", "pending", "urgent")));
	if (fy_is_valid(fyai_todo_normalize(gb, args, why, sizeof(why)))) {
		fprintf(stderr, "bogus priority was kept\n");
		rc = 1;
		goto out;
	}
	/* No content is no task. */
	args = fy_mapping(gb, "todos", fy_sequence(gb,
		todo_item(gb, "", "pending", "low")));
	if (fy_is_valid(fyai_todo_normalize(gb, args, why, sizeof(why)))) {
		fprintf(stderr, "empty content was kept\n");
		rc = 1;
		goto out;
	}
	/* A missing member is not the default of another. */
	args = fy_mapping(gb, "todos", fy_sequence(gb,
		fy_mapping(gb, "content", "Task", "status", "pending")));
	if (fy_is_valid(fyai_todo_normalize(gb, args, why, sizeof(why)))) {
		fprintf(stderr, "missing priority was kept\n");
		rc = 1;
		goto out;
	}
out:
	fy_generic_builder_destroy(gb);
	return rc;
}

/* The panel source of a stored list, without a context. */
static char *todo_panel_of(struct fy_generic_builder *gb, fy_generic todos)
{
	struct response_buffer out = {0};
	fy_generic item, g;
	const char *text, *status;
	const char *mark;
	bool first = true;

	fy_foreach(item, todos) {
		g = fy_get(item, "content", fy_invalid);
		text = fy_castp(&g, "");
		g = fy_get(item, "status", fy_invalid);
		status = fy_castp(&g, "");
		mark = !strcmp(status, "completed") ? "x" :
		       !strcmp(status, "in_progress") ? ">" :
		       !strcmp(status, "cancelled") ? "-" : " ";
		if (response_buffer_append(&out, first ? "- [" : "\n- [") ||
		    response_buffer_append(&out, mark) ||
		    response_buffer_append(&out, "] ") ||
		    response_buffer_append(&out, text) ||
		    response_buffer_append(&out, "") ||
		    0) {
			free(out.data);
			return NULL;
		}
		first = false;
	}
	(void)gb;
	if (response_buffer_append(&out, "\n")) {
		free(out.data);
		return NULL;
	}
	return out.data;
}

int todo_panel(void)
{
	struct fy_generic_builder *gb = todo_builder();
	fy_generic todos;
	char why[1024];
	char *panel;
	int rc = 0;

	todos = fyai_todo_normalize(gb, todo_args(gb), why, sizeof(why));
	FYAI_TCHECK(fy_is_valid(todos));
	panel = todo_panel_of(gb, todos);
	FYAI_TCHECK(panel != NULL);
	FYAI_TCHECK(strstr(panel, "- [>] First task") != NULL);
	FYAI_TCHECK(strstr(panel, "- [ ] Second task") != NULL);
	free(panel);
	fy_generic_builder_destroy(gb);
	return rc;
}

int todo_format(void)
{
	struct fy_generic_builder *gb = todo_builder();
	fy_generic todos;
	char why[1024];
	char *panel;
	int rc = 0;

	todos = fyai_todo_normalize(gb, fy_mapping(gb, "todos",
		fy_sequence(gb,
			todo_item(gb, "Done task", "completed", "medium"),
			todo_item(gb, "Dropped task", "cancelled", "low"))),
		why, sizeof(why));
	FYAI_TCHECK(fy_is_valid(todos));
	panel = todo_panel_of(gb, todos);
	FYAI_TCHECK(panel != NULL);
	FYAI_TCHECK(strstr(panel, "- [x] Done task") != NULL);
	FYAI_TCHECK(strstr(panel, "- [-] Dropped task") != NULL);
	free(panel);
	fy_generic_builder_destroy(gb);
	return rc;
}

int todo_all_done(void)
{
	struct fy_generic_builder *gb = todo_builder();
	fy_generic done, open;
	char why[1024];
	int rc = 0;

	done = fyai_todo_normalize(gb, fy_mapping(gb, "todos",
		fy_sequence(gb,
			todo_item(gb, "A", "completed", "high"),
			todo_item(gb, "B", "cancelled", "low"))),
		why, sizeof(why));
	FYAI_TCHECK(fy_is_valid(done));
	open = fyai_todo_normalize(gb, fy_mapping(gb, "todos",
		fy_sequence(gb,
			todo_item(gb, "A", "completed", "high"),
			todo_item(gb, "B", "pending", "low"))),
		why, sizeof(why));
	FYAI_TCHECK(fy_is_valid(open));
	/* A finished list clears; anything open stays. */
	FYAI_TCHECK(fyai_todo_all_done(done));
	FYAI_TCHECK(!fyai_todo_all_done(open));
	FYAI_TCHECK(!fyai_todo_all_done(fy_seq_empty));
	fy_generic_builder_destroy(gb);
	return rc;
}
