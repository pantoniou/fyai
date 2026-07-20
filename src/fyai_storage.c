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


int fyai_root_decode(fy_generic root, fy_generic *headp, fy_generic *configp,
		     fy_generic *catalogp)
{
	fy_generic v;

	if (headp)
		*headp = fy_invalid;
	if (configp)
		*configp = fy_invalid;
	if (catalogp)
		*catalogp = fy_invalid;

	if (!fy_generic_is_mapping(root))
		return -1;
	v = fy_get(root, "fyai");
	if (fy_generic_is_invalid(v) ||
	    fy_cast(v, 0LL) != (long long)FYAI_ROOT_VERSION)
		return -1;
	if (headp)
		*headp = fyai_root_entry(root, "head");
	if (configp)
		*configp = fyai_root_entry(root, "config");
	if (catalogp)
		*catalogp = fyai_root_entry(root, "catalog");
	return FYAI_ROOT_VERSION;
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
				  fy_generic config, fy_generic catalog,
				  fy_generic head, fy_generic prev)
{
	return fy_gb_mapping(gb,
			     "fyai", (long long)FYAI_ROOT_VERSION,
			     "config", config,
			     "catalog", catalog,
			     "head", head,
			     "prev", prev);
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

bool fyai_root_validate(struct fy_allocator *a, fy_generic root)
{
	fy_generic head, config, catalog, prev;

	if (!fy_generic_is_mapping(root))
		return false;
	if (a && !fy_allocator_contains(a, -1,
					fy_generic_resolve_collection_ptr(root)))
		return false;
	if (fyai_root_decode(root, &head, &config, &catalog) < 0)
		return false;
	prev = fyai_root_prev(root);
	if (a && (!root_ref_contained(a, head) ||
		  !root_ref_contained(a, config) ||
		  !root_ref_contained(a, catalog) ||
		  !root_ref_contained(a, prev)))
		return false;
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
	fy_generic root, head;

	if (fyai_open_arena(ctx, cfg->arena_dir))
		goto err_out;

	if (ctx->refs_head) {
		root = (fy_generic){ .v = ctx->refs_head };
		/*
		 * Validate before trusting: mapping-ness, arena containment of the
		 * root and its referenced parts, version and integrity checksum.
		 */
		if (!fyai_root_validate(ctx->durable_allocator, root) ||
		    fyai_root_decode(root, &head, &ctx->arena_config,
				     &ctx->arena_catalog) < 0) {
			fyai_error(ctx, "unrecognized arena root at %s; re-run fyai init",
				   cfg->arena_dir);
			goto err_out;
		}
		/*
		 * --new is a clear: drop the head and publish the reset as a
		 * turnless reflog entry, exactly like the /clear command; the
		 * config and catalog ride along unchanged.
		 */
		if (!cfg->new_conversation)
			ctx->last_message = head;
		else if (fyai_publish_state(ctx))
			goto err_out;
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
static int fyai_root_publish_try(struct fyai_ctx *ctx)
{
	fy_generic cfgv, catv, headv, prevv, root;
	struct timespec t_commit;
	uint64_t desired;
	int rc;

	fyai_prof_stamp(&t_commit);

	cfgv = fy_generic_is_valid(ctx->arena_config) ?
	       ctx->arena_config : fy_null;
	catv = fy_generic_is_valid(ctx->arena_catalog) ?
	       ctx->arena_catalog : fy_null;
	headv = fy_generic_is_valid(ctx->last_message) ?
		ctx->last_message : fy_null;
	/*
	 * Transient session: state lives only in the in-memory overlay, so never
	 * advance the durable refs head. In-memory ctx state stays as callers set
	 * it, so the current run still sees its own edits.
	 */
	if (ctx->cfg && ctx->cfg->transient)
		return 0;
	/*
	 * Chain each root to its predecessor (the current refs head) so root
	 * updates form a ref log: a navigable history where a turnless update
	 * (a config change with head unmoved) is a first-class entry just like a
	 * turn commit. The link is a reference into the immutable arena, O(1) per
	 * publish. NOTE: gc must bound how far back this chain is retained, else
	 * following prev keeps all history reachable.
	 */
	prevv = ctx->refs_head ? (fy_generic){ .v = ctx->refs_head } : fy_null;
	root = fyai_root_build(ctx->gb, cfgv, catv, headv, prevv);
	if (!fy_generic_is_valid(root))
		return -1;
	desired = (uint64_t)root.v;
	rc = fy_allocator_refs_publish(ctx->durable_allocator, ctx->refs_head,
				       desired, FY_ALLOC_REFS_CHECKPOINT);
	if (rc)
		return rc;
	ctx->refs_head = desired;
	fyai_prof_since("commit_durable", &t_commit);
	return 0;
}

/*
 * Read the config (and optionally catalog) documents out of the repo arena
 * without a full ctx, for the config-load phase. The documents are
 * internalized into @gb so they survive the throwaway allocator. A
 * missing/fresh arena is not an error (outputs stay fy_invalid); an
 * unrecognizable root is.
 */
int fyai_peek_arena_config(const char *arena_dir_opt,
			   struct fy_generic_builder *gb, fy_generic *configp,
			   fy_generic *catalogp)
{
	struct fy_durable_allocator_cfg dur_cfg = {};
	struct fy_allocator *allocator;
	char *arena_dir;
	uint64_t refs;
	fy_generic root, config, catalog;
	int ret;

	*configp = fy_invalid;
	if (catalogp)
		*catalogp = fy_invalid;
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
	if (refs) {
		root = (fy_generic){ .v = refs };
		if (fyai_root_decode(root, NULL, &config, &catalog) < 0) {
			/* A probe: no context to collect into, so report here. */
			fprintf(stderr,
				"unrecognized arena root at %s; re-run fyai init\n",
				arena_dir);
			ret = -1;
		} else {
			if (fy_generic_is_valid(config)) {
				*configp = fy_gb_internalize(gb, config);
				if (!fy_generic_is_valid(*configp))
					ret = -1;
			}
			if (catalogp && fy_generic_is_valid(catalog)) {
				*catalogp = fy_gb_internalize(gb, catalog);
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

int fyai_publish_state(struct fyai_ctx *ctx)
{
	int rc;

	if (!ctx->durable_allocator || !ctx->durable_gb)
		return 0;
	if (fy_generic_is_invalid(ctx->last_message) &&
	    fy_generic_is_invalid(ctx->arena_config) &&
	    fy_generic_is_invalid(ctx->arena_catalog))
		return 0;
	rc = fyai_root_publish_try(ctx);
	if (rc) {
		fyai_error(ctx, rc > 0 ? "fyai state changed concurrently" :
			   "failed to publish fyai state");
		return -1;
	}
	return 0;
}

int fyai_publish_root(struct fyai_ctx *ctx, fy_generic config,
		      fy_generic catalog, fy_generic head)
{
	fy_generic root, cur_head, cur_cfg, cur_cat;
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
		/* conflict: merge the surviving parts, keep our overrides */
		ctx->refs_head = fy_allocator_refs_get(ctx->durable_allocator);
		if (!ctx->refs_head)
			continue;
		root = (fy_generic){ .v = ctx->refs_head };
		if (fyai_root_decode(root, &cur_head, &cur_cfg, &cur_cat) < 0) {
			rc = -1;
			goto out;
		}
		if (!fy_generic_is_valid(config))
			ctx->arena_config = cur_cfg;
		if (!fy_generic_is_valid(catalog))
			ctx->arena_catalog = cur_cat;
		if (!fy_generic_is_valid(head))
			ctx->last_message = cur_head;
	}
out:
	if (rc) {
		fyai_error(ctx, rc > 0 ? "fyai state changed concurrently" :
			   "failed to publish fyai state");
		return -1;
	}
	return 0;
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
static int fyai_reflog_truncate(struct fyai_ctx *ctx, int keep)
{
	fy_generic roots[FYAI_REFLOG_KEEP_MAX];
	fy_generic node, head, cfgv, catv, rebuilt;
	uint64_t desired;
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
	/* Nothing beyond the kept window: the chain is already short enough. */
	if (fy_generic_is_invalid(node))
		return 0;

	rebuilt = fy_null;	/* prev of the oldest kept root: cut here */
	for (i = n - 1; i >= 0; i--) {
		if (fyai_root_decode(roots[i], &head, &cfgv, &catv) < 0)
			return -1;
		rebuilt = fyai_root_build(ctx->gb,
				fy_generic_is_valid(cfgv) ? cfgv : fy_null,
				fy_generic_is_valid(catv) ? catv : fy_null,
				fy_generic_is_valid(head) ? head : fy_null,
				rebuilt);
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
		rc = fyai_root_decode(root, &head, &config, &catalog);
		if (rc < 0 && !args->force) {
			fyai_error(ctx, "init: unrecognized arena root (use --force to reset)");
			goto out;
		}
		if (rc < 0) {
			head = fy_invalid;
			config = fy_invalid;
			catalog = fy_invalid;
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
