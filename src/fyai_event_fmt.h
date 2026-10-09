/* SPDX-License-Identifier: MIT */
#ifndef FYAI_EVENT_FMT_H
#define FYAI_EVENT_FMT_H

/*
 * An event is a line of text for the model: "[KIND 'NAME' TAIL]", then an
 * optional body. Write one as Markdown for the user: the kind in bold, the
 * name in code, the tail, and the body under them. The answers of a question
 * are listed as the answer of the ask_user tool is. Returns a malloc'd string,
 * or NULL for a text that is not an event, which is shown as it is. The model
 * always gets the text as it is.
 */
char *fyai_event_pretty(const char *text);

#endif
