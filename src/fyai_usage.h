/*
 * fyai_usage.h - the usage accounting component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_USAGE_H
#define FYAI_USAGE_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_usage_sum.h"

struct fyai_diag;

/*
 * The token and cost totals of the run, what the sub-agents of the branch
 * used, the cost of a change of model, and the usage of the last call. The
 * component reports into the diagnostics. The owner is struct fyai_app.
 */
struct fyai_usage {
	/* Dependency. */
	struct fyai_diag *diag;

	/* Accumulated token usage across all model calls in this run. */
	long long input;
	long long cached;
	long long cache_write;
	long long output;
	long long reasoning;
	long long total;
	double cost;
	/* The part of @cost that is estimated from the catalogue prices. */
	double cost_est;
	int calls;
	/*
	 * What the sub-agents of the branch used, from their own branches.
	 * Made again when the branch table or the head changes; @agents_root
	 * is the branch table that it was made from.
	 */
	struct fyai_usage_sum agents;
	int agent_count;
	fy_generic agents_root;
	/* The turn that the counters above total; see fyai_usage_sync(). */
	fy_generic head;
	/*
	 * The cost of a change of model: the next request has no cache on the
	 * new model. @switch_prefix tokens cost @switch_fresh there, and
	 * @switch_stay on the model that was left. Pending until a call ends.
	 */
	bool switch_pending;
	long long switch_prefix;
	double switch_fresh;
	double switch_stay;
	/* Last model call's usage (ground truth for context fill). */
	long long last_call_input;
	long long last_call_output;
	long long last_call_total;
};

void fyai_usage_init(struct fyai_usage *usage, struct fyai_diag *diag);

#endif
