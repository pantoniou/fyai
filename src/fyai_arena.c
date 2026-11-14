/*
 * fyai_arena.c - the arena component
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-allocator.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_arena.h"

void fyai_arena_init(struct fyai_arena *arena)
{
	memset(arena, 0, sizeof(*arena));
}

void fyai_arena_release(struct fyai_arena *arena)
{
	/* Release the transient overlay first (it parents onto durable_gb). */
	if (arena->gb && arena->gb != arena->durable_gb) {
		fy_generic_builder_destroy(arena->gb);
		if (arena->overlay_allocator) {
			fy_allocator_destroy(arena->overlay_allocator);
			arena->overlay_allocator = NULL;
		}
	}
	arena->gb = NULL;
	if (arena->durable_gb) {
		fy_generic_builder_destroy(arena->durable_gb);
		arena->durable_gb = NULL;
	}
	if (arena->durable_allocator) {
		fy_allocator_destroy(arena->durable_allocator);
		arena->durable_allocator = NULL;
	}
	arena->refs_head = 0;
}
