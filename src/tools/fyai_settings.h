/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SETTINGS_H
#define FYAI_SETTINGS_H

#include "fyai.h"
#include "fyai_ask.h"

/*
 * The log @member of the store of this branch: `config_changes`, the
 * changes of the model that fyai applied, or `config_proposals`, those that
 * wait for the user. A sequence of entries named `cN` and `pN`; the empty
 * sequence when the branch has none. Borrowed from the arena.
 */
fy_generic fyai_settings_log(struct fyai_ctx *ctx, const char *member);

/*
 * The pending proposal @id with its explanation and the diff it makes over
 * the configuration in force, built in @gb: {id, explanation, diff}. Reports
 * the cause and returns fy_invalid when there is no such pending proposal or
 * it cannot be applied now.
 */
fy_generic fyai_settings_proposal_show(struct fyai_ctx *ctx,
				       struct fy_generic_builder *gb,
				       const char *id);

/*
 * Approve or reject the pending proposal @id, which the user named. With
 * @notify, the model is told by an event. *@resultp is {id, status, change,
 * diff}, built in @gb. Returns 0, or -1 with the cause reported. A session
 * shows the review panel first, through fyai_settings_review_open().
 */
int fyai_settings_proposal_decide(struct fyai_ctx *ctx,
				  struct fy_generic_builder *gb,
				  const char *id, bool approve, bool notify,
				  fy_generic *resultp);

/*
 * Take back change @id of the change log: each path to its value before the
 * change. A path whose value changed since is a conflict: without @force
 * nothing is taken back, with it the other paths are. The revert is a change
 * of its own, and the entry becomes `reverted`. *@resultp is {id, status,
 * change, conflicts, diff}. Returns 0, or -1 with the cause reported.
 */
int fyai_settings_revert(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 const char *id, bool force, fy_generic *resultp);

/*
 * Store the trial change @id: each key of stored scope goes to the branch
 * configuration with the value of the trial and leaves the session layer.
 * The entry becomes `kept`. Returns 0, or -1 with the cause reported.
 */
int fyai_settings_keep(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		       const char *id, fy_generic *resultp);

/*
 * The review of the pending proposal @id in the review panel. Open checks the
 * proposal over the configuration in force and makes its diff; the questions
 * go to fyai_ask_open(), whose callback gives the replies to apply. Apply
 * applies the proposal when the user accepts it, or when @replies is NULL
 * because no panel asked; otherwise the proposal stays pending. With
 * @notify, the model is told by an event. The review borrows @gb, which
 * must live until free. Open reports the cause and returns NULL on failure.
 */
struct fyai_settings_review;

struct fyai_settings_review *
fyai_settings_review_open(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  const char *id);
fy_generic fyai_settings_review_questions(struct fyai_settings_review *r);
int fyai_settings_review_apply(struct fyai_settings_review *r,
			       const struct fyai_ask_reply *replies, size_t n,
			       bool notify, fy_generic *resultp);
void fyai_settings_review_free(struct fyai_settings_review *r);

/* Remove the files that this invocation exported, and their directory. */
void fyai_settings_release(struct fyai_ctx *ctx);

/* Forget that directory without removing it: a forked child does not own it. */
void fyai_settings_abandon(struct fyai_ctx *ctx);

#endif
