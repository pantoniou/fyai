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

/* Decoded container-root fields. */
struct fyai_root {
	fy_generic catalog;	/* arena-wide catalogue document */
	fy_generic branches;	/* mapping: branch name -> branch entry */
	fy_generic head;	/* the raw HEAD string generic */
};

/* Decode a versioned container root. */
int fyai_root_decode(fy_generic root, struct fyai_root *r);

/* Return the root HEAD name. @r must outlive the result. */
const char *fyai_root_head_name(const struct fyai_root *r);

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
 * Read a branch configuration without a context. Internalize results into
 * @gb. @branchp receives an allocated copy of the selected branch name.
 */
int fyai_peek_arena_config(const char *arena_dir_opt, const char *branch_opt,
			   struct fy_generic_builder *gb, fy_generic *configp,
			   fy_generic *catalogp, char **branchp);
int fyai_gc_storage(struct fyai_ctx *ctx);

#endif
