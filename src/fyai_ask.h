/* SPDX-License-Identifier: MIT */
#ifndef FYAI_ASK_H
#define FYAI_ASK_H

#include "fyai.h"

/*
 * The questions of the ask_user tool. One call carries one to four
 * questions. Each has an id, a header, the text, a multi-select flag, and
 * two to four options. A normalized question has these keys:
 *
 *   id, header, question, multi_select,
 *   options: [{id, label, description, preview}]
 *
 * The user can always type an answer of their own instead of an option.
 */
#define FYAI_ASK_QUESTIONS_MAX	4
#define FYAI_ASK_OPTIONS_MAX	4
#define FYAI_ASK_HEADER_MAX	12

/* What the user chose for one question. */
struct fyai_ask_reply {
	unsigned int selected;	/* bit N is option N */
	char *other;		/* text typed in place of the options, or NULL */
};

/*
 * Check the arguments of the tool and return the normalized sequence of
 * questions, built in @gb. Returns fy_invalid and writes the cause in @why
 * when an argument is not valid.
 */
fy_generic fyai_ask_normalize(struct fy_generic_builder *gb, fy_generic args,
			      char *why, size_t why_size);

/* Release the text of @n replies. The array is the caller's. */
void fyai_ask_replies_free(struct fyai_ask_reply *replies, size_t n);

/*
 * Build the result of the tool in @gb from the @n replies to @questions. A
 * NULL @replies says that the user declined to answer.
 */
/*
 * The result of the tool as Markdown for the user: each question with the
 * answer given. @args are the arguments of the call, which name the options,
 * or fy_invalid. Returns a malloc'd string, or NULL for a result that is not
 * an answer, which is shown as it is.
 */
char *fyai_ask_format(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      fy_generic args, const char *result);
fy_generic fyai_ask_result(struct fy_generic_builder *gb, fy_generic questions,
			   const struct fyai_ask_reply *replies, size_t n);

/*
 * Read a line typed in answer to @question: option numbers separated by
 * commas or blanks, or free text. Only a question that allows many choices
 * takes more than one number. Returns 0, or -1 when the text cannot be kept.
 * A blank line leaves @reply empty.
 */
int fyai_ask_reply_from_line(fy_generic question, const char *line,
			     struct fyai_ask_reply *reply);

/* Whether @reply holds an answer. */
bool fyai_ask_reply_given(const struct fyai_ask_reply *reply);

/*
 * The question as plain text for a terminal that has no page: the question,
 * and a numbered line for each option. The caller frees the result.
 */
char *fyai_ask_text(fy_generic question);

/*
 * The questions and the replies as plain text for the transcript: each question
 * and a line with the answer. A NULL @replies shows no answers. The caller
 * frees the result.
 */
char *fyai_ask_echo(fy_generic questions, const struct fyai_ask_reply *replies,
		    size_t n);

#endif
