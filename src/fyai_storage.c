/*
 * fyai_storage.c - durable arena and local state paths
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

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
		if (fyai_root_decode(root, &head, &ctx->arena_config,
				     &ctx->arena_catalog) < 0) {
			fprintf(stderr,
				"unrecognized arena root at %s; re-run fyai init\n",
				cfg->arena_dir);
			goto err_out;
		}
		if (!cfg->new_conversation)
			ctx->last_message = head;
	}

	return 0;

err_out:
	fyai_close_storage(ctx);
	return -1;
}

void fyai_last_turn_cleanup(struct fyai_last_turn *lt)
{
	if (!lt)
		return;
	memset(lt, 0, sizeof(*lt));
}

void fyai_peek_last_turn(struct fyai_cfg *cfg, struct fyai_last_turn *out)
{
	struct fy_durable_allocator_cfg dur_cfg = {};
	struct fy_allocator *allocator;
	const char *arena_dir_opt = cfg->arena_dir;
	char *arena_dir = NULL;
	uint64_t refs;
	fy_generic root, head, cur, meta, temp, tp;
	const char *prov, *e, *s;

	memset(out, 0, sizeof(*out));

	arena_dir = arena_dir_opt ? strdup(arena_dir_opt) : fyai_default_arena_dir();
	if (!arena_dir)
		return;
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
	head = fy_invalid;
	refs = fy_allocator_refs_get(allocator);
	if (refs) {
		root = (fy_generic){ .v = refs };
		if (fyai_root_decode(root, &head, NULL, NULL) < 0)
			head = fy_invalid;
	}
	for (cur = head; fy_generic_is_valid(cur); cur = fy_get(cur, "previous")) {

		meta = fyai_turn_meta(cur);
		tp = fy_get(meta, "provider");
		if (fy_generic_is_invalid(tp))
			tp = fyai_turn_provider(cur);
		prov = fy_castp(&tp, "");
		if (!*prov)
			continue;

		out->provider = fy_gb_intern_string(cfg->gb, prov);

		temp = fy_get(meta, "temperature");
		if (fy_generic_is_valid(temp)) {
			out->temperature = fy_cast(temp, (double)0.0);
			out->has_temperature = true;
		}
		e = fy_get(meta, "model", "");
		out->model = fy_gb_intern_string(cfg->gb, e);
		e = fy_get(meta, "api", "");
		out->api = fy_gb_intern_string(cfg->gb, e);
		e = fy_get(meta, "reasoning_effort", "");
		out->reasoning_effort = fy_gb_intern_string(cfg->gb, e);
		s = fy_get(meta, "reasoning_summary", "");
		out->reasoning_summary = fy_gb_intern_string(cfg->gb, s);
		break;
	}
	fy_allocator_destroy(allocator);
out:
	free(arena_dir);
}

/*
 * Build the container root from the current ctx parts and CAS-advance the
 * refs head to it. Returns 0 on success, >0 on concurrent-change conflict,
 * <0 on error.
 */
static int fyai_root_publish_try(struct fyai_ctx *ctx)
{
	fy_generic cfgv, catv, headv, root;
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
	root = fy_gb_mapping(ctx->gb,
			     "fyai", (long long)FYAI_ROOT_VERSION,
			     "config", cfgv,
			     "catalog", catv,
			     "head", headv);
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
		fprintf(stderr, rc > 0 ? "fyai state changed concurrently\n" :
			"failed to publish fyai state\n");
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
		fprintf(stderr, rc > 0 ? "fyai state changed concurrently\n" :
			"failed to publish fyai state\n");
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

int fyai_gc_storage(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_gc_args *args = &cfg->cmd.args.gc;
	int rc;

	(void)args;

	if (access(cfg->arena_dir, F_OK)) {
		if (errno == ENOENT) {
			printf("gc: no arena at %s\n", cfg->arena_dir);
			return 0;
		}
		return -1;
	}
	fyai_close_storage(ctx);
	rc = fy_durable_arena_gc(cfg->arena_dir);
	if (rc == 1) {
		fprintf(stderr, "gc: arena is busy\n");
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
 * Only the { type: env, value: NAME } indirection is allowed.
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
	fy_generic root, head, config, catalog;
	int rc, ret;

	enclosing = fyai_enclosing_project(args->dir);
	if (enclosing) {
		fprintf(stderr,
			"init: new project nested inside the fyai project at %s\n",
			enclosing);
		free(enclosing);
	}

	ret = -1;
	head = fy_invalid;
	config = fy_invalid;
	catalog = fy_invalid;

	if (asprintf(&arena_dir, "%s/.fyai/arena", args->dir) < 0) {
		fprintf(stderr, "init: OOM\n");
		return -1;
	}

	fprintf(stderr, "arena: %s\n", arena_dir);

	if (fyai_open_arena(ctx, arena_dir)) {
		fprintf(stderr, "init: cannot open arena at %s\n", arena_dir);
		goto out;
	}

	if (ctx->refs_head) {
		root = (fy_generic){ .v = ctx->refs_head };
		rc = fyai_root_decode(root, &head, &config, &catalog);
		if (rc < 0 && !args->force) {
			fprintf(stderr,
				"init: unrecognized arena root (use --force to reset)\n");
			goto out;
		}
		if (rc < 0) {
			head = fy_invalid;
			config = fy_invalid;
			catalog = fy_invalid;
		}
		if (args->config && fy_generic_is_valid(config) &&
		    !args->force) {
			fprintf(stderr,
				"init: arena already carries a config (use --force)\n");
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
				       FYOPPF_DISABLE_DIRECTORY |
				       FYOPPF_MODE_YAML_1_2,
				       args->config);
		if (!fy_generic_is_valid(config)) {
			fprintf(stderr, "init: cannot parse %s\n", args->config);
			goto out;
		}
		if (fyai_config_has_raw_secret(config)) {
			fprintf(stderr,
				"init: %s carries a raw api_key; use { type: env, value: NAME }\n",
				args->config);
			goto out;
		}
	} else if (fy_generic_is_invalid(config)) {
		/* No config supplied and none inherited: seed the arena with the
		 * embedded config.yaml.sample so the project starts from a
		 * working document rather than an empty config. */
		fy_generic_sized_string sample = {
			.data = (const char *)FYAI_EMBEDDED_CONFIG,
			.size = FYAI_EMBEDDED_CONFIG_LEN,
		};

		config = fy_parse(ctx->gb, sample,
				  FYOPPF_DISABLE_DIRECTORY |
				  FYOPPF_MODE_YAML_1_2 |
				  FYOPPF_INPUT_TYPE_STRING, NULL);
		if (!fy_generic_is_valid(config)) {
			fprintf(stderr,
				"init: cannot parse embedded config sample\n");
			goto out;
		}
	}

	ctx->arena_config = config;
	ctx->arena_catalog = catalog;
	ctx->last_message = head;
	rc = fyai_root_publish_try(ctx);
	if (rc) {
		fprintf(stderr, "init: cannot publish arena root\n");
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
