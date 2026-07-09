/* SPDX-License-Identifier: MIT */
#ifndef FYAI_STORAGE_H
#define FYAI_STORAGE_H

#include "fyai.h"

char *fyai_default_arena_dir(void);
char *fyai_history_path(void);
void fyai_reserve_arena_ranges(void);
int fyai_setup_storage(struct fyai_ctx *ctx);
int fyai_publish_state(struct fyai_ctx *ctx);
int fyai_close_storage(struct fyai_ctx *ctx);

/*
 * Decode a container root into its parts. Returns the schema version
 * (FYAI_ROOT_VERSION) on success, -1 on anything else (non-mapping,
 * missing/unknown version). Null-valued entries come back as fy_invalid.
 */
int fyai_root_decode(fy_generic root, fy_generic *headp, fy_generic *configp,
		     fy_generic *catalogp);

/* The predecessor root in the ref log, or fy_invalid at the chain start. */
fy_generic fyai_root_prev(fy_generic root);

/*
 * Validate a root ref before trusting it: a mapping, contained in @a (pass NULL
 * to skip containment), with a good version and integrity checksum and all
 * referenced parts contained. True if safe to decode/dereference.
 */
bool fyai_root_validate(struct fy_allocator *a, fy_generic root);

/*
 * Publish a new container root. Valid arguments replace the corresponding
 * part; fy_invalid keeps the current one. On a concurrent-change CAS
 * conflict the root is re-read once, the surviving parts merged, and the
 * publish retried.
 */
int fyai_publish_root(struct fyai_ctx *ctx, fy_generic config,
		      fy_generic catalog, fy_generic head);

int fyai_init_storage(struct fyai_ctx *ctx);

/* true when a config document carries a raw (non-env-indirected) api_key */
bool fyai_config_has_raw_secret(fy_generic doc);

/*
 * Read the repo arena's config (and optionally catalog) documents,
 * internalized into @gb, without a full ctx. Missing arena => outputs stay
 * fy_invalid, rc 0; bad root => rc -1. @catalogp may be NULL.
 */
int fyai_peek_arena_config(const char *arena_dir_opt,
			   struct fy_generic_builder *gb, fy_generic *configp,
			   fy_generic *catalogp);
int fyai_gc_storage(struct fyai_ctx *ctx);

#endif
