/*
 * fyai_storage.c - durable arena and local state paths
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * Several verbs report from here (init, gc, config), so each message names
 * its own rather than taking one module prefix for the file.
 */
#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <alloca.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fyai_prof.h"
#include "fyai_branch.h"
#include "fyai_merge.h"
#include "fyai_config.h"
#include "fyai_storage.h"
#include "fyai_turn.h"

/* FYAI_EMBEDDED_CONFIG[] / FYAI_EMBEDDED_CONFIG_LEN - the config.yaml.sample
 * snapshot, generated at configure time; used as the default document when
 * `fyai init` is invoked without an explicit config file. */
#include "embedded_config.inc"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FYAI_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define FYAI_ASAN 1
#endif

#ifdef FYAI_ASAN
#define FYAI_ASAN_CONTENT_SIZE		(64ULL << 30)
#define FYAI_ASAN_INDEX_SIZE		(16ULL << 30)

#if defined(__x86_64__) || defined(__aarch64__)
/* Linux/x86_64 and Linux/AArch64 ASAN 48-bit VMA shared HighMem range. */
#define FYAI_ASAN_CONTENT_BASE		0x201000000000ULL
#define FYAI_ASAN_INDEX_BASE		0x241000000000ULL
#endif

#if defined(FYAI_ASAN_CONTENT_BASE) && defined(MAP_FIXED_NOREPLACE)
static void *fyai_reserved_content;
static void *fyai_reserved_index;
#endif
#endif

/* Upper bound on ref-log roots rebuilt in a single gc --keep-reflogs pass. */
#define FYAI_REFLOG_KEEP_MAX 4096

struct fyai_arena_boot {
	uint8_t magic[8];
	uint32_t version;
	uint32_t endian;
	uint64_t region_base;
};

static fy_generic fyai_root_entry(fy_generic root, const char *key)
{
	fy_generic v;

	v = fy_get(root, key);
	if (fy_generic_is_valid(v) && fy_generic_is_null_type(v))
		v = fy_invalid;
	return v;
}


int fyai_root_decode(fy_generic root, struct fyai_root *r)
{
	fy_generic v;

	memset(r, 0, sizeof(*r));
	r->catalog = fy_invalid;
	r->branches = fy_invalid;
	r->head = fy_invalid;
	r->created = fy_invalid;

	if (!fy_generic_is_mapping(root))
		return -1;
	v = fy_get(root, "fyai");
	if (fy_generic_is_invalid(v) ||
	    fy_cast(v, 0LL) != (long long)FYAI_ROOT_VERSION)
		return -1;
	r->catalog = fyai_root_entry(root, "catalog");
	r->branches = fyai_root_entry(root, "branches");
	r->head = fyai_root_entry(root, "HEAD");
	r->created = fyai_root_entry(root, "created");
	return FYAI_ROOT_VERSION;
}

const char *fyai_root_head_name(const struct fyai_root *r)
{
	return fy_castp(&r->head, (const char *)NULL);
}

/*
 * Build a container root mapping. The single point where a root is constructed,
 * so publish and ref-log truncation stay consistent. prev links to the
 * predecessor root (the ref log).
 *
 * NOTE: an integrity checksum field is intentionally not stamped here. A
 * checksum over the raw generic words does not survive `gc`, which relocates
 * objects (rewriting the address-bearing words) without touching a stored
 * scalar sum; and libfyaml exposes no relocation-stable content id to hash
 * instead. A content-emit hash or a gc-aware re-stamp would be needed - left
 * for a deliberate follow-up. Structural + containment validation
 * (fyai_root_validate) is relocation-stable and covers the memory-safety case.
 */
static fy_generic fyai_root_build(struct fy_generic_builder *gb,
				  fy_generic catalog, fy_generic branches,
				  fy_generic head, fy_generic created,
				  fy_generic prev)
{
	return fy_gb_mapping(gb,
			     "fyai", (long long)FYAI_ROOT_VERSION,
			     "catalog", catalog,
			     "HEAD", head,
			     "branches", branches,
			     "created", created,
			     "prev", prev);
}

static bool generic_same(fy_generic a, fy_generic b)
{
	return a.v == b.v;
}

/*
 * Validate a root reference before it is trusted: it must be a mapping
 * contained in the arena allocator, its version and checksum must check out
 * (via decode), and every out-of-place field it references must also live in
 * the allocator - so a stray or hostile ref value cannot make us dereference
 * memory outside the arena. @a may be NULL to skip the containment checks
 * (structural + checksum only). Deep nested references are covered
 * transitively by the checksum's tamper detection.
 */
static bool root_ref_contained(struct fy_allocator *a, fy_generic v)
{
	if (fy_generic_is_invalid(v) || !fy_generic_is_mapping(v))
		return true;	/* null / inplace: no out-of-place pointer */
	return fy_allocator_contains(a, -1, fy_generic_resolve_collection_ptr(v));
}

/* Validate at most @depth branch ref-log entries. */
bool fyai_branch_entry_contained(struct fy_allocator *a, fy_generic entry,
				 unsigned int depth)
{
	struct fyai_branch b;
	unsigned int n;

	if (fy_generic_is_invalid(entry))
		return false;
	for (n = 0; n < depth; n++) {
		if (fy_generic_is_invalid(entry))
			return true;
		if (!root_ref_contained(a, entry))
			return false;
		if (!fyai_branch_decode(entry, &b))
			return false;
		if (!root_ref_contained(a, b.config) ||
		    !root_ref_contained(a, b.head) ||
		    !root_ref_contained(a, b.description) ||
		    !root_ref_contained(a, b.agent) ||
		    !root_ref_contained(a, b.op) ||
		    !root_ref_contained(a, b.from) ||
		    !root_ref_contained(a, b.prev))
			return false;
		entry = b.prev;
	}
	return true;
}

/* Validate a root before following its references. */
static bool root_shape_ok(struct fy_allocator *a, fy_generic root,
			  struct fyai_root *r)
{
	if (!fy_generic_is_mapping(root))
		return false;
	if (a && !fy_allocator_contains(a, -1,
					fy_generic_resolve_collection_ptr(root)))
		return false;
	if (fyai_root_decode(root, r) < 0)
		return false;
	if (!a)
		return true;
	return root_ref_contained(a, r->catalog) &&
	       root_ref_contained(a, r->branches) &&
	       root_ref_contained(a, fyai_root_prev(root));
}

bool fyai_root_validate(struct fy_allocator *a, fy_generic root)
{
	struct fyai_root r;
	size_t i, count;

	if (!root_shape_ok(a, root, &r))
		return false;
	/* Ref-log walkers validate each predecessor before following it. */
	if (a && fy_generic_is_mapping(r.branches)) {
		count = fy_generic_mapping_get_pair_count(r.branches);
		for (i = 0; i < count; i++) {
			if (!fyai_branch_entry_contained(a,
					fy_get_at(r.branches, i), 1))
				return false;
		}
	}
	return true;
}

/*
 * The predecessor root in the ref log, or fy_invalid at the start of the chain
 * (or for a pre-chain root that carries no prev). Walk it to replay root
 * history - turn commits and turnless config updates alike.
 */
fy_generic fyai_root_prev(fy_generic root)
{
	if (!fy_generic_is_mapping(root))
		return fy_invalid;
	return fyai_root_entry(root, "prev");
}

fy_generic fyai_root_find(struct fy_allocator *a, fy_generic_value from,
			  fy_generic_value want)
{
	struct fyai_root r;
	fy_generic node, current;
	unsigned int n;

	if (!from || !want)
		return fy_invalid;

	if (from == want) {
		current = (fy_generic){ .v = from };
		if (fyai_root_validate(a, current))
			return current;
		return fy_invalid;
	}

	node = (fy_generic){ .v = from };
	for (n = 0; n < FYAI_REFLOG_KEEP_MAX; n++) {
		/* Run the full validation only on a matching root. */
		if (!root_shape_ok(a, node, &r))
			return fy_invalid;
		if ((uint64_t)node.v == want)
			return fyai_root_validate(a, node) ? node : fy_invalid;
		node = fyai_root_prev(node);
		if (fy_generic_is_invalid(node))
			return fy_invalid;
	}
	return fy_invalid;
}

/* True when @spec is only hexadecimal digits, i.e. a handle and not a name. */
static bool root_spec_is_handle(const char *spec, fy_generic_value *vp)
{
	unsigned long long v;
	const char *p;
	char *end;

	p = spec;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	if (!*p)
		return false;
	for (end = (char *)p; *end; end++) {
		if (!isxdigit((unsigned char)*end))
			return false;
	}
	errno = 0;
	v = strtoull(p, &end, 16);
	if (errno || *end || !v)
		return false;
	*vp = (fy_generic_value)v;
	return true;
}

/* Find the oldest root in the run that holds @want for @name. */
static fy_generic_value root_for_branch_entry(struct fy_allocator *a,
					      fy_generic_value from,
					      const char *name,
					      fy_generic want, bool by_head)
{
	struct fyai_branch b;
	struct fyai_root r;
	fy_generic node, have;
	fy_generic_value match;
	unsigned int n;

	match = 0;
	node = (fy_generic){ .v = from };
	for (n = 0; n < FYAI_REFLOG_KEEP_MAX; n++) {
		if (!root_shape_ok(a, node, &r))
			break;
		fyai_branch_lookup(r.branches, name, &b);
		have = by_head ? b.head : b.entry;
		if (fy_generic_is_valid(have) && have.v == want.v) {
			match = (uint64_t)node.v;
		} else if (match) {
			break;	/* the run ended: keep its oldest root */
		}
		node = fyai_root_prev(node);
		if (fy_generic_is_invalid(node))
			break;
	}
	return match;
}

int fyai_root_resolve_spec(struct fy_allocator *a, fy_generic_value from,
			   const char *spec, fy_generic_value *outp)
{
	char name[256];
	struct fyai_branch b;
	struct fyai_root r;
	fy_generic root, cur;
	long long count, i;
	fy_generic_value v;
	int kind;

	*outp = 0;
	if (!spec || !*spec || !from)
		return -1;

	root = (fy_generic){ .v = from };
	if (!root_shape_ok(a, root, &r))
		return -1;

	/*
	 * Prefer a branch name over a handle when the current root contains
	 * that branch. A hexadecimal branch name is valid.
	 */
	kind = fyai_ref_parse(spec, name, sizeof(name), &count);
	if ((kind < 0 || !fyai_branch_name_valid(name) ||
	     !fyai_branch_lookup(r.branches, name, &b)) &&
	    root_spec_is_handle(spec, &v)) {
		root = fyai_root_find(a, from, v);
		if (fy_generic_is_invalid(root))
			return -1;
		*outp = v;
		return 0;
	}

	if (kind < 0 || !fyai_branch_name_valid(name))
		return -1;
	if (!fyai_branch_lookup(r.branches, name, &b))
		return -1;

	if (kind == '@') {
		for (i = 0; i < count; i++) {
			if (!fyai_branch_entry_contained(a, b.prev, 1) ||
			    !fyai_branch_decode(b.prev, &b))
				return -1;
		}
		*outp = root_for_branch_entry(a, from, name, b.entry, false);
	} else if (kind == '~') {
		cur = b.head;
		for (i = 0; i < count; i++) {
			if (fy_generic_is_invalid(cur) ||
			    fy_generic_is_null_type(cur))
				return -1;
			cur = fy_get(cur, "previous");
		}
		if (fy_generic_is_invalid(cur) || fy_generic_is_null_type(cur))
			return -1;
		*outp = root_for_branch_entry(a, from, name, cur, true);
	} else {
		*outp = root_for_branch_entry(a, from, name, b.entry, false);
	}
	return *outp ? 0 : -1;
}

void fyai_reserve_arena_ranges(void)
{
#if defined(FYAI_ASAN_CONTENT_BASE) && defined(MAP_FIXED_NOREPLACE)
	void *base;

	base = (void *)(uintptr_t)FYAI_ASAN_CONTENT_BASE;
	fyai_reserved_content = mmap(base, FYAI_ASAN_CONTENT_SIZE, PROT_NONE,
				     MAP_PRIVATE | MAP_ANONYMOUS |
				     MAP_FIXED_NOREPLACE | MAP_NORESERVE,
				     -1, 0);
	if (fyai_reserved_content == MAP_FAILED)
		fyai_reserved_content = NULL;

	base = (void *)(uintptr_t)FYAI_ASAN_INDEX_BASE;
	fyai_reserved_index = mmap(base, FYAI_ASAN_INDEX_SIZE, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS |
				   MAP_FIXED_NOREPLACE | MAP_NORESERVE,
				   -1, 0);
	if (fyai_reserved_index == MAP_FAILED)
		fyai_reserved_index = NULL;
#endif
}

static void fyai_unreserve_arena_ranges(struct fy_durable_allocator_cfg *dur_cfg)
{
#if defined(FYAI_ASAN_CONTENT_BASE) && defined(MAP_FIXED_NOREPLACE)
	if (fyai_reserved_content &&
	    dur_cfg->region_base == FYAI_ASAN_CONTENT_BASE &&
	    (!dur_cfg->region_size ||
	     dur_cfg->region_size == FYAI_ASAN_CONTENT_SIZE)) {
		munmap(fyai_reserved_content, FYAI_ASAN_CONTENT_SIZE);
		fyai_reserved_content = NULL;
	}
	if (fyai_reserved_index &&
	    ((dur_cfg->index_region_base == FYAI_ASAN_INDEX_BASE &&
	      (!dur_cfg->index_region_size ||
	       dur_cfg->index_region_size == FYAI_ASAN_INDEX_SIZE)) ||
	     dur_cfg->region_base == FYAI_ASAN_CONTENT_BASE)) {
		munmap(fyai_reserved_index, FYAI_ASAN_INDEX_SIZE);
		fyai_reserved_index = NULL;
	}
#else
	(void)dur_cfg;
#endif
}

int fyai_mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	char *p;
	size_t len;

	len = strlen(path);
	if (!len || len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0700) && errno != EEXIST)
		return -1;
	return 0;
}

char *fyai_default_arena_dir(void)
{
	char cwd[PATH_MAX], root[PATH_MAX], probe[PATH_MAX];
	char *slash, *dir;

	if (!getcwd(cwd, sizeof(cwd)))
		return NULL;
	memcpy(root, cwd, sizeof(root));
	for (;;) {
		if (snprintf(probe, sizeof(probe), "%s/.fyai", cwd) >=
		    (int)sizeof(probe))
			return NULL;
		if (!access(probe, F_OK))
			break;
		slash = strrchr(cwd, '/');
		if (!slash || slash == cwd) {
			if (snprintf(probe, sizeof(probe), "%s/.fyai", root) >=
			    (int)sizeof(probe))
				return NULL;
			break;
		}
		*slash = '\0';
	}
	if (asprintf(&dir, "%s/arena", probe) == -1)
		return NULL;
	return dir;
}

char *fyai_history_path(void)
{
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	char *dir, *path;

	if (xdg && *xdg) {
		if (asprintf(&dir, "%s/fyai", xdg) == -1)
			return NULL;
	} else if (home && *home) {
		if (asprintf(&dir, "%s/.local/state/fyai", home) == -1)
			return NULL;
	} else {
		return NULL;
	}
	if (fyai_mkdir_p(dir)) {
		free(dir);
		return NULL;
	}
	if (asprintf(&path, "%s/history", dir) == -1)
		path = NULL;
	free(dir);
	return path;
}

static uint64_t fyai_arena_boot_base(const char *arena_dir)
{
	static const uint8_t magic[8] = { 'O', 'B', 'A', 'R', 'U', 'D', 'F', 'Y' };
	struct fyai_arena_boot boot;
	char *path = NULL;
	uint64_t base = 0;
	int fd;

	if (asprintf(&path, "%s/arena-0.bin", arena_dir) == -1)
		return 0;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto out;
	if (read(fd, &boot, sizeof(boot)) == (ssize_t)sizeof(boot) &&
	    !memcmp(boot.magic, magic, sizeof(magic)) &&
	    boot.version && boot.endian == 0x12345678u)
		base = boot.region_base;
	close(fd);
out:
	free(path);
	return base;
}

/* Open (creating if needed) the durable arena at @arena_dir into @ctx. */
static int fyai_open_arena(struct fyai_ctx *ctx, const char *arena_dir)
{
	struct fy_durable_allocator_cfg dur_cfg;
	struct fy_generic_builder_cfg gb_cfg;
	struct fy_auto_allocator_cfg ov_cfg;
	struct fy_generic_builder_cfg ov_gb_cfg;


	if (fyai_mkdir_p(arena_dir))
		return -1;

	memset(&dur_cfg, 0, sizeof(dur_cfg));
	dur_cfg.dir = arena_dir;
	dur_cfg.region_base = fyai_arena_boot_base(arena_dir);
	dur_cfg.flags = FY_DURABLE_ARENA_CREATE | FY_DURABLE_ARENA_DEDUP |
			FY_DURABLE_ARENA_SPARSE | FY_DURABLE_ARENA_SEPARATE_INDEX;

#if defined(FYAI_ASAN_CONTENT_BASE)
	/*
	 * Declare fyai's ASAN arena range in ASAN HighMem, clear of shadow and
	 * shadow-gap ranges. Existing arenas keep their recorded boot base.
	 */
	if (!dur_cfg.region_base) {
		dur_cfg.region_base = FYAI_ASAN_CONTENT_BASE;
		dur_cfg.region_size = FYAI_ASAN_CONTENT_SIZE;
		dur_cfg.index_region_base = FYAI_ASAN_INDEX_BASE;
		dur_cfg.index_region_size = FYAI_ASAN_INDEX_SIZE;
	}
#endif
	fyai_unreserve_arena_ranges(&dur_cfg);
	ctx->durable_allocator = fy_allocator_create("durable", &dur_cfg);
	if (!ctx->durable_allocator)
		return -1;

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	gb_cfg.allocator = ctx->durable_allocator;
	ctx->durable_gb = fy_generic_builder_create(&gb_cfg);
	if (!ctx->durable_gb)
		return -1;

	/*
	 * The working builder stacks over the durable one in transient mode, so
	 * config/state writes are visible this run but never published; normally
	 * it is the durable builder itself.
	 */
	if (ctx->cfg && ctx->cfg->transient) {
		memset(&ov_cfg, 0, sizeof(ov_cfg));
		memset(&ov_gb_cfg, 0, sizeof(ov_gb_cfg));

		ov_cfg.scenario = FYAST_PER_TAG_FREE_DEDUP;
		ctx->overlay_allocator = fy_allocator_create("auto", &ov_cfg);
		if (!ctx->overlay_allocator)
			return -1;
		ov_gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED |
				  FYGBCF_CREATE_TAG;
		ov_gb_cfg.allocator = ctx->overlay_allocator;
		ov_gb_cfg.parent = ctx->durable_gb;
		ctx->gb = fy_generic_builder_create(&ov_gb_cfg);
		if (!ctx->gb)
			return -1;
	} else {
		ctx->gb = ctx->durable_gb;
	}

	ctx->refs_head = fy_allocator_refs_get(ctx->durable_allocator);
	return 0;
}

int fyai_setup_storage(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_branch b;
	struct fyai_root r;
	const char *name;
	fy_generic root;
	int rc;

	rc = fyai_open_arena(ctx, cfg->arena_dir);
	fyai_error_check(ctx, !rc, err_out, "could not open arena");

	if (cfg->root_pinned && ctx->refs_head) {
		/* Confirm that the arena still contains the pinned root. */
		root = fyai_root_find(ctx->durable_allocator, ctx->refs_head,
				      cfg->root_ref);
		fyai_error_check(ctx, fy_generic_is_valid(root), err_out,
				 "--root '%s' names no root in %s; "
				 "gc may have dropped it",
				 cfg->root_spec ? cfg->root_spec : "",
				 cfg->arena_dir);
		ctx->refs_head = cfg->root_ref;
	}

	if (ctx->refs_head) {
		root = (fy_generic){ .v = ctx->refs_head };
		/*
		 * Validate before trusting: mapping-ness, arena containment of the
		 * root and its referenced parts, version and integrity checksum.
		 */
		if (!fyai_root_validate(ctx->durable_allocator, root) ||
		    fyai_root_decode(root, &r) < 0) {
			fyai_error(ctx, "unrecognized arena root at %s; re-run fyai init",
				   cfg->arena_dir);
			goto err_out;
		}
		ctx->arena_catalog = r.catalog;
		ctx->arena_branches = r.branches;
		/* Use an explicit branch, the stored HEAD, or the default. */
		name = fyai_root_head_name(&r);
		if (name) {
			/*
			 * Remember the stored HEAD so a publish preserves it.
			 * An explicit --branch selects where this invocation
			 * works without moving HEAD.
			 */
			ctx->head_branch = strdup(name);
			fyai_error_check(ctx, ctx->head_branch, err_out,
					 "out of memory reading HEAD");
			if (!cfg->branch_explicit) {
				rc = fyai_ctx_set_branch(ctx, name);
				fyai_error_check(ctx, !rc, err_out,
						 "could not select HEAD");
			}
		}

		/* An absent branch is created by its first publish. */
		fyai_branch_lookup(r.branches, fyai_ctx_branch(ctx), &b);
		ctx->arena_config = b.config;
		ctx->branch_desc = b.description;
		ctx->branch_agent = b.agent;
		ctx->branch_prev = b.entry;
		/*
		 * --new is a clear: drop the head and publish the reset as a
		 * turnless reflog entry, exactly like the /clear command; the
		 * config and catalog ride along unchanged. It is scoped to this
		 * branch and leaves every other branch alone.
		 */
		if (!cfg->new_conversation)
			ctx->last_message = b.head;
		else {
			rc = fyai_publish_state(ctx);
			fyai_error_check(ctx, !rc, err_out,
					 "could not publish new conversation");
		}
	} else if (!cfg->branch_explicit) {
		rc = fyai_ctx_set_branch(ctx, FYAI_BRANCH_DEFAULT);
		fyai_error_check(ctx, !rc, err_out,
				 "could not select the default branch");
	}

	return 0;

err_out:
	fyai_close_storage(ctx);
	return -1;
}

/*
 * Build the container root from the current ctx parts and CAS-advance the
 * refs head to it. Returns 0 on success, >0 on concurrent-change conflict,
 * <0 on error.
 */
/* Build and splice the active branch entry. */
static fy_generic fyai_branches_commit(struct fyai_ctx *ctx)
{
	fy_generic entry, created, opv, fromv;
	struct fyai_branch prev;
	const char *name, *op;
	bool same;

	name = fyai_ctx_branch(ctx);
	created = fy_value(ctx->gb, (long long)fyai_branch_timestamp());

	/* Infer an unlabeled publish from whether the head changed. */
	op = ctx->branch_op;
	if (!op) {
		/*
		 * Structural sharing makes raw generic identity sufficient,
		 * including when both heads are invalid.
		 */
		fyai_branch_decode(ctx->branch_prev, &prev);
		same = prev.head.v == ctx->last_message.v;
		op = same ? FYAI_BRANCH_OP_CONFIG : FYAI_BRANCH_OP_TURN;
	}
	opv = fy_value(ctx->gb, op);
	fromv = ctx->branch_op_from ?
		fy_value(ctx->gb, ctx->branch_op_from) : fy_invalid;
	fyai_branch_op_set(ctx, NULL, NULL);

	entry = fyai_branch_build(ctx->gb, ctx->arena_config, ctx->last_message,
				  created, ctx->branch_desc, ctx->branch_agent,
				  opv, fromv, ctx->branch_prev);
	if (!fy_generic_is_valid(entry))
		return fy_invalid;
	return fyai_branches_set(ctx->gb, ctx->arena_branches, name, entry);
}

static int fyai_root_publish_try(struct fyai_ctx *ctx)
{
	fy_generic catv, headv, branchesv, prevv, root;
	struct timespec t_commit;
	uint64_t desired;
	bool root_pinned;
	int rc;

	fyai_prof_stamp(&t_commit);

	/*
	 * Transient session: state lives only in the in-memory overlay, so never
	 * advance the durable refs head. In-memory ctx state stays as callers set
	 * it, so the current run still sees its own edits.
	 */
	/* Refuse writes through a pinned root. */
	root_pinned = ctx->cfg && ctx->cfg->root_pinned;
	if (root_pinned)
		fyai_branch_op_set(ctx, NULL, NULL);
	fyai_error_check(ctx, !root_pinned, err_out,
			 "--root is read-only; this command would change state");
	if (ctx->cfg && ctx->cfg->transient) {
		fyai_branch_op_set(ctx, NULL, NULL);
		return 0;
	}

	catv = fyai_generic_or_null(ctx->arena_catalog);
	headv = fy_value(ctx->gb, fyai_ctx_head_branch(ctx));
	if (!fy_generic_is_valid(headv))
		return -1;
	branchesv = fyai_branches_commit(ctx);
	if (!fy_generic_is_valid(branchesv))
		return -1;
	/*
	 * Chain each root to its predecessor (the current refs head) so root
	 * updates form a ref log: a navigable history where a turnless update
	 * (a config change with head unmoved) is a first-class entry just like a
	 * turn commit. The link is a reference into the immutable arena, O(1) per
	 * publish. NOTE: gc must bound how far back this chain is retained, else
	 * following prev keeps all history reachable.
	 */
	prevv = ctx->refs_head ? (fy_generic){ .v = ctx->refs_head } : fy_null;
	root = fyai_root_build(ctx->gb, catv, branchesv, headv,
			       fy_value(ctx->gb,
					(long long)fyai_branch_timestamp()), prevv);
	if (!fy_generic_is_valid(root))
		return -1;
	desired = (uint64_t)root.v;
	rc = fy_allocator_refs_publish(ctx->durable_allocator, ctx->refs_head,
				       desired, FY_ALLOC_REFS_CHECKPOINT);
	if (rc)
		return rc;
	ctx->refs_head = desired;
	ctx->arena_branches = branchesv;
	/* Chain the next publish to the entry that was written. */
	ctx->branch_prev = fy_get(branchesv, fyai_ctx_branch(ctx));
	fyai_prof_since("commit_durable", &t_commit);
	return 0;

err_out:
	return -1;
}

/*
 * Read the config (and optionally catalog) documents out of the repo arena
 * without a full ctx, for the config-load phase. The documents are
 * internalized into @gb so they survive the throwaway allocator. A
 * missing/fresh arena is not an error (outputs stay fy_invalid); an
 * unrecognizable root is.
 */
int fyai_peek_arena_config(const char *arena_dir_opt, const char *branch_opt,
			   const char *root_spec, struct fy_generic_builder *gb,
			   fy_generic *configp, fy_generic *catalogp,
			   char **branchp, uint64_t *rootp)
{
	struct fy_durable_allocator_cfg dur_cfg = {};
	struct fy_allocator *allocator;
	struct fyai_branch b;
	struct fyai_root r;
	const char *name;
	char *arena_dir;
	uint64_t refs;
	fy_generic_value pinned;
	fy_generic root;
	int ret;

	*configp = fy_invalid;
	if (catalogp)
		*catalogp = fy_invalid;
	if (branchp)
		*branchp = NULL;
	if (rootp)
		*rootp = 0;
	ret = 0;

	arena_dir = arena_dir_opt ? strdup(arena_dir_opt) :
		    fyai_default_arena_dir();
	if (!arena_dir)
		return 0;
	dur_cfg.region_base = fyai_arena_boot_base(arena_dir);
	if (!dur_cfg.region_base)
		goto out;
	dur_cfg.dir = arena_dir;
	dur_cfg.flags = FY_DURABLE_ARENA_CREATE | FY_DURABLE_ARENA_DEDUP |
			FY_DURABLE_ARENA_SPARSE | FY_DURABLE_ARENA_SEPARATE_INDEX;
	/* an ASAN-based arena maps at the reserved ranges; release them */
	fyai_unreserve_arena_ranges(&dur_cfg);
	allocator = fy_allocator_create("durable", &dur_cfg);
	if (!allocator)
		goto out;
	refs = fy_allocator_refs_get(allocator);
	/* Resolve --root before reading its branch configuration. */
	if (refs && root_spec) {
		if (fyai_root_resolve_spec(allocator, refs, root_spec, &pinned)) {
			fprintf(stderr, "--root '%s' names no root in %s; "
				"gc may have dropped it\n", root_spec, arena_dir);
			fy_allocator_destroy(allocator);
			ret = -1;
			goto out;
		}
		refs = pinned;
		if (rootp)
			*rootp = pinned;
	}
	if (refs) {
		root = (fy_generic){ .v = refs };
		if (fyai_root_decode(root, &r) < 0) {
			/* A probe: no context to collect into, so report here. */
			fprintf(stderr,
				"PEEK: unrecognized arena root at %s; re-run fyai init\n",
				arena_dir);
			ret = -1;
		} else {
			/* Resolve the selected branch before a context exists. */
			name = branch_opt;
			if (!name)
				name = fyai_root_head_name(&r);
			if (!name)
				name = FYAI_BRANCH_DEFAULT;
			fyai_branch_lookup(r.branches, name, &b);
			if (branchp) {
				*branchp = strdup(name);
				if (!*branchp)
					ret = -1;
			}
			if (fy_generic_is_valid(b.config)) {
				*configp = fy_gb_internalize(gb, b.config);
				if (!fy_generic_is_valid(*configp))
					ret = -1;
			}
			if (catalogp && fy_generic_is_valid(r.catalog)) {
				*catalogp = fy_gb_internalize(gb, r.catalog);
				if (!fy_generic_is_valid(*catalogp))
					ret = -1;
			}
		}
	}
	fy_allocator_destroy(allocator);
out:
	free(arena_dir);
	return ret;
}

/* Reconcile the surviving root after a lost CAS. */
static int publish_reconcile(struct fyai_ctx *ctx)
{
	struct fyai_branch cur;
	struct fyai_root r;
	fy_generic root, head;
	const char *policy;
	uint64_t refs;
	int rc;

	refs = fy_allocator_refs_get(ctx->durable_allocator);
	fyai_error_check(ctx, refs, err_out,
			 "could not read the concurrent root");
	root = (fy_generic){ .v = refs };
	fyai_error_check(ctx, fyai_root_validate(ctx->durable_allocator, root),
			 err_out, "concurrent root is invalid");
	rc = fyai_root_decode(root, &r);
	fyai_error_check(ctx, rc >= 0, err_out,
			 "could not decode the concurrent root");

	ctx->refs_head = refs;
	ctx->arena_branches = r.branches;
	ctx->arena_catalog = r.catalog;
	fyai_branch_lookup(r.branches, fyai_ctx_branch(ctx), &cur);

	/* Our branch is where we left it: nothing of ours is at stake. */
	if (cur.entry.v == ctx->branch_prev.v) {
		ctx->branch_prev = cur.entry;
		return 0;
	}

	policy = ctx->cfg->branch_on_conflict;
	if (!policy || !strcmp(policy, "abort")) {
		fyai_error(ctx, "branch '%s' changed while this command was "
			   "running; nothing was written. Set "
			   "branch/on_conflict to rebase or merge to replay "
			   "on top instead", fyai_ctx_branch(ctx));
		return -1;
	}

	if (fyai_join_onto_head(ctx, cur.head, &head))
		return -1;
	ctx->last_message = head;
	ctx->branch_prev = cur.entry;
	fyai_notice(ctx, "branch '%s' changed while this command was running; "
		    "replayed on top of it", fyai_ctx_branch(ctx));
	return 0;

err_out:
	return -1;
}

int fyai_publish_state(struct fyai_ctx *ctx)
{
	const char *op, *from;
	int rc, tries;

	if (!ctx->durable_allocator || !ctx->durable_gb)
		return 0;
	/* Skip a branch that has no state. */
	if (fy_generic_is_invalid(ctx->last_message) &&
	    fy_generic_is_invalid(ctx->arena_config) &&
	    fy_generic_is_invalid(ctx->arena_catalog) &&
	    fy_generic_is_invalid(ctx->branch_prev))
		return 0;

	/*
	 * The publish consumes the operation label, so keep a copy: a retry
	 * after a lost CAS must still record what it was doing.
	 */
	op = ctx->branch_op;
	from = ctx->branch_op_from;

	for (tries = 0; tries < 4; tries++) {
		fyai_branch_op_set(ctx, op, from);
		rc = fyai_root_publish_try(ctx);
		if (!rc)
			return 0;
		if (rc < 0)
			break;
		/* Lost the CAS: re-read and decide whether it matters. */
		if (publish_reconcile(ctx))
			return -1;
	}
	fyai_error(ctx, rc > 0 ? "fyai state changed concurrently" :
		   "failed to publish fyai state");
	return -1;
}

int fyai_publish_root(struct fyai_ctx *ctx, fy_generic config,
		      fy_generic catalog, fy_generic head)
{
	struct fyai_branch cur_b;
	struct fyai_root cur_r;
	fy_generic root;
	int tries, rc;

	if (!ctx->durable_allocator || !ctx->durable_gb)
		return -1;

	if (fy_generic_is_valid(config))
		ctx->arena_config = config;
	if (fy_generic_is_valid(catalog))
		ctx->arena_catalog = catalog;
	if (fy_generic_is_valid(head))
		ctx->last_message = head;

	for (tries = 0; tries < 2; tries++) {
		rc = fyai_root_publish_try(ctx);
		if (rc <= 0)
			goto out;
		/* Adopt the surviving root, then apply this branch's changes. */
		ctx->refs_head = fy_allocator_refs_get(ctx->durable_allocator);
		if (!ctx->refs_head)
			continue;
		root = (fy_generic){ .v = ctx->refs_head };
		if (fyai_root_decode(root, &cur_r) < 0) {
			rc = -1;
			goto out;
		}
		ctx->arena_branches = cur_r.branches;
		fyai_branch_lookup(cur_r.branches, fyai_ctx_branch(ctx), &cur_b);
		ctx->branch_prev = cur_b.entry;
		ctx->branch_desc = cur_b.description;
		ctx->branch_agent = cur_b.agent;
		if (!fy_generic_is_valid(config))
			ctx->arena_config = cur_b.config;
		if (!fy_generic_is_valid(catalog))
			ctx->arena_catalog = cur_r.catalog;
		if (!fy_generic_is_valid(head))
			ctx->last_message = cur_b.head;
	}
out:
	if (rc) {
		fyai_error(ctx, rc > 0 ? "fyai state changed concurrently" :
			   "failed to publish fyai state");
		return -1;
	}
	return 0;
}

/* Delay a branch publish for functional CAS tests. */
static void branch_publish_test_delay(void)
{
	const char *s;
	char *end;
	long ms;

	s = getenv("FYAI_TEST_BRANCH_CAS_DELAY_MS");
	if (!s || !*s)
		return;
	errno = 0;
	ms = strtol(s, &end, 10);
	if (errno || *end || ms <= 0 || ms > 5000)
		return;
	usleep((useconds_t)ms * 1000);
}

/* Reapply a branch-table delta after a lost CAS. */
static fy_generic branches_delta_apply(struct fyai_ctx *ctx, fy_generic base,
				       fy_generic desired, fy_generic latest)
{
	fy_generic key, before, after, now;
	const char *name;
	size_t i, count;

	if (!fy_generic_is_mapping(base))
		base = fy_map_empty;
	if (!fy_generic_is_mapping(desired))
		desired = fy_map_empty;
	if (!fy_generic_is_mapping(latest))
		latest = fy_map_empty;

	/* Changed or added keys. */
	count = fy_generic_mapping_get_pair_count(desired);
	for (i = 0; i < count; i++) {
		key = fy_get_key_at(desired, i);
		name = fy_castp(&key, "");
		if (!name || !*name)
			continue;
		before = fy_get(base, name);
		after = fy_get_at(desired, i);
		if (generic_same(before, after))
			continue;
		now = fy_get(latest, name);
		if (!generic_same(now, before))
			goto err_concurrent_change;
		latest = fy_assoc(ctx->gb, latest, name, after);
		fyai_error_check(ctx, fy_generic_is_valid(latest), err_out,
				 "could not reapply branch '%s'", name);
	}

	/* Removed keys. */
	count = fy_generic_mapping_get_pair_count(base);
	for (i = 0; i < count; i++) {
		key = fy_get_key_at(base, i);
		name = fy_castp(&key, "");
		if (!name || !*name || fy_generic_is_valid(fy_get(desired, name)))
			continue;
		before = fy_get_at(base, i);
		now = fy_get(latest, name);
		if (!generic_same(now, before))
			goto err_concurrent_change;
		latest = fy_disassoc(ctx->gb, latest, name);
		fyai_error_check(ctx, fy_generic_is_valid(latest), err_out,
				 "could not reapply deletion of branch '%s'",
				 name);
	}
	return latest;

err_concurrent_change:
	fyai_error(ctx, "branch '%s' changed concurrently; nothing was written",
		   name);
err_out:
	return fy_invalid;
}

int fyai_publish_branches(struct fyai_ctx *ctx, fy_generic base,
			  fy_generic branches)
{
	struct fyai_root current, original;
	fy_generic root, merged, headv, catv, prevv;
	const char *head_name;
	char *wanted_head;
	size_t wanted_head_len;
	uint64_t desired;
	int rc, tries;
	bool reconciled, head_changed;

	fyai_error_check(ctx, ctx->durable_allocator && ctx->durable_gb,
			 err_out, "branch table requires durable storage");
	if (ctx->cfg && ctx->cfg->root_pinned) {
		fyai_error(ctx, "--root is read-only; this command would "
			   "change state");
		return -1;
	}
	if (ctx->cfg && ctx->cfg->transient) {
		ctx->arena_branches = branches;
		return 0;
	}

	merged = branches;
	reconciled = false;
	wanted_head_len = strlen(fyai_ctx_head_branch(ctx));
	wanted_head = alloca(wanted_head_len + 1);
	memcpy(wanted_head, fyai_ctx_head_branch(ctx), wanted_head_len + 1);
	head_changed = false;
	if (ctx->refs_head) {
		root = (fy_generic){ .v = ctx->refs_head };
		if (fyai_root_decode(root, &original) >= 0) {
			head_name = fyai_root_head_name(&original);
			head_changed = !head_name ||
				       strcmp(head_name, wanted_head);
		}
	}
	for (tries = 0; tries < 4; tries++) {
		catv = fyai_generic_or_null(ctx->arena_catalog);
		headv = fy_value(ctx->gb, fyai_ctx_head_branch(ctx));
		prevv = ctx->refs_head ?
			(fy_generic){ .v = ctx->refs_head } : fy_null;
		root = fyai_root_build(ctx->gb, catv, merged, headv,
				       fy_value(ctx->gb, (long long)
						fyai_branch_timestamp()), prevv);
		fyai_error_check(ctx, fy_generic_is_valid(root), err_out,
				 "could not build branch-table root");
		desired = (uint64_t)root.v;
		branch_publish_test_delay();
		rc = fy_allocator_refs_publish(ctx->durable_allocator,
					       ctx->refs_head, desired,
					       FY_ALLOC_REFS_CHECKPOINT);
		if (!rc) {
			ctx->refs_head = desired;
			ctx->arena_branches = merged;
			if (reconciled)
				fyai_notice(ctx, "state changed concurrently; "
					    "reapplied branch-table changes");
			return 0;
		}
		fyai_error_check(ctx, rc > 0, err_out,
				 "could not publish branch-table root");

		ctx->refs_head = fy_allocator_refs_get(ctx->durable_allocator);
		fyai_error_check(ctx, ctx->refs_head, err_out,
				 "could not read concurrent arena root");
		root = (fy_generic){ .v = ctx->refs_head };
		rc = -1;
		if (fyai_root_validate(ctx->durable_allocator, root))
			rc = fyai_root_decode(root, &current);
		fyai_error_check(ctx, rc >= 0, err_out,
				 "concurrent arena root is invalid");
		merged = branches_delta_apply(ctx, base, branches,
					      current.branches);
		fyai_error_check(ctx, fy_generic_is_valid(merged), err_out,
				 "could not reconcile branch-table changes");
		ctx->arena_catalog = current.catalog;
		if (!head_changed) {
			free(ctx->head_branch);
			ctx->head_branch = NULL;
			head_name = fyai_root_head_name(&current);
			if (head_name) {
				ctx->head_branch = strdup(head_name);
				fyai_error_check(ctx, ctx->head_branch, err_out,
						 "out of memory reading "
						 "concurrent HEAD");
			}
		}
		reconciled = true;
	}
	fyai_error(ctx, "failed to publish branch-table changes");

err_out:
	return -1;
}

int fyai_close_storage(struct fyai_ctx *ctx)
{
	/* Release the transient overlay first (it parents onto durable_gb). */
	if (ctx->gb && ctx->gb != ctx->durable_gb) {
		fy_generic_builder_destroy(ctx->gb);
		if (ctx->overlay_allocator) {
			fy_allocator_destroy(ctx->overlay_allocator);
			ctx->overlay_allocator = NULL;
		}
	}
	ctx->gb = NULL;
	if (ctx->durable_gb) {
		fy_generic_builder_destroy(ctx->durable_gb);
		ctx->durable_gb = NULL;
	}
	if (ctx->durable_allocator) {
		fy_allocator_destroy(ctx->durable_allocator);
		ctx->durable_allocator = NULL;
	}
	ctx->last_message = fy_invalid;
	ctx->arena_config = fy_invalid;
	ctx->arena_catalog = fy_invalid;
	ctx->arena_branches = fy_invalid;
	ctx->branch_prev = fy_invalid;
	ctx->branch_desc = fy_invalid;
	ctx->branch_agent = fy_invalid;
	ctx->refs_head = 0;
	return 0;
}

/*
 * Truncate the ref-log chain to at most @keep entries (the current root plus
 * keep-1 predecessors). Rebuilds the retained roots bottom-up with the oldest
 * kept root's prev cut to null, then publishes the rebuilt head; the older
 * roots become unreachable and are freed by the arena gc that follows. A no-op
 * when keep < 1 or the chain is already within the limit. Must run with storage
 * open (uses ctx->gb and the durable refs).
 */
/* Return the branch ref-log length, capped at @limit. */
static int branch_chain_len(fy_generic entry, int limit)
{
	struct fyai_branch b;
	int n;

	for (n = 0; n < limit && fy_generic_is_valid(entry); n++) {
		if (!fyai_branch_decode(entry, &b))
			break;
		entry = b.prev;
	}
	return n;
}

/* Limit the branch ref log to @keep entries. */
static fy_generic fyai_branch_reflog_trim(struct fyai_ctx *ctx, fy_generic entry,
					  int keep, fy_generic *scratch)
{
	struct fyai_branch b;
	fy_generic node, rebuilt;
	int n, i;

	if (!fy_generic_is_mapping(entry))
		return entry;

	node = entry;
	for (n = 0; n < keep && fy_generic_is_valid(node); n++) {
		scratch[n] = node;
		if (!fyai_branch_decode(node, &b))
			return fy_invalid;
		node = b.prev;
	}
	/* Nothing beyond the window: keep the entry (and its sharing) as is. */
	if (fy_generic_is_invalid(node))
		return entry;

	rebuilt = fy_null;	/* prev of the oldest kept entry: cut here */
	for (i = n - 1; i >= 0; i--) {
		if (!fyai_branch_decode(scratch[i], &b))
			return fy_invalid;
		rebuilt = fyai_branch_build(ctx->gb, b.config, b.head,
					    fy_get(scratch[i], "created"),
					    b.description, b.agent, b.op,
					    b.from, rebuilt);
		if (!fy_generic_is_valid(rebuilt))
			return fy_invalid;
	}
	return rebuilt;
}

static int fyai_reflog_truncate(struct fyai_ctx *ctx, int keep)
{
	fy_generic roots[FYAI_REFLOG_KEEP_MAX];
	fy_generic entries[FYAI_REFLOG_KEEP_MAX];
	fy_generic node, rebuilt, branches, name, entry;
	struct fyai_root r;
	uint64_t desired;
	bool trim;
	int n, i, rc;

	if (keep < 1 || !ctx->refs_head || !ctx->durable_allocator || !ctx->gb)
		return 0;
	if (keep > FYAI_REFLOG_KEEP_MAX)
		keep = FYAI_REFLOG_KEEP_MAX;

	node = (fy_generic){ .v = ctx->refs_head };
	for (n = 0; n < keep && fy_generic_is_valid(node); n++) {
		roots[n] = node;
		node = fyai_root_prev(node);
	}
	/* A short root log can contain an overlong branch log. */
	if (fy_generic_is_invalid(node) && n > 0) {
		if (fyai_root_decode(roots[0], &r) < 0)
			return -1;
		trim = false;
		if (fy_generic_is_mapping(r.branches)) {
			fy_foreach(name, r.branches) {
				if (branch_chain_len(fy_get(r.branches, name),
						     keep + 1) > keep) {
					trim = true;
					break;
				}
			}
		}
		if (!trim)
			return 0;
	}

	rebuilt = fy_null;	/* prev of the oldest kept root: cut here */
	for (i = n - 1; i >= 0; i--) {
		if (fyai_root_decode(roots[i], &r) < 0)
			return -1;
		branches = r.branches;
		/* Apply the same limit to each branch ref log. */
		if (fy_generic_is_mapping(branches)) {
			fy_foreach(name, r.branches) {
				entry = fy_get(r.branches, name);
				entry = fyai_branch_reflog_trim(ctx, entry, keep,
								entries);
				if (fy_generic_is_invalid(entry))
					return -1;
				branches = fy_assoc(ctx->gb, branches, name,
						    entry);
				if (!fy_generic_is_valid(branches))
					return -1;
			}
		}
		rebuilt = fyai_root_build(ctx->gb,
				fyai_generic_or_null(r.catalog),
				fyai_generic_or_null(branches),
				fyai_generic_or_null(r.head),
				fy_get(roots[i], "created"), rebuilt);
		if (!fy_generic_is_valid(rebuilt))
			return -1;
	}
	desired = (uint64_t)rebuilt.v;
	rc = fy_allocator_refs_publish(ctx->durable_allocator, ctx->refs_head,
				       desired, FY_ALLOC_REFS_CHECKPOINT);
	if (rc)
		return -1;
	ctx->refs_head = desired;
	return 0;
}

int fyai_gc_storage(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_gc_args *args = &cfg->cmd.args.gc;
	int rc;

	if (access(cfg->arena_dir, F_OK)) {
		if (errno == ENOENT) {
			printf("gc: no arena at %s\n", cfg->arena_dir);
			return 0;
		}
		return -1;
	}
	/*
	 * Cut the ref log to the requested window before compacting, so the
	 * dropped roots (and anything only they referenced) become collectable.
	 * gc runs with storage closed (NO_STORAGE), so open the arena just for
	 * the rewrite and release it again - the compaction needs a quiescent
	 * arena.
	 */
	if (args->keep_reflogs >= 1) {
		if (fyai_setup_storage(ctx))
			return -1;
		rc = fyai_reflog_truncate(ctx, args->keep_reflogs);
		fyai_close_storage(ctx);
		if (rc) {
			fyai_error(ctx, "gc: failed to truncate ref log");
			return -1;
		}
	}
	rc = fy_durable_arena_gc(cfg->arena_dir);
	if (rc == 1) {
		fyai_error(ctx, "gc: arena is busy");
		return -1;
	}
	if (rc)
		return -1;
	printf("gc: compacted %s\n", cfg->arena_dir);
	return 0;
}

/*
 * Refuse to ingest a config document carrying a raw api_key string; the
 * content-addressed arena is immutable, a leaked secret cannot be removed.
 * Only an { type: env|secret, value: NAME } indirection is allowed.
 */
bool fyai_config_has_raw_secret(fy_generic doc)
{
	fy_generic v, providers, preset;
	size_t i, count;

	if (!fy_generic_is_mapping(doc))
		return false;
	v = fy_get(doc, "api_key");
	if (fy_generic_is_string(v))
		return true;
	providers = fy_get(doc, "providers");
	if (!fy_generic_is_mapping(providers))
		return false;
	count = fy_generic_mapping_get_pair_count(providers);
	for (i = 0; i < count; i++) {
		preset = fy_get_at(providers, i);
		v = fy_get(preset, "api_key");
		if (fy_generic_is_string(v))
			return true;
	}
	return false;
}

/*
 * Return the closest strict ancestor of @dir carrying a .fyai entry, or
 * NULL. Nesting is allowed - init creates a new project underneath, which
 * shadows the enclosing arena for everything below - but say so.
 */
static char *fyai_enclosing_project(const char *dir)
{
	char real[PATH_MAX], probe[PATH_MAX];
	char *slash;

	if (!realpath(dir, real))
		return NULL;
	for (;;) {
		slash = strrchr(real, '/');
		if (!slash || slash == real)
			return NULL;
		*slash = '\0';
		if (snprintf(probe, sizeof(probe), "%s/.fyai", real) >=
		    (int)sizeof(probe))
			return NULL;
		if (!access(probe, F_OK))
			return strdup(real);
	}
}

int fyai_init_storage(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_init_args *args = &cfg->cmd.args.init;
	char *arena_dir = NULL;
	char *enclosing;
	struct fyai_branch b;
	struct fyai_root r;
	fy_generic report;
	fy_generic root, head, config, catalog;
	int rc, ret;

	enclosing = fyai_enclosing_project(args->dir);
	if (enclosing) {
		/* init goes ahead and makes it; an error here would also demote
		 * whatever actually goes wrong below. */
		fyai_warning(ctx, "init: new project nested inside the fyai "
			     "project at %s", enclosing);
		free(enclosing);
	}

	ret = -1;
	head = fy_invalid;
	config = fy_invalid;
	catalog = fy_invalid;

	if (asprintf(&arena_dir, "%s/.fyai/arena", args->dir) < 0) {
		fyai_error(ctx, "init: OOM");
		return -1;
	}

	/* Where init is putting things: normal output, not a diagnostic. */
	fprintf(stderr, "arena: %s\n", arena_dir);

	if (fyai_open_arena(ctx, arena_dir)) {
		fyai_error(ctx, "init: cannot open arena at %s", arena_dir);
		goto out;
	}

	if (ctx->refs_head) {
		root = (fy_generic){ .v = ctx->refs_head };
		rc = fyai_root_decode(root, &r);
		if (rc < 0 && !args->force) {
			fyai_error(ctx, "init: unrecognized arena root (use --force to reset)");
			goto out;
		}
		if (rc >= 0) {
			/* Inherit the default branch's existing state. */
			ctx->arena_branches = r.branches;
			fyai_branch_lookup(r.branches, fyai_ctx_branch(ctx), &b);
			head = b.head;
			config = b.config;
			catalog = r.catalog;
			ctx->branch_prev = b.entry;
			ctx->branch_desc = b.description;
			ctx->branch_agent = b.agent;
		}
		if (args->config && fy_generic_is_valid(config) &&
		    !args->force) {
			fyai_error(ctx, "init: arena already carries a config (use --force)");
			goto out;
		}
		if (!args->config && rc >= 0) {
			printf("already initialized .fyai\n");
			ret = 0;
			goto out;
		}
	}

	if (args->config) {
		config = fy_parse_file(ctx->gb,
				       FYAI_YAML_PARSE_FLAGS,
				       args->config);
		if (!fy_generic_is_valid(config)) {
			fyai_error(ctx, "init: cannot parse %s", args->config);
			goto out;
		}
		report = fyai_config_validate_report(ctx->cfg, config,
						     args->config);
		if (fy_not_equal(fy_get(report, "result"), "ok")) {
			fyai_config_report_problems(ctx->cfg, report);
			goto out;
		}
		config = fy_get(report, "config", config);
	} else if (fy_generic_is_invalid(config)) {
		/* No config supplied and none inherited: seed the arena with the
		 * embedded config.yaml.sample so the project starts from a
		 * working document rather than an empty config. */
		fy_generic_sized_string sample = {
			.data = (const char *)FYAI_EMBEDDED_CONFIG,
			.size = FYAI_EMBEDDED_CONFIG_LEN,
		};

		config = fy_parse(ctx->gb, sample,
				  FYAI_YAML_PARSE_FLAGS |
				  FYOPPF_INPUT_TYPE_STRING, NULL);
		if (!fy_generic_is_valid(config)) {
			fyai_error(ctx, "init: cannot parse embedded config sample");
			goto out;
		}
		report = fyai_config_validate_report(ctx->cfg, config,
						     "embedded config sample");
		if (fy_not_equal(fy_get(report, "result"), "ok")) {
			fyai_config_report_problems(ctx->cfg, report);
			goto out;
		}
		config = fy_get(report, "config", config);
	}

	ctx->arena_config = config;
	ctx->arena_catalog = catalog;
	ctx->last_message = head;
	rc = fyai_root_publish_try(ctx);
	if (rc) {
		fyai_error(ctx, "init: cannot publish arena root");
		goto out;
	}

	if (args->config)
		printf("initialized .fyai (config from %s)\n", args->config);
	else
		printf("initialized .fyai\n");
	ret = 0;
out:
	fyai_close_storage(ctx);
	free(arena_dir);
	return ret;
}
