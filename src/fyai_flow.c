/*
 * fyai_flow.c - output separation manager
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stddef.h>

#include "fyai.h"
#include "fyai_flow.h"
#include "fyai_terminal.h"
#include "fyai_diag.h"

static const char *flow_separator(const char *sep)
{
	return sep && *sep ? sep : NULL;
}

bool fyai_flow_unit_is_tool(enum fyai_flow_unit unit)
{
	switch (unit) {
	case FYAI_FLOW_TOOL_HEAD:
	case FYAI_FLOW_TOOL_BODY:
	case FYAI_FLOW_TOOL_TEXT:
	case FYAI_FLOW_TOOL_RESULT:
		return true;
	default:
		break;
	}
	return false;
}

const char *fyai_flow_unit_name(enum fyai_flow_unit unit)
{
	switch (unit) {
	case FYAI_FLOW_NONE:		return "none";
	case FYAI_FLOW_USER_CARD:	return "user";
	case FYAI_FLOW_SYSTEM:		return "system";
	case FYAI_FLOW_PROSE:		return "prose";
	case FYAI_FLOW_REASONING:	return "reasoning";
	case FYAI_FLOW_TOOL_HEAD:	return "tool_head";
	case FYAI_FLOW_TOOL_BODY:	return "tool_body";
	case FYAI_FLOW_TOOL_TEXT:	return "tool_text";
	case FYAI_FLOW_TOOL_RESULT:	return "tool_result";
	case FYAI_FLOW_NOTICE:		return "notice";
	case FYAI_FLOW_TURN_BREAK:	return "turn_break";
	}
	return "unknown";
}

void fyai_flow_reset(struct fyai_flow *f, struct fyai_cfg *cfg)
{
	if (!f)
		return;
	f->cfg = cfg;
	f->prev = FYAI_FLOW_NONE;
	f->at_line_start = true;
	f->blank_rows = 0;
}

void fyai_flow_blank_rows(struct fyai_flow *f, unsigned rows)
{
	if (f)
		f->blank_rows = rows;
}

unsigned fyai_flow_lead_rows(const char *buf, size_t len)
{
	unsigned rows = 0;
	size_t i = 0;
	size_t eol;

	while (i < len) {
		eol = i;
		while (eol < len && buf[eol] != '\n')
			eol++;
		if (eol >= len)
			break;
		if (terminal_trim_blank_rows(buf + i, eol + 1 - i))
			break;
		rows++;
		i = eol + 1;
	}
	return rows;
}

size_t fyai_flow_trim_tail(const char *buf, size_t len, unsigned keep)
{
	size_t end;
	size_t i;
	unsigned rows = 0;

	if (!buf || !len)
		return len;
	end = terminal_trim_blank_rows(buf, len);
	if (!end)
		return len;
	for (i = end; i < len; i++) {
		if (buf[i] != '\n')
			continue;
		rows++;
		if (rows > keep)
			return i;
	}
	return len;
}

/*
 * Blank rows at the end of @buf. @whole reports that every row of @buf is
 * blank. A row with no terminator is open, not blank.
 */
static unsigned tail_blank_rows(const char *buf, size_t len, bool *whole)
{
	unsigned rows = 0;
	size_t line_start;
	size_t end;

	*whole = false;
	if (!len || buf[len - 1] != '\n')
		return 0;
	end = len - 1;
	for (;;) {
		line_start = end;
		while (line_start && buf[line_start - 1] != '\n')
			line_start--;
		if (terminal_trim_blank_rows(buf + line_start,
					     end - line_start))
			return rows;
		rows++;
		if (!line_start) {
			*whole = true;
			return rows;
		}
		end = line_start - 1;
	}
}

void fyai_flow_observe(struct fyai_flow *f, const char *buf, size_t len)
{
	unsigned rows;
	bool was_start;
	bool whole;

	if (!f || !buf || !len)
		return;
	was_start = f->at_line_start;
	f->at_line_start = terminal_text_at_line_start(buf, len);
	if (!f->at_line_start) {
		/* An open row is at the tail. */
		f->blank_rows = 0;
		return;
	}
	rows = tail_blank_rows(buf, len, &whole);
	/* The first newline of an all-blank write closes an open row. */
	if (whole && !was_start && rows)
		rows--;
	f->blank_rows = whole ? f->blank_rows + rows : rows;
}

unsigned fyai_flow_sep_rows(const struct fyai_flow_sep *sep)
{
	if (!sep)
		return 0;
	/* A rendered separator takes its own row. */
	return sep->rows + (sep->markdown ? 1 : 0);
}

static struct fyai_flow_sep flow_sep_raw(const struct fyai_flow *f,
					 enum fyai_flow_unit next);

struct fyai_flow_sep fyai_flow_before(const struct fyai_flow *f,
				      enum fyai_flow_unit next)
{
	struct fyai_flow_sep sep;

	sep = flow_sep_raw(f, next);
	/* A blank row already present replaces one the manager would add. */
	if (f && sep.rows) {
		if (f->blank_rows >= sep.rows)
			sep.rows = 0;
		else
			sep.rows -= f->blank_rows;
	}
	return sep;
}

static struct fyai_flow_sep flow_sep_raw(const struct fyai_flow *f,
					 enum fyai_flow_unit next)
{
	struct fyai_flow_sep sep = {0, NULL};
	struct fyai_cfg *cfg;
	unsigned fence;
	bool prev_tool;
	bool next_tool;

	if (!f || !f->cfg)
		return sep;
	cfg = f->cfg;

	/* No separation goes before the first unit. */
	if (f->prev == FYAI_FLOW_NONE)
		return sep;

	prev_tool = fyai_flow_unit_is_tool(f->prev);
	next_tool = fyai_flow_unit_is_tool(next);
	fence = cfg->tool_group_fence > 0 ? (unsigned)cfg->tool_group_fence : 0;

	/* The transcript view draws the configured rule. Here a break is a row. */
	if (next == FYAI_FLOW_USER_CARD || next == FYAI_FLOW_SYSTEM ||
	    next == FYAI_FLOW_TURN_BREAK) {
		sep.rows = 1;
		return sep;
	}

	/* The card sets the separation that follows it. */
	if (f->prev == FYAI_FLOW_USER_CARD) {
		sep.rows = cfg->user_card_fence > 0 ?
			(unsigned)cfg->user_card_fence : 0;
		return sep;
	}

	/*
	 * A title row opens a call and is fenced. The body, the screen and the
	 * result continue that call.
	 */
	if (next == FYAI_FLOW_TOOL_HEAD) {
		sep.rows = fence;
		return sep;
	}
	if (next_tool)
		return sep;
	if (prev_tool) {
		sep.rows = fence;
		return sep;
	}

	/* Reasoning ends with its configured break. */
	if (f->prev == FYAI_FLOW_REASONING && next != FYAI_FLOW_REASONING) {
		sep.rows = 1;
		sep.markdown = flow_separator(cfg->section_separator);
		return sep;
	}

	if (next == FYAI_FLOW_NOTICE || f->prev == FYAI_FLOW_NOTICE) {
		sep.rows = 1;
		return sep;
	}

	/* Consecutive units of one kind are one block. */
	if (f->prev == next)
		return sep;

	sep.rows = 1;
	return sep;
}

void fyai_flow_emitted(struct fyai_flow *f, enum fyai_flow_unit unit,
		       bool at_line_start)
{
	if (!f)
		return;
	f->prev = unit;
	f->at_line_start = at_line_start;
	f->blank_rows = 0;
}
