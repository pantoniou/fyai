/*
 * fyai_askq.h - the questions component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_ASKQ_H
#define FYAI_ASKQ_H

#include <stddef.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ask_entry;
struct fyai_ask_reply;
struct fyai_ask_pending;
struct fyai_cfg;
struct fyai_diag;
struct fyai_waitq;

/*
 * The background questions that wait for the user, and the sets of questions
 * of the questions store with their serial numbers. The component reports
 * into the diagnostics. The owner is struct fyai_app; a view context that
 * detaches from the UI keeps a component of its own.
 */
struct fyai_askq {
	/* Dependencies. */
	struct fyai_diag *diag;
	struct fyai_waitq *waitq;	/* where an answer is reported */
	struct fyai_cfg *cfg;		/* whose builder keeps the names */
	/*
	 * Called up: put the questions to the user, and take them back. The
	 * answer comes to @done once, unless @close takes the questions first.
	 * Without them a question cannot be asked.
	 */
	int (*open)(void *data, fy_generic questions,
		    void (*done)(void *user, const struct fyai_ask_reply *replies,
				 size_t n), void *user);
	void (*close)(void *data, void *user);
	void *hook_data;

	struct fyai_ask_pending *asks;	/* background questions that wait for the user */
	/* The sets of questions in the questions store (src/tools/fyai_ask.c). */
	struct fyai_ask_entry *entries;
	unsigned long long serial;
	unsigned int seq;		/* the number of the last question named by default */
};

void fyai_askq_init(struct fyai_askq *q, struct fyai_diag *diag,
		    struct fyai_waitq *waitq, struct fyai_cfg *cfg);

#endif
