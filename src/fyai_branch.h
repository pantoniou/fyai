/* SPDX-License-Identifier: MIT */
#ifndef FYAI_BRANCH_H
#define FYAI_BRANCH_H

#include "fyai.h"

/* Branch state and operations. See doc/branching.md. */

/* Return the active branch or FYAI_BRANCH_DEFAULT. */
const char *fyai_ctx_branch(const struct fyai_ctx *ctx);

/* Set the read-only root handle or symbolic reference. */
int fyai_cfg_set_root(struct fyai_cfg *cfg, const char *spec);

/* Validate and copy @name as the active branch. */
int fyai_ctx_set_branch(struct fyai_ctx *ctx, const char *name);

/* Return the stored HEAD or the active branch. */
const char *fyai_ctx_head_branch(const struct fyai_ctx *ctx);

/*
 * Switch branch and move HEAD with it, so the next invocation starts here.
 * This is the one operation that changes the stored HEAD.
 */
int fyai_ctx_checkout(struct fyai_ctx *ctx, const char *name);

/*
 * Select the branch from the command line. Marks it explicit, so neither the
 * environment nor the arena's HEAD overrides it afterwards.
 */
int fyai_cfg_set_branch(struct fyai_cfg *cfg, const char *name);

/*
 * Apply $FYAI_BRANCH when --branch did not already select one. A missing or
 * empty variable is not an error; an invalid name is.
 */
int fyai_cfg_branch_from_env(struct fyai_cfg *cfg);

/* Return the current time in microseconds as an inline generic integer. */
uint64_t fyai_branch_timestamp(void);

/* The current directory as a string generic, or fy_invalid if unreadable. */
fy_generic fyai_branch_cwd_generic(struct fy_generic_builder *gb);

/* Decoded branch entry. Null values become fy_invalid. */
struct fyai_branch {
	fy_generic entry;	/* the entry mapping itself */
	fy_generic config;	/* this branch's configuration document */
	fy_generic head;	/* tip of the turn chain */
	fy_generic created;	/* first publication of the branch */
	fy_generic updated;	/* publication time of this entry */
	fy_generic cwd;		/* directory the branch started in */
	fy_generic description;	/* free-text purpose of the branch */
	fy_generic agent;	/* sub-agent provenance, if any */
	fy_generic op;		/* the operation that made this entry */
	fy_generic from;	/* the previous name, on a rename */
	fy_generic prev;	/* previous entry of this branch (its ref log) */
};

/*
 * Entry metadata, for an entry of any version. An entry written before the
 * branch carried two timestamps holds one "created" member that is the time of
 * that publication, so it reads as the update time and leaves the creation
 * time unknown.
 */
uint64_t fyai_branch_updated(const struct fyai_branch *b);
uint64_t fyai_branch_created(const struct fyai_branch *b);
const char *fyai_branch_cwd(const struct fyai_branch *b);

/* Operations stored in branch ref-log entries. */
#define FYAI_BRANCH_OP_TURN	"turn"
#define FYAI_BRANCH_OP_CONFIG	"config"
#define FYAI_BRANCH_OP_CREATE	"create"
#define FYAI_BRANCH_OP_RESET	"reset"
#define FYAI_BRANCH_OP_RENAME	"rename"
#define FYAI_BRANCH_OP_CLEAR	"clear"
#define FYAI_BRANCH_OP_COMPACT	"compact"
#define FYAI_BRANCH_OP_CHECKOUT	"checkout"
#define FYAI_BRANCH_OP_DESCRIBE	"describe"
#define FYAI_BRANCH_OP_REBASE	"rebase"
#define FYAI_BRANCH_OP_MERGE	"merge"

/* Set the operation and previous name for the next publish. */
void fyai_branch_op_set(struct fyai_ctx *ctx, const char *op, const char *from);

/* Return true if @name is a valid branch name. */
/* A name a user may create: never contains ':'. */
bool fyai_branch_name_valid(const char *name);
/*
 * A name that may be selected, which also accepts the "agent:<slug>" component
 * that fyai itself writes for a sub-agent branch.
 */
bool fyai_branch_name_ref_valid(const char *name);

/* The hierarchy a fresh interactive session is named under. */
#define FYAI_BRANCH_SESSION_PREFIX "session"

/*
 * Name the branch of a fresh interactive session, under the session
 * hierarchy and free in @branches. Returns 0 on success, -1 on failure.
 */
int fyai_branch_session_name(fy_generic branches, char *buf, size_t size);

/* The marker that makes a branch component a sub-agent branch. */
#define FYAI_BRANCH_AGENT_PREFIX "agent:"


/* Return true if @name is @parent or is below @parent. */
bool fyai_branch_is_below(const char *name, const char *parent);

/* Nesting depth: "main" is 0, "main/explore-1" is 1, and so on. */
unsigned int fyai_branch_depth(const char *name);

/* Longest single path component produced by fyai_branch_sanitize(). */
#define FYAI_BRANCH_COMPONENT_MAX 32

/* Longest full branch name. A buffer of FYAI_BRANCH_NAME_MAX + 1 holds one. */
#define FYAI_BRANCH_NAME_MAX 255

/*
 * Reduce an untrusted string to one valid path component, written to @buf.
 * The name of a sub-agent is chosen by the model, so it is arbitrary text and
 * cannot be used as a branch name as it stands: this lower-cases it, maps
 * every other character to '-', collapses and trims the runs, truncates, and
 * falls back to @fallback when nothing usable is left. The result always
 * satisfies fyai_branch_name_valid() and contains no '/'.
 */
void fyai_branch_sanitize(const char *raw, const char *fallback, char *buf,
			  size_t size);

/*
 * Compose a unique child branch name below @parent from the untrusted @raw,
 * as "<parent>/<slug>-<n>" with the smallest free n. Fails when @parent is
 * already at @max_depth, so a runaway delegation cannot grow the namespace
 * without bound. Returns 0 on success, -1 with a diagnostic raised.
 */
/*
 * Name the branch that a sub-agent called @raw takes below @parent. A name is
 * a handle, so a stored name is that sub-agent and not a clash. @existsp says
 * if the name is there already, and the caller decides between a continuation
 * and a refusal. Pass NULL to refuse a name that is in use.
 */
int fyai_branch_alloc_child(struct fyai_ctx *ctx, const char *parent,
			    const char *raw, unsigned int max_depth,
			    char *buf, size_t size, bool *existsp);

/* Decode @entry into @b. Clear @b and return false on failure. */
bool fyai_branch_decode(fy_generic entry, struct fyai_branch *b);

/* Look up and decode @name. Clear @b and return false if it is absent. */
bool fyai_branch_lookup(fy_generic branches, const char *name,
			struct fyai_branch *b);

/*
 * Build a branch entry from @b. fy_invalid members are stored as null and the
 * "entry" member is ignored. @b->prev chains to this branch's predecessor entry
 * and forms the per-branch ref log. A caller copies the decoded predecessor and
 * changes the members the operation changes, so metadata it does not name is
 * preserved.
 */
fy_generic fyai_branch_build(struct fy_generic_builder *gb,
			     const struct fyai_branch *b);

/*
 * Return a copy of @branches with @name bound to @entry, or with @name removed
 * when @entry is fy_invalid. @branches may be fy_invalid (an empty mapping).
 * Untouched branches are carried over by reference, so a write to one branch
 * leaves every other branch byte-identical.
 */
fy_generic fyai_branches_set(struct fy_generic_builder *gb, fy_generic branches,
			     const char *name, fy_generic entry);

/*
 * The resumable sessions of @branches, newest first, then by name. A sub-agent
 * branch is left out. Without @all, only a branch whose recorded directory is
 * @cwd is included, so a branch that records none appears only under @all.
 * Each row is {branch, updated, created, cwd, turns, model, description}.
 * @ctx only receives diagnostics and may be NULL, so a caller can select a
 * branch before a context exists.
 */
fy_generic fyai_branch_select_rows(struct fyai_ctx *ctx,
				   struct fy_generic_builder *gb,
				   fy_generic branches, const char *cwd,
				   bool all);

/*
 * The most recently updated branch that fyai_branch_select_rows() would list,
 * as a string the caller frees, or NULL when there is none. @ctx may be NULL.
 */
char *fyai_branch_pick_last(struct fyai_ctx *ctx, fy_generic branches,
			    const char *cwd, bool all);

/*
 * Count the turns on a branch, capped at @limit so a listing of many branches
 * cannot walk unbounded history.
 */
long long fyai_branch_turn_count(fy_generic head, long long limit);

/*
 * Resolve a symbolic reference to a turn. Accepted forms are "<branch>",
 * "<branch>~N" (N turns back) and "<branch>@{N}" (N entries back in that
 * branch's ref log); "HEAD" stands for the active branch. A numeric or
 * hexadecimal spec is refused - arena addresses do not survive gc, so a
 * reference must be symbolic. Returns 0 with *headp set (possibly fy_invalid
 * for a turnless point), -1 with a diagnostic raised.
 */
int fyai_resolve_ref(struct fyai_ctx *ctx, const char *spec, fy_generic *headp);

/* Resolve a reference and its configuration. @configp may be NULL. */
int fyai_resolve_ref_state(struct fyai_ctx *ctx, const char *spec,
			   fy_generic *headp, fy_generic *configp);

/*
 * Split a reference into its branch name and its suffix. Returns 0 for a bare
 * name, '~' for "~N" or a run of '^', '@' for "@{N}", or -1 when the spec is
 * malformed. @buf receives the branch part and @np the count.
 */
int fyai_ref_parse(const char *spec, char *buf, size_t size, long long *np);

/* Backends for the `branch` and `checkout` verbs and the /branch command. */
int fyai_branch_list(struct fyai_ctx *ctx, const char *under, bool all);
int fyai_branch_show(struct fyai_ctx *ctx, const char *name);
int fyai_branch_create(struct fyai_ctx *ctx, const char *name,
		       const char *start, const char *description,
		       bool switch_to);
int fyai_branch_delete(struct fyai_ctx *ctx, const char *name, bool force);
int fyai_branch_rename(struct fyai_ctx *ctx, const char *from, const char *to);
/*
 * Adopt an existing branch in memory. @keep_head selects the branch for this
 * invocation only, as --branch does; otherwise HEAD moves with it, so the next
 * invocation starts there.
 */
int fyai_branch_adopt(struct fyai_ctx *ctx, const char *name, bool keep_head);
int fyai_branch_checkout(struct fyai_ctx *ctx, const char *name, bool create,
			 const char *start);
/* Reset the active branch and retain its previous ref-log entry. */
int fyai_branch_reset(struct fyai_ctx *ctx, const char *spec);

/* Report the current root or the root behind @spec. */
int fyai_root_report(struct fyai_ctx *ctx, const char *spec, bool verbose);
int fyai_branch_describe(struct fyai_ctx *ctx, const char *name,
			 const char *description);

#endif
