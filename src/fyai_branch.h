/* SPDX-License-Identifier: MIT */
#ifndef FYAI_BRANCH_H
#define FYAI_BRANCH_H

#include "fyai.h"

/* Branch state and operations. See doc/branching.md. */

/* Room for an RFC 3339 UTC timestamp, e.g. "2026-07-27T09:15:04Z". */
#define FYAI_BRANCH_TIMESTAMP_SIZE 32

/* Return the active branch or FYAI_BRANCH_DEFAULT. */
const char *fyai_ctx_branch(const struct fyai_ctx *ctx);

/* Validate and copy @name as the active branch. */
int fyai_ctx_set_branch(struct fyai_ctx *ctx, const char *name);

/* Return the stored HEAD or the active branch. */
const char *fyai_ctx_head_branch(const struct fyai_ctx *ctx);

/* Return the current time in microseconds as an inline generic integer. */
uint64_t fyai_branch_timestamp(void);

/* Decoded branch entry. Null values become fy_invalid. */
struct fyai_branch {
	fy_generic entry;	/* the entry mapping itself */
	fy_generic config;	/* this branch's configuration document */
	fy_generic head;	/* tip of the turn chain */
	fy_generic description;	/* free-text purpose of the branch */
	fy_generic agent;	/* sub-agent provenance, if any */
	fy_generic prev;	/* previous entry of this branch (its ref log) */
};

/* Return true if @name is a valid branch name. */
bool fyai_branch_name_valid(const char *name);

/* Return true if @name is @parent or is below @parent. */
bool fyai_branch_is_below(const char *name, const char *parent);

/* Nesting depth: "main" is 0, "main/explore-1" is 1, and so on. */
unsigned int fyai_branch_depth(const char *name);

/* Decode @entry into @b. Clear @b and return false on failure. */
bool fyai_branch_decode(fy_generic entry, struct fyai_branch *b);

/* Look up and decode @name. Clear @b and return false if it is absent. */
bool fyai_branch_lookup(fy_generic branches, const char *name,
			struct fyai_branch *b);

/*
 * Build a branch entry. fy_invalid members are stored as null. @prev chains to
 * this branch's predecessor entry and forms the per-branch ref log.
 */
fy_generic fyai_branch_build(struct fy_generic_builder *gb, fy_generic config,
			     fy_generic head, fy_generic created,
			     fy_generic description, fy_generic agent,
			     fy_generic prev);

/*
 * Return a copy of @branches with @name bound to @entry, or with @name removed
 * when @entry is fy_invalid. @branches may be fy_invalid (an empty mapping).
 * Untouched branches are carried over by reference, so a write to one branch
 * leaves every other branch byte-identical.
 */
fy_generic fyai_branches_set(struct fy_generic_builder *gb, fy_generic branches,
			     const char *name, fy_generic entry);

#endif
