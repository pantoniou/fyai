/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_render.h"
#include "fyai_storage.h"
#include "fyai_turn.h"

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

int fyai_ctx_checkout(struct fyai_ctx *ctx, const char *name)
{
	char *dup;

	if (fyai_ctx_set_branch(ctx, name))
		return -1;
	dup = strdup(name);
	if (!dup) {
		fyai_error(ctx, "out of memory checking out '%s'", name);
		return -1;
	}
	free(ctx->head_branch);
	ctx->head_branch = dup;
	return 0;
}

int fyai_cfg_set_branch(struct fyai_cfg *cfg, const char *name)
{
	char *dup;

	if (!fyai_branch_name_valid(name)) {
		fyai_cfg_error(cfg, "invalid branch name '%s'", name ? name : "");
		return -1;
	}
	dup = strdup(name);
	if (!dup) {
		fyai_cfg_error(cfg, "out of memory setting branch '%s'", name);
		return -1;
	}
	free(cfg->branch);
	cfg->branch = dup;
	cfg->branch_explicit = true;
	return 0;
}

int fyai_cfg_branch_from_env(struct fyai_cfg *cfg)
{
	const char *env;

	if (cfg->branch_explicit)
		return 0;
	env = getenv("FYAI_BRANCH");
	if (!env || !*env)
		return 0;
	return fyai_cfg_set_branch(cfg, env);
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

void fyai_branch_op_set(struct fyai_ctx *ctx, const char *op, const char *from)
{
	ctx->branch_op = op;
	ctx->branch_op_from = from;
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

void fyai_branch_sanitize(const char *raw, const char *fallback, char *buf,
			  size_t size)
{
	size_t n;
	char c;

	n = 0;
	if (raw) {
		for (; *raw && n + 1 < size && n < FYAI_BRANCH_COMPONENT_MAX;
		     raw++) {
			c = *raw;
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
				c = '-';
			if (c == '-' && (!n || buf[n - 1] == '-'))
				continue;
			buf[n++] = c;
		}
	}
	while (n && buf[n - 1] == '-')	/* and never end with one */
		n--;
	buf[n] = '\0';

	if (!n)
		snprintf(buf, size, "%s", fallback && *fallback ?
			 fallback : "agent");
}

int fyai_branch_alloc_child(struct fyai_ctx *ctx, const char *parent,
			    const char *raw, unsigned int max_depth,
			    char *buf, size_t size)
{
	char slug[FYAI_BRANCH_COMPONENT_MAX + 1];
	struct fyai_branch b;
	unsigned int n;
	int len;

	fyai_error_check(ctx, parent && *parent, err_out,
			 "no parent branch for a sub-agent");
	fyai_error_check(ctx, fyai_branch_depth(parent) + 1 <= max_depth,
			 err_out,
			 "sub-agent nesting deeper than %u below '%s'",
			 max_depth, parent);

	fyai_branch_sanitize(raw, "agent", slug, sizeof(slug));

	for (n = 1; n < 10000; n++) {
		len = snprintf(buf, size, "%s/%s-%u", parent, slug, n);
		fyai_error_check(ctx, len >= 0 && len < (int)size, err_out,
				 "sub-agent branch name too long below '%s'",
				 parent);
		if (!fyai_branch_lookup(ctx->arena_branches, buf, &b))
			return 0;
	}
	fyai_error_check(ctx, false, err_out,
			 "too many '%s' sub-agent branches below '%s'",
			 slug, parent);

err_out:
	return -1;
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
	b->op = fy_invalid;
	b->from = fy_invalid;
	b->prev = fy_invalid;

	if (!fy_generic_is_mapping(entry))
		return false;

	b->entry = entry;
	b->config = fyai_branch_member(entry, "config");
	b->head = fyai_branch_member(entry, "head");
	b->description = fyai_branch_member(entry, "description");
	b->agent = fyai_branch_member(entry, "agent");
	b->op = fyai_branch_member(entry, "op");
	b->from = fyai_branch_member(entry, "from");
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
			     fy_generic op, fy_generic from, fy_generic prev)
{
	return fy_mapping(gb,
			  "config", fyai_generic_or_null(config),
			  "head", fyai_generic_or_null(head),
			  "created", fyai_generic_or_null(created),
			  "description", fyai_generic_or_null(description),
			  "agent", fyai_generic_or_null(agent),
			  "op", fyai_generic_or_null(op),
			  "from", fyai_generic_or_null(from),
			  "prev", fyai_generic_or_null(prev));
}

long long fyai_branch_turn_count(fy_generic head, long long limit)
{
	fy_generic cur;
	long long n;

	n = 0;
	fyai_turn_foreach(cur, head) {
		if (++n >= limit)
			break;
	}
	return n;
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

/* Cap on turns walked when counting or resolving a "~N" offset. */
#define FYAI_BRANCH_WALK_MAX 1000000
/* Cap on ref-log entries reported for one branch. */
#define FYAI_BRANCH_REFLOG_MAX 4096

/* Length of a branch entry's own ref-log chain, bounded so a corrupt or
 * cyclic prev link cannot spin here. */
static long long branch_chain_len(fy_generic entry)
{
	struct fyai_branch b;
	long long n;

	for (n = 0; n < FYAI_BRANCH_REFLOG_MAX; n++) {
		if (!fyai_branch_decode(entry, &b))
			break;
		entry = b.prev;
	}
	return n;
}

/*
 * Split a reference spec into its branch name and its suffix. Returns the
 * suffix kind: 0 none, '~' for "~N", '@' for "@{N}", -1 on a malformed spec.
 * @name receives the branch part, truncated into @buf.
 */
static int ref_split(const char *spec, char *buf, size_t size, long long *np)
{
	const char *sep, *num;
	char *end;
	size_t len;

	*np = 0;
	sep = strpbrk(spec, "~^@");
	len = sep ? (size_t)(sep - spec) : strlen(spec);
	if (len >= size)
		return -1;
	memcpy(buf, spec, len);
	buf[len] = '\0';
	if (!sep)
		return 0;

	/* Treat each '^' as one '~' step. */
	if (*sep == '^') {
		for (num = sep; *num == '^'; num++)
			(*np)++;
		return *num ? -1 : '~';
	}

	if (*sep == '~') {
		num = sep + 1;
	} else {
		if (sep[1] != '{')
			return -1;
		num = sep + 2;
	}
	if (!isdigit((unsigned char)*num))
		return -1;
	errno = 0;
	*np = strtoll(num, &end, 10);
	if (errno || *np < 0)
		return -1;
	if (*sep == '~')
		return *end ? -1 : '~';
	return (end[0] == '}' && !end[1]) ? '@' : -1;
}

/* True when @spec looks like a raw arena address rather than a branch name. */
static bool ref_looks_numeric(const char *spec)
{
	const char *p;

	for (p = spec; *p; p++) {
		if (!isxdigit((unsigned char)*p))
			return false;
	}
	return *spec != '\0';
}

int fyai_resolve_ref(struct fyai_ctx *ctx, const char *spec, fy_generic *headp)
{
	return fyai_resolve_ref_state(ctx, spec, headp, NULL);
}

int fyai_resolve_ref_state(struct fyai_ctx *ctx, const char *spec,
			   fy_generic *headp, fy_generic *configp)
{
	char parsed[256];
	struct fyai_branch b;
	fy_generic cur;
	const char *name;
	bool found, decoded;
	long long n, i;
	int kind;

	*headp = fy_invalid;
	if (configp)
		*configp = fy_invalid;
	fyai_error_check(ctx, spec && *spec, err_out, "empty reference");

	kind = ref_split(spec, parsed, sizeof(parsed), &n);
	fyai_error_check(ctx, kind >= 0, err_out,
			 "malformed reference '%s'; use <branch>, "
			 "<branch>~N or <branch>@{N}", spec);
	name = !strcmp(parsed, "HEAD") ?
	       fy_sprintfa("%s", fyai_ctx_branch(ctx)) : parsed;
	fyai_error_check(ctx, fyai_branch_name_valid(name), err_out,
			 "invalid branch name '%s'", name);

	found = fyai_branch_lookup(ctx->arena_branches, name, &b);
	if (!found) {
		/*
		 * A bare hex string that names no branch is almost certainly
		 * someone reaching for a commit id. There are none: gc
		 * relocates arena objects, so an address is not a stable name.
		 * Say that rather than just "no such branch".
		 */
		if (ref_looks_numeric(name))
			fyai_error(ctx, "'%s' is not a reference; fyai has no "
				   "numeric ids - use <branch>, <branch>~N or "
				   "<branch>@{N}", name);
		else
			fyai_error(ctx, "no such branch '%s'", name);
		goto err_out;
	}

	if (kind == '@') {
		for (i = 0; i < n; i++) {
			decoded = fyai_branch_decode(b.prev, &b);
			fyai_error_check(ctx, decoded, err_out,
					 "%s: ref log has no entry %lld",
					 name, n);
		}
		if (configp)
			*configp = b.config;
		*headp = b.head;
		return 0;
	}

	if (kind == '~') {
		cur = b.head;
		for (i = 0; i < n; i++) {
			fyai_error_check(ctx, fy_generic_is_valid(cur) &&
					 !fy_generic_is_null_type(cur),
					 err_out,
					 "%s: only %lld turns, cannot go "
					 "back %lld", name, i, n);
			cur = fy_get(cur, "previous");
		}
		if (fy_generic_is_valid(cur) && fy_generic_is_null_type(cur))
			cur = fy_invalid;
		if (configp)
			*configp = b.config;
		*headp = cur;
		return 0;
	}

	if (configp)
		*configp = b.config;
	*headp = b.head;
	return 0;

err_out:
	return -1;
}

/* Build the rows for `branch list` in the transient builder. */
static fy_generic branch_list_data(struct fyai_ctx *ctx, const char *under,
				   bool all)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct fyai_branch b;
	fy_generic out, name, entry;
	const char *nm, *active;
	size_t i, count;

	out = fy_seq_empty;
	active = fyai_ctx_branch(ctx);
	if (!fy_generic_is_mapping(ctx->arena_branches))
		return out;

	count = fy_generic_mapping_get_pair_count(ctx->arena_branches);
	for (i = 0; i < count; i++) {
		name = fy_get_key_at(ctx->arena_branches, i);
		/* Short strings require the address of the stored generic. */
		nm = fy_castp(&name, "");
		if (!nm || !*nm)
			continue;
		if (under && !fyai_branch_is_below(nm, under))
			continue;
		entry = fy_get_at(ctx->arena_branches, i);
		if (!fyai_branch_decode(entry, &b))
			continue;
		if (!all && fy_generic_is_valid(b.agent))
			continue;

		out = fy_append(gb, out,
			fy_mapping(gb,
				   "current", fy_value(gb, (bool)!strcmp(nm, active)),
				   "branch", name,
				   "turns", fy_value(gb,
					fyai_branch_turn_count(b.head,
						FYAI_BRANCH_WALK_MAX)),
				   "model", fy_value(gb,
					fy_get(b.config, "model", "-")),
				   "updated", fy_value(gb,
					fy_get(entry, "created", 0LL)),
				   "description", fy_value(gb,
					fy_generic_is_valid(b.description) ?
					fy_get(entry, "description", "") : "")));
	}
	return out;
}

int fyai_branch_list(struct fyai_ctx *ctx, const char *under, bool all)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic opts;

	opts = fy_mapping(gb,
		"title", "Branches",
		"empty", "no branches",
		"keys", fy_sequence(gb, "current", "branch", "turns", "model",
				    "updated", "description"),
		"columns", fy_mapping(gb,
			"current", fy_mapping(gb, "name", "*", "format", "mark"),
			"branch", fy_mapping(gb, "name", "Branch",
					     "align", "left"),
			"turns", fy_mapping(gb, "name", "Turns"),
			"model", fy_mapping(gb, "name", "Model", "align", "left"),
			"updated", fy_mapping(gb, "name", "Updated",
					      "align", "left"),
			"description", fy_mapping(gb, "name", "Description",
						  "align", "left")));
	return fyai_generic_to_markdown(ctx, opts, branch_list_data(ctx, under,
								    all));
}

int fyai_branch_show(struct fyai_ctx *ctx, const char *name)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct fyai_branch b;
	fy_generic data, opts;

	if (!name)
		name = fyai_ctx_branch(ctx);
	if (!fyai_branch_lookup(ctx->arena_branches, name, &b)) {
		fyai_error(ctx, "no such branch '%s'", name);
		return -1;
	}

	data = fy_mapping(gb,
		"branch", fy_value(gb, name),
		"current", fy_value(gb, (bool)!strcmp(name,
					fyai_ctx_branch(ctx))),
		"turns", fy_value(gb, fyai_branch_turn_count(b.head,
					FYAI_BRANCH_WALK_MAX)),
		"model", fy_value(gb, fy_get(b.config, "model", "-")),
		"created", fy_value(gb, fy_get(b.entry, "created", 0LL)),
		"description", fy_value(gb, fy_get(b.entry, "description", "-")),
		"agent", fy_value(gb, fy_get(b.agent, "persona", "-")),
		"reflog_entries", fy_value(gb, branch_chain_len(b.entry)));
	opts = fy_mapping(gb, "title", "Branch", "key_header", "Field",
			  "value_header", "Value");
	return fyai_generic_to_markdown(ctx, opts, data);
}

/* Publish changes to branches other than the active branch. */
static int branches_publish(struct fyai_ctx *ctx, fy_generic branches)
{
	if (!fy_generic_is_valid(branches)) {
		fyai_error(ctx, "cannot build the branch table");
		return -1;
	}
	ctx->arena_branches = branches;
	return fyai_publish_state(ctx);
}

static size_t branch_parent_len(const char *name)
{
	const char *slash;

	slash = strrchr(name, '/');
	return slash ? (size_t)(slash - name) : 0;
}

int fyai_branch_create(struct fyai_ctx *ctx, const char *name,
		       const char *start, const char *description,
		       bool switch_to)
{
	struct fyai_branch b, cur;
	fy_generic head, entry, branches, desc, config;
	bool found;
	int rc;

	fyai_error_check(ctx, fyai_branch_name_valid(name), err_out,
			 "invalid branch name '%s'", name ? name : "");
	found = fyai_branch_lookup(ctx->arena_branches, name, &b);
	fyai_error_check(ctx, !found, err_out,
			 "branch '%s' already exists", name);

	/* A start point supplies both the conversation and configuration. */
	head = fy_invalid;
	config = fy_invalid;
	if (start) {
		rc = fyai_resolve_ref_state(ctx, start, &head, &config);
		fyai_error_check(ctx, !rc, err_out,
				 "could not resolve start point '%s'", start);
	} else {
		found = fyai_branch_lookup(ctx->arena_branches,
					   fyai_ctx_branch(ctx), &cur);
		fyai_error_check(ctx, found, err_out,
				 "current branch '%s' is missing",
				 fyai_ctx_branch(ctx));
		head = cur.head;
		config = cur.config;
	}

	desc = description ? fy_value(ctx->gb, description) : fy_invalid;

	entry = fyai_branch_build(ctx->gb, config, head,
				  fy_value(ctx->gb, (long long)
					   fyai_branch_timestamp()),
				  desc, fy_invalid,
				  fy_value(ctx->gb, FYAI_BRANCH_OP_CREATE),
				  fy_invalid, fy_invalid);
	branches = fyai_branches_set(ctx->gb, ctx->arena_branches, name, entry);
	rc = branches_publish(ctx, branches);
	fyai_error_check(ctx, !rc, err_out,
			 "could not publish branch '%s'", name);

	if (switch_to)
		return fyai_branch_checkout(ctx, name, false, NULL);
	printf("created branch %s\n", name);
	return 0;

err_out:
	return -1;
}

int fyai_branch_delete(struct fyai_ctx *ctx, const char *name, bool force)
{
	struct fyai_branch b;
	fy_generic branches, key, entry;
	const char *nm, *newname;
	size_t i, count, plen;
	bool found;
	int rc;

	fyai_error_check(ctx, strcmp(name, fyai_ctx_branch(ctx)), err_out,
			 "cannot delete the current branch '%s'", name);
	found = fyai_branch_lookup(ctx->arena_branches, name, &b);
	fyai_error_check(ctx, found, err_out, "no such branch '%s'", name);
	fyai_error_check(ctx, force || fy_generic_is_invalid(b.head), err_out,
			 "branch '%s' has turns; use --force to delete it",
			 name);

	/* Children move to the deleted branch's parent, or to the empty root. */
	plen = branch_parent_len(name);
	count = fy_generic_mapping_get_pair_count(ctx->arena_branches);
	for (i = 0; i < count; i++) {
		key = fy_get_key_at(ctx->arena_branches, i);
		nm = fy_castp(&key, "");
		if (!nm || !*nm || !strcmp(nm, name) ||
		    !fyai_branch_is_below(nm, name))
			continue;
		newname = plen ?
			  fy_sprintfa("%.*s%s", (int)plen, name,
				      nm + strlen(name)) :
			  fy_sprintfa("%s", nm + strlen(name) + 1);
		found = fyai_branch_lookup(ctx->arena_branches, newname, &b);
		fyai_error_check(ctx, !found, err_out,
				 "cannot reparent '%s': branch '%s' exists",
				 nm, newname);
	}

	branches = ctx->arena_branches;
	for (i = 0; i < count; i++) {
		key = fy_get_key_at(ctx->arena_branches, i);
		nm = fy_castp(&key, "");
		if (!nm || !*nm || !fyai_branch_is_below(nm, name))
			continue;
		entry = fy_get_at(ctx->arena_branches, i);
		branches = fyai_branches_set(ctx->gb, branches, nm, fy_invalid);
		if (!strcmp(nm, name))
			continue;
		newname = plen ?
			  fy_sprintfa("%.*s%s", (int)plen, name,
				      nm + strlen(name)) :
			  fy_sprintfa("%s", nm + strlen(name) + 1);
		found = fyai_branch_decode(entry, &b);
		fyai_error_check(ctx, found, err_out,
				 "could not decode child branch '%s'", nm);
		entry = fyai_branch_build(ctx->gb, b.config, b.head,
				fy_value(ctx->gb, (long long)
					 fyai_branch_timestamp()),
				b.description, b.agent,
				fy_value(ctx->gb, FYAI_BRANCH_OP_RENAME),
				fy_value(ctx->gb, nm), entry);
		fyai_error_check(ctx, fy_generic_is_valid(entry), err_out,
				 "could not record reparenting of '%s'", nm);
		branches = fyai_branches_set(ctx->gb, branches, newname, entry);
		if (!strcmp(nm, fyai_ctx_branch(ctx))) {
			rc = fyai_ctx_set_branch(ctx, newname);
			fyai_error_check(ctx, !rc, err_out,
					 "could not reparent current branch");
		}
		if (ctx->head_branch && !strcmp(nm, ctx->head_branch)) {
			free(ctx->head_branch);
			ctx->head_branch = strdup(newname);
			fyai_error_check(ctx, ctx->head_branch, err_out,
					 "out of memory reparenting HEAD");
		}
	}
	fyai_error_check(ctx, fy_generic_mapping_get_pair_count(branches),
			 err_out, "cannot delete the last branch");
	rc = branches_publish(ctx, branches);
	fyai_error_check(ctx, !rc, err_out,
			 "could not publish deletion of '%s'", name);
	printf("deleted branch %s\n", name);
	return 0;

err_out:
	return -1;
}

int fyai_branch_rename(struct fyai_ctx *ctx, const char *from, const char *to)
{
	struct fyai_branch b;
	fy_generic branches, key, entry;
	char newname[512];
	const char *nm;
	size_t i, count;
	bool found, renamed_current;
	int rc;

	found = fyai_branch_lookup(ctx->arena_branches, from, &b);
	fyai_error_check(ctx, found, err_out, "no such branch '%s'", from);
	fyai_error_check(ctx, fyai_branch_name_valid(to), err_out,
			 "invalid branch name '%s'", to);
	found = fyai_branch_lookup(ctx->arena_branches, to, &b);
	fyai_error_check(ctx, !found, err_out,
			 "branch '%s' already exists", to);

	/* Check every renamed child before changing the mapping. */
	count = fy_generic_mapping_get_pair_count(ctx->arena_branches);
	for (i = 0; i < count; i++) {
		key = fy_get_key_at(ctx->arena_branches, i);
		nm = fy_castp(&key, "");
		if (!nm || !*nm || !fyai_branch_is_below(nm, from))
			continue;
		if (snprintf(newname, sizeof(newname), "%s%s", to,
			     nm + strlen(from)) >= (int)sizeof(newname)) {
			fyai_error_check(ctx, false, err_out,
					 "renamed branch name too long");
		}
		found = fyai_branch_lookup(ctx->arena_branches, newname, &b);
		fyai_error_check(ctx, !found, err_out,
				 "branch '%s' already exists", newname);
	}

	branches = ctx->arena_branches;
	renamed_current = false;
	for (i = 0; i < count; i++) {
		key = fy_get_key_at(ctx->arena_branches, i);
		nm = fy_castp(&key, "");
		if (!nm || !*nm || !fyai_branch_is_below(nm, from))
			continue;
		snprintf(newname, sizeof(newname), "%s%s", to,
			 nm + strlen(from));
		entry = fy_get_at(ctx->arena_branches, i);
		/* Record the previous name in the branch ref log. */
		if (strcmp(nm, fyai_ctx_branch(ctx))) {
			fyai_branch_decode(entry, &b);
			entry = fyai_branch_build(ctx->gb, b.config, b.head,
					fy_value(ctx->gb, (long long)
						 fyai_branch_timestamp()),
					b.description, b.agent,
					fy_value(ctx->gb, FYAI_BRANCH_OP_RENAME),
					fy_value(ctx->gb, nm), entry);
			if (!fy_generic_is_valid(entry)) {
				fyai_error(ctx, "could not record the rename "
					   "of '%s'", nm);
				return -1;
			}
		}
		branches = fyai_branches_set(ctx->gb, branches, nm, fy_invalid);
		branches = fyai_branches_set(ctx->gb, branches, newname, entry);
		if (!strcmp(nm, fyai_ctx_branch(ctx))) {
			renamed_current = true;
			rc = fyai_ctx_set_branch(ctx, newname);
			fyai_error_check(ctx, !rc, err_out,
					 "could not rename current branch");
		}
		if (ctx->head_branch && !strcmp(nm, ctx->head_branch)) {
			free(ctx->head_branch);
			ctx->head_branch = strdup(newname);
			fyai_error_check(ctx, ctx->head_branch, err_out,
					 "out of memory renaming HEAD");
		}
	}
	/*
	 * When the current branch moved, the publish that follows rebuilds its
	 * entry under the new name; re-point the ref-log link at the entry now
	 * carrying it, or the chain would restart.
	 */
	if (renamed_current) {
		fyai_branch_lookup(branches, fyai_ctx_branch(ctx), &b);
		ctx->branch_prev = b.entry;
		fyai_branch_op_set(ctx, FYAI_BRANCH_OP_RENAME, from);
	}
	rc = branches_publish(ctx, branches);
	fyai_error_check(ctx, !rc, err_out,
			 "could not publish rename of '%s'", from);
	printf("renamed %s to %s\n", from, to);
	return 0;

err_out:
	return -1;
}

int fyai_branch_adopt(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_branch b;
	int rc;

	fyai_error_check(ctx, fyai_branch_name_valid(name), err_out,
			 "invalid branch name '%s'", name ? name : "");
	rc = fyai_branch_lookup(ctx->arena_branches, name, &b);
	fyai_error_check(ctx, rc, err_out, "no such branch '%s'", name);
	rc = fyai_ctx_checkout(ctx, name);
	fyai_error_check(ctx, !rc, err_out,
			 "could not adopt branch '%s'", name);

	ctx->last_message = b.head;
	ctx->arena_config = b.config;
	ctx->branch_desc = b.description;
	ctx->branch_agent = b.agent;
	ctx->branch_prev = b.entry;
	return 0;

err_out:
	return -1;
}

int fyai_branch_checkout(struct fyai_ctx *ctx, const char *name, bool create,
			 const char *start)
{
	struct fyai_branch b;
	bool exists;
	int rc;

	fyai_error_check(ctx, fyai_branch_name_valid(name), err_out,
			 "invalid branch name '%s'", name ? name : "");
	exists = fyai_branch_lookup(ctx->arena_branches, name, &b);
	fyai_error_check(ctx, exists || create, err_out,
			 "no such branch '%s'; use -b to create it", name);
	if (!exists && create) {
		rc = fyai_branch_create(ctx, name, start, NULL, false);
		fyai_error_check(ctx, !rc, err_out,
				 "could not create branch '%s'", name);
		exists = fyai_branch_lookup(ctx->arena_branches, name, &b);
		fyai_error_check(ctx, exists, err_out,
				 "created branch '%s' is missing", name);
	} else if (start) {
		fyai_error_check(ctx, create, err_out,
				 "a start point needs -b to make '%s'", name);
		fyai_error_check(ctx, false, err_out,
				 "branch '%s' already exists", name);
	}
	rc = fyai_branch_adopt(ctx, name);
	fyai_error_check(ctx, !rc, err_out,
			 "could not check out branch '%s'", name);

	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_CHECKOUT, NULL);
	rc = fyai_publish_state(ctx);
	fyai_error_check(ctx, !rc, err_out,
			 "could not publish checkout of '%s'", name);
	printf("switched to branch %s\n", name);
	return 0;

err_out:
	return -1;
}

int fyai_branch_reset(struct fyai_ctx *ctx, const char *spec)
{
	fy_generic head;
	long long turns;

	if (fyai_resolve_ref(ctx, spec, &head))
		return -1;

	/* The previous head remains available as "<branch>@{1}". */
	ctx->last_message = head;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_RESET, NULL);
	if (fyai_publish_state(ctx))
		return -1;

	turns = fyai_branch_turn_count(head, FYAI_BRANCH_WALK_MAX);
	printf("%s is now at %s (%lld turn%s); the previous head is %s@{1}\n",
	       fyai_ctx_branch(ctx), spec, turns, turns == 1 ? "" : "s",
	       fyai_ctx_branch(ctx));
	return 0;
}

int fyai_branch_describe(struct fyai_ctx *ctx, const char *name,
			 const char *description)
{
	struct fyai_branch b;
	fy_generic entry, branches, desc;

	if (!name)
		name = fyai_ctx_branch(ctx);
	if (!fyai_branch_lookup(ctx->arena_branches, name, &b)) {
		fyai_error(ctx, "no such branch '%s'", name);
		return -1;
	}

	desc = description && *description ?
	       fy_value(ctx->gb, description) : fy_invalid;

	if (!strcmp(name, fyai_ctx_branch(ctx))) {
		ctx->branch_desc = desc;
		fyai_branch_op_set(ctx, FYAI_BRANCH_OP_DESCRIBE, NULL);
		return fyai_publish_state(ctx);
	}

	entry = fyai_branch_build(ctx->gb, b.config, b.head,
				  fy_value(ctx->gb, (long long)
					   fyai_branch_timestamp()),
				  desc, b.agent,
				  fy_value(ctx->gb, FYAI_BRANCH_OP_DESCRIBE),
				  fy_invalid, b.entry);
	branches = fyai_branches_set(ctx->gb, ctx->arena_branches, name, entry);
	return branches_publish(ctx, branches);
}
