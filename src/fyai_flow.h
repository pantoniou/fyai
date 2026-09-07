/*
 * fyai_flow.h - output separation manager
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_FLOW_H
#define FYAI_FLOW_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_cfg;

/* A unit of presented output. Separation applies between two adjacent units. */
enum fyai_flow_unit {
	FYAI_FLOW_NONE,		/* nothing presented yet */
	FYAI_FLOW_USER_CARD,
	FYAI_FLOW_SYSTEM,
	FYAI_FLOW_PROSE,
	FYAI_FLOW_REASONING,
	FYAI_FLOW_TOOL_HEAD,
	FYAI_FLOW_TOOL_BODY,
	FYAI_FLOW_TOOL_TEXT,
	FYAI_FLOW_TOOL_RESULT,
	FYAI_FLOW_NOTICE,
	FYAI_FLOW_TURN_BREAK,
};

/*
 * Separation before the next unit. The configuration owns @markdown, which is
 * a rendered separator or NULL.
 */
struct fyai_flow_sep {
	unsigned rows;
	const char *markdown;
};

/* The state of one output medium. */
struct fyai_flow {
	struct fyai_cfg *cfg;
	enum fyai_flow_unit prev;
	bool at_line_start;		/* no partial row at the tail */
	unsigned blank_rows;		/* blank rows at the tail */
};

void fyai_flow_reset(struct fyai_flow *f, struct fyai_cfg *cfg);

/* Separation between the last unit and @next. It changes no state. */
struct fyai_flow_sep fyai_flow_before(const struct fyai_flow *f,
				      enum fyai_flow_unit next);

/* Record that the medium ends with @rows blank rows. */
void fyai_flow_blank_rows(struct fyai_flow *f, unsigned rows);

/*
 * Record how the medium ends after @buf is presented. A row of terminal
 * control alone is blank.
 */
void fyai_flow_observe(struct fyai_flow *f, const char *buf, size_t len);

/* Blank rows at the start of @buf. */
unsigned fyai_flow_lead_rows(const char *buf, size_t len);

/* Length of @buf with its trailing blank rows reduced to @keep. */
size_t fyai_flow_trim_tail(const char *buf, size_t len, unsigned keep);

/* Record that @unit was presented. */
void fyai_flow_emitted(struct fyai_flow *f, enum fyai_flow_unit unit,
		       bool at_line_start);

/* Test if @unit is part of a tool exchange. */
bool fyai_flow_unit_is_tool(enum fyai_flow_unit unit);

/* Rows @sep occupies. */
unsigned fyai_flow_sep_rows(const struct fyai_flow_sep *sep);

const char *fyai_flow_unit_name(enum fyai_flow_unit unit);

#endif
