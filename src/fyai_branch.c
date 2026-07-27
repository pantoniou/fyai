/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fyai.h"
#include "fyai_branch.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

const char *fyai_ctx_branch(const struct fyai_ctx *ctx)
{
	if (ctx->branch && *ctx->branch)
		return ctx->branch;
	if (ctx->cfg && ctx->cfg->branch && *ctx->cfg->branch)
		return ctx->cfg->branch;
	return FYAI_BRANCH_DEFAULT;
}

const char *fyai_ctx_head_branch(const struct fyai_ctx *ctx)
{
	if (ctx->head_branch && *ctx->head_branch)
		return ctx->head_branch;
	return fyai_ctx_branch(ctx);
}

int fyai_ctx_set_branch(struct fyai_ctx *ctx, const char *name)
{
	char *dup;

	if (!fyai_branch_name_valid(name)) {
		fyai_error(ctx, "invalid branch name '%s'", name ? name : "");
		return -1;
	}
	dup = strdup(name);
	if (!dup) {
		fyai_error(ctx, "out of memory setting branch '%s'", name);
		return -1;
	}
	free(ctx->branch);
	ctx->branch = dup;
	return 0;
}

uint64_t fyai_branch_timestamp(void)
{
	struct timespec ts;
	uint64_t usec;

	if (clock_gettime(CLOCK_REALTIME, &ts) || ts.tv_sec < 0)
		return 0;
	usec = (uint64_t)ts.tv_sec * 1000000U + ts.tv_nsec / 1000U;
	return usec <= FYGT_INT_INPLACE_MAX ? usec : 0;
}

/* Characters git's check-ref-format rejects that also have meaning to us:
 * '~' and '@' introduce the reference suffixes, the rest are reserved so a
 * name can never need quoting on a command line. */
static const char fyai_branch_bad_chars[] = "~^@:?*[\\";

bool fyai_branch_name_valid(const char *name)
{
	const char *p, *comp;
	size_t len;

	if (!name || !*name)
		return false;
	if (!strcmp(name, "HEAD"))
		return false;

	len = strlen(name);
	if (name[0] == '/' || name[len - 1] == '/')
		return false;

	comp = name;
	for (p = name; ; p++) {
		if (*p && (isspace((unsigned char)*p) ||
			   iscntrl((unsigned char)*p) ||
			   strchr(fyai_branch_bad_chars, *p)))
			return false;
		if (*p && *p != '/')
			continue;
		/* Reject empty, "." and ".." path components. */
		if (p == comp)
			return false;
		if ((size_t)(p - comp) == 1 && comp[0] == '.')
			return false;
		if ((size_t)(p - comp) == 2 && comp[0] == '.' && comp[1] == '.')
			return false;
		if (!*p)
			break;
		comp = p + 1;
	}
	return true;
}

bool fyai_branch_is_below(const char *name, const char *parent)
{
	size_t plen;

	if (!parent || !*parent)
		return true;
	if (!name)
		return false;
	plen = strlen(parent);
	if (strncmp(name, parent, plen))
		return false;
	return !name[plen] || name[plen] == '/';
}

unsigned int fyai_branch_depth(const char *name)
{
	unsigned int depth = 0;
	const char *p;

	if (!name)
		return 0;
	for (p = name; *p; p++) {
		if (*p == '/')
			depth++;
	}
	return depth;
}

/* Return a branch member and normalize null to fy_invalid. */
static fy_generic fyai_branch_member(fy_generic entry, const char *key)
{
	fy_generic v;

	v = fy_get(entry, key);
	if (fy_generic_is_valid(v) && fy_generic_is_null_type(v))
		v = fy_invalid;
	return v;
}

bool fyai_branch_decode(fy_generic entry, struct fyai_branch *b)
{
	memset(b, 0, sizeof(*b));
	b->entry = fy_invalid;
	b->config = fy_invalid;
	b->head = fy_invalid;
	b->description = fy_invalid;
	b->agent = fy_invalid;
	b->prev = fy_invalid;

	if (!fy_generic_is_mapping(entry))
		return false;

	b->entry = entry;
	b->config = fyai_branch_member(entry, "config");
	b->head = fyai_branch_member(entry, "head");
	b->description = fyai_branch_member(entry, "description");
	b->agent = fyai_branch_member(entry, "agent");
	b->prev = fyai_branch_member(entry, "prev");
	return true;
}

bool fyai_branch_lookup(fy_generic branches, const char *name,
			struct fyai_branch *b)
{
	fy_generic entry;

	entry = fy_invalid;
	if (name && fy_generic_is_mapping(branches))
		entry = fy_get(branches, name);
	return fyai_branch_decode(entry, b);
}

fy_generic fyai_branch_build(struct fy_generic_builder *gb, fy_generic config,
			     fy_generic head, fy_generic created,
			     fy_generic description, fy_generic agent,
			     fy_generic prev)
{
	return fy_mapping(gb,
			  "config", fyai_generic_or_null(config),
			  "head", fyai_generic_or_null(head),
			  "created", fyai_generic_or_null(created),
			  "description", fyai_generic_or_null(description),
			  "agent", fyai_generic_or_null(agent),
			  "prev", fyai_generic_or_null(prev));
}

fy_generic fyai_branches_set(struct fy_generic_builder *gb, fy_generic branches,
			     const char *name, fy_generic entry)
{
	if (!gb || !name)
		return fy_invalid;

	if (!fy_generic_is_mapping(branches)) {
		if (fy_generic_is_invalid(entry))
			return branches;
		branches = fy_gb_mapping(gb);
		if (!fy_generic_is_valid(branches))
			return fy_invalid;
	}

	if (fy_generic_is_invalid(entry))
		return fy_disassoc(gb, branches, name);
	return fy_assoc(gb, branches, name, entry);
}
