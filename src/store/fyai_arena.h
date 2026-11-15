/*
 * fyai_arena.h - the arena component
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_ARENA_H
#define FYAI_ARENA_H

#include <stdint.h>

struct fy_allocator;
struct fy_generic_builder;

/*
 * The builders of the canonical state. The durable allocator and builder are
 * the content-addressed arena on disk. @gb is the working builder: the
 * durable one normally, or an in-memory builder stacked over it when the run
 * is transient, so that nothing reaches the arena. All canonical
 * configuration and turn state is built through it. When it differs from the
 * durable builder, @overlay_allocator backs it. An arena reopen replaces the
 * builders, so a borrower reads @gb at the use and does not keep it.
 *
 * Opening and publishing are in src/store/fyai_storage.c, which still reads the
 * configuration and the branch state of the context; this struct owns what
 * they open.
 */
struct fyai_arena {
	struct fy_allocator *durable_allocator;
	struct fy_generic_builder *durable_gb;
	struct fy_generic_builder *gb;
	struct fy_allocator *overlay_allocator;
	/* The head of the published root, as the allocator last gave it. */
	uint64_t refs_head;
};

/* Make the component with no arena open. */
void fyai_arena_init(struct fyai_arena *arena);

/*
 * Destroy the overlay, the durable builder and the allocator, in that order,
 * as the overlay stacks on the durable builder. Safe to call with none open.
 */
void fyai_arena_release(struct fyai_arena *arena);

#endif
