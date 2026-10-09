/*
 * fyai_todo.c - branch-stored todo list of the todo_write tool
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_markdown.h"
#include "fyai_storage.h"
#include "fyai_todo.h"
#include "utils.h"

/* Whether @s names a usable todo state. */
static bool todo_status_valid(const char *s)
{
	return s && (!strcmp(s, FYAI_TODO_PENDING) ||
		     !strcmp(s, FYAI_TODO_IN_PROGRESS) ||
		     !strcmp(s, FYAI_TODO_COMPLETED) ||
		     !strcmp(s, FYAI_TODO_CANCELLED));
}

/* Whether @s names a usable todo priority. */
static bool todo_priority_valid(const char *s)
{
	return s && (!strcmp(s, FYAI_TODO_HIGH) ||
		     !strcmp(s, FYAI_TODO_MEDIUM) ||
		     !strcmp(s, FYAI_TODO_LOW));
}

/* Longest value quoted into a cause. Longer values are cut with an ellipsis. */
#define TODO_WHY_VALUE_MAX	128

/* Whether every item of @todos is finished: completed or cancelled. */
bool fyai_todo_all_done(fy_generic todos)
{
	fy_generic item, status;
	const char *state;

	if (!fy_is_sequence(todos) || fy_empty(todos))
		return false;
	fy_foreach(item, todos) {
		status = fy_get(item, "status", fy_invalid);
		state = fy_castp(&status, "");
		if (strcmp(state, FYAI_TODO_COMPLETED) &&
		    strcmp(state, FYAI_TODO_CANCELLED))
			return false;
	}
	return true;
}

fy_generic fyai_todo_normalize(struct fy_generic_builder *gb, fy_generic args,
			       char *why, size_t why_size)
{
	fy_generic todos, item, out, content, status, priority;
	const char *text, *state, *prio;
	size_t n;

	todos = fy_get(args, "todos", fy_invalid);
	if (!fy_is_sequence(todos) || fy_empty(todos)) {
		snprintf(why, why_size, "todo_write needs a non-empty todos list");
		return fy_invalid;
	}
	n = 0;
	out = fy_seq_empty;
	fy_foreach(item, todos) {
		n++;
		if (n > FYAI_TODO_ITEMS_MAX) {
			snprintf(why, why_size, "todo_write holds at most %d items",
				 FYAI_TODO_ITEMS_MAX);
			return fy_invalid;
		}
		if (!fy_is_mapping(item)) {
			snprintf(why, why_size, "todo %zu is not a mapping", n);
			return fy_invalid;
		}
		content = fy_get(item, "content", fy_invalid);
		status = fy_get(item, "status", fy_invalid);
		priority = fy_get(item, "priority", fy_invalid);
		text = fy_castp(&content, "");
		state = fy_castp(&status, "");
		prio = fy_castp(&priority, "");
		if (fy_str_empty(text)) {
			snprintf(why, why_size, "todo %zu needs a content", n);
			return fy_invalid;
		}
		if (strlen(text) > FYAI_TODO_CONTENT_MAX) {
			snprintf(why, why_size, "todo %zu content is too long", n);
			return fy_invalid;
		}
		if (!todo_status_valid(state)) {
			snprintf(why, why_size, "todo %zu status '%.*s%s' is not "
				 "pending, in_progress, completed or cancelled",
				 n, TODO_WHY_VALUE_MAX, state,
				 strlen(state) > TODO_WHY_VALUE_MAX ? "..." : "");
			return fy_invalid;
		}
		if (!todo_priority_valid(prio)) {
			snprintf(why, why_size, "todo %zu priority '%.*s%s' is not "
				 "high, medium or low", n, TODO_WHY_VALUE_MAX,
				 prio, strlen(prio) > TODO_WHY_VALUE_MAX ?
				 "..." : "");
			return fy_invalid;
		}
		out = fy_append(gb, out, fy_mapping(gb,
			"content", content, "status", status,
			"priority", priority));
		if (fy_is_invalid(out)) {
			snprintf(why, why_size, "todo_write: out of memory");
			return fy_invalid;
		}
	}
	return out;
}

/* The stored list of this branch, whatever builder holds it. */
static fy_generic todo_stored_generic(struct fyai_ctx *ctx)
{
	struct fyai_branch b;

	if (!ctx || fy_is_invalid(ctx->branch_prev))
		return fy_invalid;
	if (!fyai_branch_decode(ctx->branch_prev, &b))
		return fy_invalid;
	if (!fy_is_sequence(b.todos) || fy_empty(b.todos))
		return fy_invalid;
	return b.todos;
}

fy_generic fyai_todo_stored(struct fyai_ctx *ctx)
{
	fy_generic stored;

	stored = todo_stored_generic(ctx);
	if (fy_is_invalid(stored))
		return fy_invalid;
	return fy_gb_internalize(fyai_ctx_transient_gb(ctx), stored);
}

/*
 * Publish @todos, kept in the durable builder, as the todo list of this
 * branch. The list is branch state, as the configuration is: the publish
 * carries it to the store of the branch.
 */
static int todo_publish(struct fyai_ctx *ctx, fy_generic todos)
{
	struct fyai_branch prev;
	fy_generic store, item, rebuilt, content, status, priority;
	const char *text, *state, *prio;

	if (!ctx->durable_gb) {
		fyai_error(ctx, "todo_write: no arena; run fyai init");
		return -1;
	}
	/*
	 * The normalized list lives in the transient builder, which the arena
	 * cannot reference. Rebuild it in the durable builder: short strings
	 * may live in the generic word, so copy each member across.
	 */
	rebuilt = fy_seq_empty;
	fy_foreach(item, todos) {
		content = fy_get(item, "content", fy_invalid);
		status = fy_get(item, "status", fy_invalid);
		priority = fy_get(item, "priority", fy_invalid);
		text = fy_castp(&content, "");
		state = fy_castp(&status, "");
		prio = fy_castp(&priority, "");
		rebuilt = fy_append(ctx->gb, rebuilt,
			fy_mapping(ctx->gb,
				"content", fy_value(ctx->gb, text),
				"status", fy_value(ctx->gb, state),
				"priority", fy_value(ctx->gb, prio)));
		fyai_error_check(ctx, fy_is_valid(rebuilt), err_out,
				 "todo_write: cannot keep the todo list");
	}
	todos = fy_gb_internalize(ctx->gb, rebuilt);
	fyai_error_check(ctx, fy_is_valid(todos), err_out,
			 "todo_write: cannot keep the todo list");
	fyai_branch_decode(ctx->branch_prev, &prev);
	/* The commit path below carries only known members: stage the list
	 * there so the publish does not drop it. */
	prev.todos = todos;
	store = fyai_branch_store_build(ctx->gb, &prev);
	if (fy_is_invalid(todos))
		store = fy_disassoc(ctx->gb, store, "todos");
	fyai_error_check(ctx, fy_is_valid(store), err_out,
			 "todo_write: cannot build the branch store");
	ctx->branch_store = store;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_TODO, NULL);
	if (fyai_publish_state(ctx))
		return -1;
	return 0;

err_out:
	return -1;
}

char *fyai_todo_commit(struct fyai_ctx *ctx, fy_generic todos)
{
	fy_generic stored;

	if (todo_publish(ctx, todos))
		return NULL;
	/*
	 * The publish wrote the entry and moved branch_prev to it; read the
	 * list back from there. A turnless publish on a branch with no other
	 * state still advances branch_prev, so this holds either way.
	 */
	stored = todo_stored_generic(ctx);
	if (fy_is_invalid(stored))
		return strdup("tool error: the todo list was not stored");
	stored = fy_gb_internalize(fyai_ctx_transient_gb(ctx), stored);
	if (fy_is_invalid(stored))
		return strdup("tool error: the todo list was not stored");
	if (fyai_todo_all_done(stored)) {
		/*
		 * Nothing is left to do: drop the list so the panel goes
		 * away. The finished list stays in the ref log behind
		 * "<branch>@{1}".
		 */
		if (todo_publish(ctx, fy_invalid))
			return NULL;
		return strdup("todo list completed: all items done");
	}
	return strdup(fy_sprintfa("todo list updated: %zu items",
				  fy_len(stored)));
}

int fyai_todo_clear(struct fyai_ctx *ctx)
{
	return todo_publish(ctx, fy_invalid);
}

fy_generic fyai_todo_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	fy_generic stored, item, rows;

	rows = fy_seq_empty;
	stored = todo_stored_generic(ctx);
	if (fy_is_invalid(stored))
		return rows;
	fy_foreach(item, stored) {
		rows = fy_append(gb, rows, fy_mapping(gb,
			"content", fy_get(item, "content", ""),
			"status", fy_get(item, "status", ""),
			"priority", fy_get(item, "priority", "")));
		if (fy_is_invalid(rows))
			return fy_invalid;
	}
	return rows;
}

/* One row of the panel: the state mark and the task text, escaped. */
static const char *todo_mark(const char *status)
{
	if (!strcmp(status, FYAI_TODO_COMPLETED))
		return "x";
	if (!strcmp(status, FYAI_TODO_IN_PROGRESS))
		return ">";
	if (!strcmp(status, FYAI_TODO_CANCELLED))
		return "-";
	return " ";
}

const char *fyai_todo_panel(struct fyai_ctx *ctx)
{
	struct response_buffer out = {0};
	fy_generic stored, item, g;
	const char *text, *status;
	const char *escaped;
	bool first;
	int rc;

	stored = todo_stored_generic(ctx);
	if (fy_is_invalid(stored))
		return NULL;
	first = true;
	fy_foreach(item, stored) {
		g = fy_get(item, "content", fy_invalid);
		text = fy_castp(&g, "");
		g = fy_get(item, "status", fy_invalid);
		status = fy_castp(&g, "");
		escaped = markdown_ui_escape(text);
		if (!escaped) {
			free(out.data);
			return NULL;
		}
		rc = response_buffer_append(&out, first ? "- [" : "\n- [");
		rc = rc || response_buffer_append(&out, todo_mark(status));
		rc = rc || response_buffer_append(&out, "] ");
		rc = rc || response_buffer_append(&out, escaped);
		free((char *)escaped);
		if (rc) {
			free(out.data);
			return NULL;
		}
		first = false;
	}
	rc = response_buffer_append(&out, "\n");
	if (rc) {
		free(out.data);
		return NULL;
	}
	return out.data;
}

char *fyai_todo_format(struct fyai_ctx *ctx)
{
	fy_generic stored, item, g;
	struct response_buffer out = {0};
	const char *text, *status;
	bool first;
	int rc;

	stored = todo_stored_generic(ctx);
	if (fy_is_invalid(stored))
		return NULL;
	first = true;
	fy_foreach(item, stored) {
		g = fy_get(item, "content", fy_invalid);
		text = fy_castp(&g, "");
		g = fy_get(item, "status", fy_invalid);
		status = fy_castp(&g, "");
		rc = response_buffer_append(&out, first ? "- [" : "\n- [");
		rc = rc || response_buffer_append(&out, todo_mark(status));
		rc = rc || response_buffer_append(&out, "] ");
		rc = rc || response_buffer_append(&out, text ? text : "");
		if (rc) {
			free(out.data);
			return NULL;
		}
		first = false;
	}
	rc = response_buffer_append(&out, "\n");
	if (rc) {
		free(out.data);
		return NULL;
	}
	return out.data;
}

char *fyai_todo_format_result(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb,
			      fy_generic args, const char *result)
{
	(void)gb;
	(void)args;
	(void)result;
	return fyai_todo_format(ctx);
}
