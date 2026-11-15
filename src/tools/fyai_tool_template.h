/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TOOL_TEMPLATE_H
#define FYAI_TOOL_TEMPLATE_H

#include "fyai.h"

/*
 * A tool description can hold {{name}}. The model is sent the text that the
 * name stands for in this run, so a description states what the run can do:
 * an option that is off, or that the run cannot use, says nothing.
 *
 * The run decides the state of a name. The text for each state is data: the
 * tool lists it under `templates`, name by state, beside its description. A
 * state with no text, or a name that has no entry, stands for no text. The
 * `templates` key is not sent to the provider.
 */

/*
 * The state of name in this run: a non-empty string that names a text of the
 * tool, "" when the name stands for no text, or NULL when name is not listed.
 */
const char *fyai_tool_template_state(struct fyai_ctx *ctx, const char *name);

/*
 * Expand every string of a tool in place of its {{name}} from the templates of
 * the tool, and remove the `templates` key.
 */
fy_generic fyai_tool_template_expand(struct fyai_ctx *ctx, fy_generic tool);

#endif
