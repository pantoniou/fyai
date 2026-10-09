/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_ask.h"
#include "utils.h"

static fy_generic ask_option(struct fy_generic_builder *gb, fy_generic option,
			     size_t qn, size_t on, bool multi, char *why,
			     size_t why_size)
{
	const char *label, *desc, *preview, *id;

	if (!fy_is_mapping(option)) {
		snprintf(why, why_size, "question %zu option %zu is not an object",
			 qn, on);
		return fy_invalid;
	}
	label = fy_get(option, "label", "");
	desc = fy_get(option, "description", "");
	preview = fy_get(option, "preview", "");
	id = fy_get(option, "id", "");
	if (!*label) {
		snprintf(why, why_size, "question %zu option %zu has no label",
			 qn, on);
		return fy_invalid;
	}
	if (*preview && multi) {
		snprintf(why, why_size,
			 "question %zu allows many choices, so option %zu cannot have a preview",
			 qn, on);
		return fy_invalid;
	}
	return fy_mapping(gb, "id", *id ? id : label, "label", label,
			  "description", desc, "preview", preview);
}

/* Whether option @n of @options repeats the id or label of one before it. */
static bool ask_option_repeats(fy_generic options, size_t n)
{
	fy_generic a, b, ia, ib, la, lb;
	size_t i;

	a = fy_get_at(options, n);
	ia = fy_get(a, "id", fy_invalid);
	la = fy_get(a, "label", fy_invalid);
	for (i = 0; i < n; i++) {
		b = fy_get_at(options, i);
		ib = fy_get(b, "id", fy_invalid);
		lb = fy_get(b, "label", fy_invalid);
		if (fy_equal(ia, ib) || fy_equal(la, lb))
			return true;
	}
	return false;
}

static fy_generic ask_question(struct fy_generic_builder *gb, fy_generic q,
			       size_t qn, char *why, size_t why_size)
{
	fy_generic go, options, option, norm;
	const char *text, *header, *id;
	bool multi;
	size_t on, n;

	if (!fy_is_mapping(q)) {
		snprintf(why, why_size, "question %zu is not an object", qn);
		return fy_invalid;
	}
	text = fy_get(q, "question", "");
	header = fy_get(q, "header", "");
	id = fy_get(q, "id", "");
	if (!*text) {
		snprintf(why, why_size, "question %zu has no text", qn);
		return fy_invalid;
	}
	if (!*header || fyai_utf8_length(header) > FYAI_ASK_HEADER_MAX) {
		snprintf(why, why_size,
			 "question %zu needs a header of 1 to %d characters", qn,
			 FYAI_ASK_HEADER_MAX);
		return fy_invalid;
	}
	multi = fy_get(q, "multi_select", false);
	go = fy_get(q, "options", fy_invalid);
	n = fy_is_sequence(go) ? fy_len(go) : 0;
	if (n < 2 || n > FYAI_ASK_OPTIONS_MAX) {
		snprintf(why, why_size, "question %zu needs 2 to %d options", qn,
			 FYAI_ASK_OPTIONS_MAX);
		return fy_invalid;
	}
	options = fy_sequence(gb);
	on = 0;
	fy_foreach(option, go) {
		norm = ask_option(gb, option, qn, ++on, multi, why, why_size);
		if (fy_is_invalid(norm))
			return fy_invalid;
		options = fy_append(gb, options, norm);
		if (ask_option_repeats(options, on - 1)) {
			snprintf(why, why_size,
				 "question %zu repeats the label or id of option %zu",
				 qn, on);
			return fy_invalid;
		}
	}
	if (!*id)
		id = fy_sprintfa("q%zu", qn);
	return fy_mapping(gb, "id", id, "header", header, "question", text,
			  "multi_select", multi, "options", options);
}

fy_generic fyai_ask_normalize(struct fy_generic_builder *gb, fy_generic args,
			      char *why, size_t why_size)
{
	fy_generic list, q, norm, out;
	size_t qn, i, n;

	why[0] = '\0';
	list = fy_get(args, "questions", fy_invalid);
	n = fy_is_sequence(list) ? fy_len(list) : 0;
	if (n < 1 || n > FYAI_ASK_QUESTIONS_MAX) {
		snprintf(why, why_size, "ask_user needs 1 to %d questions",
			 FYAI_ASK_QUESTIONS_MAX);
		return fy_invalid;
	}
	out = fy_sequence(gb);
	qn = 0;
	fy_foreach(q, list) {
		norm = ask_question(gb, q, ++qn, why, why_size);
		if (fy_is_invalid(norm))
			return fy_invalid;
		for (i = 0; i < qn - 1; i++)
			if (fy_equal(fy_get(fy_get_at(out, i), "id", fy_invalid),
				     fy_get(norm, "id", fy_invalid))) {
				snprintf(why, why_size,
					 "question %zu repeats the id of question %zu",
					 qn, i + 1);
				return fy_invalid;
			}
		out = fy_append(gb, out, norm);
	}
	return out;
}

void fyai_ask_replies_free(struct fyai_ask_reply *replies, size_t n)
{
	size_t i;

	for (i = 0; replies && i < n; i++) {
		free(replies[i].other);
		replies[i].other = NULL;
		replies[i].selected = 0;
	}
}

bool fyai_ask_reply_given(const struct fyai_ask_reply *reply)
{
	return reply->selected || (reply->other && *reply->other);
}

fy_generic fyai_ask_result(struct fy_generic_builder *gb, fy_generic questions,
			   const struct fyai_ask_reply *replies, size_t n)
{
	fy_generic q, options, answers, selected, answer;
	size_t i, o;

	if (!replies)
		return fy_mapping(gb, "status", "declined", "note",
				  "the user did not provide an answer");
	answers = fy_sequence(gb);
	i = 0;
	fy_foreach(q, questions) {
		if (i >= n)
			break;
		options = fy_get(q, "options", fy_invalid);
		selected = fy_sequence(gb);
		for (o = 0; o < fy_len(options); o++)
			if (replies[i].selected & (1u << o))
				selected = fy_append(gb, selected,
					fy_get(fy_get_at(options, o), "id",
					       fy_invalid));
		answer = fy_mapping(gb, "id", fy_get(q, "id", fy_invalid),
				    "header", fy_get(q, "header", fy_invalid),
				    "question", fy_get(q, "question", fy_invalid),
				    "selected", selected);
		if (replies[i].other && *replies[i].other)
			answer = fy_assoc(gb, answer, "other", replies[i].other);
		answers = fy_append(gb, answers, answer);
		i++;
	}
	return fy_mapping(gb, "status", "answered", "answers", answers);
}

/*
 * The label of the option @id of question @qid, else @id itself. A short
 * label lives in the word of its generic, so it is copied to @gb.
 */
static const char *ask_label(struct fy_generic_builder *gb,
			     fy_generic questions, const char *qid,
			     const char *id)
{
	fy_generic q, o, gid, glabel;

	fy_foreach(q, questions) {
		gid = fy_get(q, "id", fy_invalid);
		if (strcmp(fy_castp(&gid, ""), qid))
			continue;
		fy_foreach(o, fy_get(q, "options", fy_invalid)) {
			gid = fy_get(o, "id", fy_invalid);
			if (strcmp(fy_castp(&gid, ""), id))
				continue;
			glabel = fy_get(o, "label", fy_invalid);
			return fy_gb_intern_string(gb, fy_castp(&glabel, id));
		}
	}
	return id;
}

char *fyai_ask_format(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      fy_generic args, const char *result)
{
	fy_generic res, answers, a, id, sel, s, questions = fy_invalid;
	const char *qid, *other, *status;
	char why[128];
	char *out = NULL;
	size_t len = 0, n;
	FILE *mf;

	(void)ctx;
	res = parse_json_string(gb, result);
	if (!fy_is_mapping(res))
		return NULL;
	if (fy_is_valid(args))
		questions = fyai_ask_normalize(gb, args, why, sizeof(why));
	status = fy_get(res, "status", "");
	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	if (!strcmp(status, "declined")) {
		fprintf(mf, "_The user did not answer._\n");
	} else if (!strcmp(status, "answered")) {
		answers = fy_get(res, "answers", fy_invalid);
		fy_foreach(a, answers) {
			id = fy_get(a, "id", fy_invalid);
			qid = fy_castp(&id, "");
			fprintf(mf, "- **%s** %s  \n", fy_get(a, "header", ""),
				fy_get(a, "question", ""));
			sel = fy_get(a, "selected", fy_invalid);
			n = 0;
			fy_foreach(s, sel)
				fprintf(mf, "%s%s", n++ ? ", " : "  \u2192 ",
					ask_label(gb, questions, qid,
						  fy_castp(&s, "")));
			other = fy_get(a, "other", "");
			if (*other)
				fprintf(mf, "%s\u201c%s\u201d", n++ ? ", " : "  \u2192 ",
					other);
			if (!n)
				fprintf(mf, "  \u2192 _no answer_");
			fprintf(mf, "\n");
		}
	} else {
		fclose(mf);
		free(out);
		return NULL;
	}
	fclose(mf);
	return out;
}

/* Read @s as a list of option numbers; false when it is anything else. */
static bool ask_numbers(const char *s, size_t noptions, bool multi,
			unsigned int *mask)
{
	unsigned int bits = 0;
	char *end;
	unsigned long v;
	size_t count = 0;

	for (;;) {
		while (*s == ' ' || *s == '\t' || *s == ',')
			s++;
		if (!*s)
			break;
		if (!isdigit((unsigned char)*s))
			return false;
		v = strtoul(s, &end, 10);
		if (v < 1 || v > noptions)
			return false;
		bits |= 1u << (v - 1);
		count++;
		s = end;
	}
	if (!count || (!multi && count != 1))
		return false;
	*mask = bits;
	return true;
}

int fyai_ask_reply_from_line(fy_generic question, const char *line,
			     struct fyai_ask_reply *reply)
{
	const char *s = line ? line : "";
	size_t len;

	reply->selected = 0;
	reply->other = NULL;
	while (isspace((unsigned char)*s))
		s++;
	len = strlen(s);
	while (len && isspace((unsigned char)s[len - 1]))
		len--;
	if (!len)
		return 0;
	if (ask_numbers(s, fy_len(fy_get(question, "options", fy_invalid)),
			fy_get(question, "multi_select", false),
			&reply->selected))
		return 0;
	reply->other = strndup(s, len);
	return reply->other ? 0 : -1;
}

char *fyai_ask_text(fy_generic question)
{
	fy_generic options, option, gt, gl, gd;
	const char *desc;
	char *buf = NULL;
	size_t size = 0, i = 0;
	FILE *fp;

	fp = open_memstream(&buf, &size);
	if (!fp)
		return NULL;
	gt = fy_get(question, "question", fy_invalid);
	fprintf(fp, "? %s\n", fy_castp(&gt, ""));
	options = fy_get(question, "options", fy_invalid);
	fy_foreach(option, options) {
		gl = fy_get(option, "label", fy_invalid);
		gd = fy_get(option, "description", fy_invalid);
		desc = fy_castp(&gd, "");
		fprintf(fp, "  %zu) %s%s%s\n", ++i, fy_castp(&gl, ""),
			*desc ? " - " : "", desc);
	}
	if (fy_get(question, "multi_select", false))
		fprintf(fp, "  (several numbers, separated by commas, are allowed)\n");
	if (fclose(fp)) {
		free(buf);
		return NULL;
	}
	return buf;
}

char *fyai_ask_echo(fy_generic questions, const struct fyai_ask_reply *replies,
		    size_t n)
{
	fy_generic q, options, gt, gl;
	char *buf = NULL;
	size_t size = 0, i = 0, o, shown;
	FILE *fp;

	fp = open_memstream(&buf, &size);
	if (!fp)
		return NULL;
	fy_foreach(q, questions) {
		gt = fy_get(q, "question", fy_invalid);
		fprintf(fp, "? %s\n> ", fy_castp(&gt, ""));
		shown = 0;
		options = fy_get(q, "options", fy_invalid);
		for (o = 0; replies && i < n && o < fy_len(options); o++) {
			if (!(replies[i].selected & (1u << o)))
				continue;
			gl = fy_get(fy_get_at(options, o), "label", fy_invalid);
			fprintf(fp, "%s%s", shown++ ? ", " : "",
				fy_castp(&gl, ""));
		}
		if (replies && i < n && replies[i].other && *replies[i].other)
			fprintf(fp, "%s%s", shown++ ? ", " : "", replies[i].other);
		if (!shown)
			fprintf(fp, "(no answer)");
		fprintf(fp, "\n");
		i++;
	}
	if (fclose(fp)) {
		free(buf);
		return NULL;
	}
	return buf;
}
