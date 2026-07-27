/* SPDX-License-Identifier: MIT */
#ifndef FYAI_MERGE_H
#define FYAI_MERGE_H

#include "fyai.h"

/* Branch conversation join operations. */

enum fyai_join_mode {
	FYAI_JOIN_REBASE,
	FYAI_JOIN_MERGE,
};

/* Return the newest turn shared by @a and @b. */
fy_generic fyai_merge_base(struct fyai_ctx *ctx, fy_generic a, fy_generic b);

/* Join @source into the active branch. */
int fyai_branch_join(struct fyai_ctx *ctx, const char *source,
		     enum fyai_join_mode mode, bool allow_unrelated);

/* Turn times read from a branch ref log. */
struct fyai_turn_time;

struct fyai_turn_times {
	struct fyai_turn_time *items;
	size_t count;
};

int fyai_turn_times_collect(struct fyai_ctx *ctx, fy_generic entry,
			    struct fyai_turn_times *tt);
void fyai_turn_times_cleanup(struct fyai_turn_times *tt);
uint64_t fyai_turn_time_lookup(const struct fyai_turn_times *tt,
			       fy_generic turn);

#endif
