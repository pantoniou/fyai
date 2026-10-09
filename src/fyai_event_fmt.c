/* SPDX-License-Identifier: MIT */
/*
 * The events of the run for the user. The text is the model's; this file only
 * decides how it is drawn in the transcript.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_ask.h"
#include "fyai_event_fmt.h"

/* The characters of a kind: lower case letters, digits and "-_". */
static bool event_kind_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       c == '-' || c == '_';
}

static bool event_kind_is(const char *kind, size_t klen, const char *want)
{
	return klen == strlen(want) && !strncmp(kind, want, klen);
}

/* The answers of a question event: the text of the tool result in its body. */
static char *event_answers(const char *body)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	char *s;

	if (!gb)
		return NULL;
	s = fyai_ask_format(NULL, gb, fy_invalid, body);
	fy_generic_builder_destroy(gb);
	return s;
}

char *fyai_event_pretty(const char *text)
{
	const char *kind, *name, *tail, *body, *close, *p;
	size_t klen, nlen, tlen;
	char *out = NULL, *answers;
	size_t len = 0;
	FILE *mf;

	if (!text || *text != '[')
		return NULL;
	kind = text + 1;
	for (p = kind; event_kind_char(*p); p++)
		;
	klen = (size_t)(p - kind);
	if (!klen || p[0] != ' ' || p[1] != '\'')
		return NULL;
	name = p + 2;
	close = strchr(name, '\'');
	if (!close)
		return NULL;
	nlen = (size_t)(close - name);
	/* The tail runs to the bracket that ends the header. */
	tail = close + 1;
	p = strchr(tail, ']');
	if (!p)
		return NULL;
	tlen = (size_t)(p - tail);
	body = p + 1;
	while (*body == ' ')
		body++;
	if (*body == '\n')
		body++;
	while (tlen && *tail == ' ') {
		tail++;
		tlen--;
	}

	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	fprintf(mf, "**%.*s** `%.*s`", (int)klen, kind, (int)nlen, name);
	if (tlen)
		fprintf(mf, " %.*s", (int)tlen, tail);
	fprintf(mf, "\n");
	if (*body) {
		fprintf(mf, "\n");
		answers = event_kind_is(kind, klen, "question") ?
			  event_answers(body) : NULL;
		if (answers) {
			fprintf(mf, "%s", answers);
			free(answers);
		} else if (event_kind_is(kind, klen, "monitor")) {
			/* The lines that a monitor reports keep their breaks. */
			fprintf(mf, "```\n%s\n```\n", body);
		} else {
			fprintf(mf, "%s\n", body);
		}
	}
	fclose(mf);
	return out;
}
