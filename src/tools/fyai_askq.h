/*
 * fyai_askq.h - the questions component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_ASKQ_H
#define FYAI_ASKQ_H

struct fyai_ask_entry;
struct fyai_ask_pending;
struct fyai_diag;

/*
 * The background questions that wait for the user, and the sets of questions
 * of the questions store with their serial numbers. The component reports
 * into the diagnostics. The owner is struct fyai_app; a view context that
 * detaches from the UI keeps a component of its own.
 */
struct fyai_askq {
	/* Dependency. */
	struct fyai_diag *diag;

	struct fyai_ask_pending *asks;	/* background questions that wait for the user */
	/* The sets of questions in the questions store (src/tools/fyai_ask.c). */
	struct fyai_ask_entry *entries;
	unsigned long long serial;
	unsigned int seq;		/* the number of the last question named by default */
};

void fyai_askq_init(struct fyai_askq *q, struct fyai_diag *diag);

#endif
