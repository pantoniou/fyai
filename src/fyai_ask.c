/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_ask.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
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
	fy_generic q, options, answers, selected, labels, answer;
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
		labels = fy_sequence(gb);
		for (o = 0; o < fy_len(options); o++)
			if (replies[i].selected & (1u << o)) {
				selected = fy_append(gb, selected,
					fy_get(fy_get_at(options, o), "id",
					       fy_invalid));
				labels = fy_append(gb, labels,
					fy_get(fy_get_at(options, o), "label",
					       fy_invalid));
			}
		answer = fy_mapping(gb, "id", fy_get(q, "id", fy_invalid),
				    "header", fy_get(q, "header", fy_invalid),
				    "question", fy_get(q, "question", fy_invalid),
				    "selected", selected, "labels", labels);
		if (replies[i].other && *replies[i].other)
			answer = fy_assoc(gb, answer, "other", replies[i].other);
		answers = fy_append(gb, answers, answer);
		i++;
	}
	return fy_mapping(gb, "status", "answered", "answers", answers);
}

char *fyai_ask_format(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      fy_generic args, const char *result)
{
	fy_generic res, answers, a, sel, s;
	const char *other, *status;
	char *out = NULL;
	size_t len = 0, n;
	FILE *mf;

	(void)ctx;
	(void)args;
	res = parse_json_string(gb, result);
	if (!fy_is_mapping(res))
		return NULL;
	status = fy_get(res, "status", "");
	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	if (!strcmp(status, "declined")) {
		fprintf(mf, "_The user did not answer._\n");
	} else if (!strcmp(status, "answered")) {
		answers = fy_get(res, "answers", fy_invalid);
		fy_foreach(a, answers) {
			fprintf(mf, "- **%s** %s  \n", fy_get(a, "header", ""),
				fy_get(a, "question", ""));
			/* The labels are the options as the user read them. */
			sel = fy_get(a, "labels", fy_invalid);
			if (!fy_is_sequence(sel))
				sel = fy_get(a, "selected", fy_invalid);
			n = 0;
			fy_foreach(s, sel)
				fprintf(mf, "%s%s", n++ ? ", " : "  \u2192 ",
					fy_castp(&s, ""));
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

/* One set of questions that waits in the questions store. */
struct fyai_ask_entry {
	struct fyai_ask_entry *next;
	char qid[32];
	fyai_ask_done_fn done;
	void *user;
};

/* Rewrite the member "open" of the questions store without the set @qid. */
static void ask_store_remove(struct fyai_ctx *ctx, const char *qid)
{
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	fy_generic open = fy_seq_empty, item;

	if (gb) {
		fy_foreach(item, fy_get(fyai_uimodel_get(ctx, FYAI_UI_QUESTIONS),
					"open", fy_seq_empty))
			if (strcmp(fy_get(item, "qid", ""), qid))
				open = fy_append(gb, open, item);
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_QUESTIONS, "open",
					      open);
	}
	fyai_uimodel_end(ctx);
}

int fyai_ask_open(struct fyai_ctx *ctx, fy_generic questions, const char *from,
		  fyai_ask_done_fn done, void *user)
{
	struct fyai_ask_entry *e, **ep;
	struct fy_generic_builder *gb;
	fy_generic item, open;
	size_t nq;
	int rc;

	nq = fy_is_sequence(questions) ? fy_len(questions) : 0;
	if (!done || !nq || nq > FYAI_ASK_QUESTIONS_MAX ||
	    !fyai_ui_ask_available(ctx))
		return -1;
	e = calloc(1, sizeof(*e));
	fyai_error_check(ctx, e, err, "cannot keep a set of questions");
	snprintf(e->qid, sizeof(e->qid), "q%llu", ++ctx->ask_serial);
	e->done = done;
	e->user = user;
	gb = fyai_uimodel_begin(ctx);
	rc = -1;
	if (gb) {
		item = fy_mapping(gb, "qid", e->qid, "questions", questions);
		if (!fy_str_empty(from))
			item = fy_assoc(gb, item, "from", fy_value(gb, from));
		open = fy_append(gb, fy_get(fyai_uimodel_get(ctx,
				FYAI_UI_QUESTIONS), "open", fy_seq_empty), item);
		rc = fyai_uimodel_set_member(ctx, FYAI_UI_QUESTIONS, "open",
					     open);
	}
	fyai_uimodel_end(ctx);
	if (rc < 0) {
		free(e);
		return -1;
	}
	for (ep = &ctx->ask_entries; *ep; ep = &(*ep)->next)
		;
	*ep = e;
	/* The legacy path: the oracle of the store. */
	if (fyai_ui_ask_oracle(ctx, e->qid, questions, from)) {
		*ep = NULL;
		ask_store_remove(ctx, e->qid);
		free(e);
		return -1;
	}
	return 0;
err:
	return -1;
}

void fyai_ask_close(struct fyai_ctx *ctx, void *user)
{
	struct fyai_ask_entry **ep, *e;

	if (!ctx)
		return;
	ep = &ctx->ask_entries;
	while ((e = *ep)) {
		if (e->user != user) {
			ep = &e->next;
			continue;
		}
		*ep = e->next;
		fyai_ui_ask_oracle_drop(ctx, e->qid);
		ask_store_remove(ctx, e->qid);
		free(e);
	}
}

/* Send the answer to set @qid of a mirror to the process that owns it. */
static void ask_settle_remote(struct fyai_ctx *ctx, const char *qid,
			      const struct fyai_ask_reply *replies, size_t n)
{
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	fy_generic values = fy_seq_empty, selected;
	unsigned int j;
	size_t i;

	if (gb) {
		for (i = 0; replies && i < n; i++) {
			selected = fy_seq_empty;
			for (j = 0; j < FYAI_ASK_OPTIONS_MAX; j++)
				if (replies[i].selected & (1u << j))
					selected = fy_append(gb, selected,
							     (long long)j);
			values = fy_append(gb, values, fy_mapping(gb,
				"selected", selected,
				"other", replies[i].other ?
					fy_value(gb, replies[i].other) : fy_null));
		}
		if (fyai_uimodel_intent(ctx, "fyai_answer", fy_mapping(gb,
				"qid", fy_value(gb, qid), "replies", values)))
			fyai_error(ctx, "cannot send the answer to the questions");
	}
	fyai_uimodel_end(ctx);
}

void fyai_ask_settle(struct fyai_ctx *ctx, const char *qid,
		     const struct fyai_ask_reply *replies, size_t n)
{
	struct fyai_ask_entry **ep, *e;

	/* The set of a mirror belongs to the process that owns the model. */
	if (fyai_uimodel_mirrored(ctx)) {
		ask_settle_remote(ctx, qid, replies, n);
		return;
	}
	for (ep = &ctx->ask_entries; (e = *ep); ep = &e->next)
		if (!strcmp(e->qid, qid))
			break;
	if (!e)
		return;
	*ep = e->next;
	ask_store_remove(ctx, e->qid);
	/* The callback can open the next set: it is linked again by then. */
	e->done(e->user, replies, n);
	free(e);
}

void fyai_ask_settle_all(struct fyai_ctx *ctx)
{
	while (ctx && ctx->ask_entries)
		fyai_ask_settle(ctx, ctx->ask_entries->qid, NULL, 0);
}

void fyai_ask_abandon(struct fyai_ctx *ctx)
{
	struct fyai_ask_entry *e;

	while (ctx && (e = ctx->ask_entries)) {
		ctx->ask_entries = e->next;
		free(e);
	}
}
