/*
 * fyai_transient.h - the transient builder component
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TRANSIENT_H
#define FYAI_TRANSIENT_H

#include <stdbool.h>

struct fy_allocator;
struct fy_generic_builder;

/*
 * The transient builder: the builder for the values of one operation: it is destroyed, not rewound,
 * when the operation ends. It stacks on the working builder of the arena, so
 * the values that it builds can refer to stored ones.
 *
 * The component depends on that builder through a slot, because the arena
 * replaces its builder when it reopens: the scratch reads the slot when it
 * makes the builder. It tells its owner when it releases the builder, through
 * a hook, so that what the owner keeps beside it can go with it.
 */
struct fyai_transient {
	/* Dependencies. */
	struct fy_generic_builder **parent_gb;	/* the slot of the working builder */
	void (*on_release)(void *data);		/* the builder is about to go */
	void *on_release_data;

	struct fy_allocator *allocator;
	struct fy_generic_builder *gb;		/* NULL: none is made */
	/* The builder was made for an idle operation: release it on the next
	 * loop iteration, unless an owner adopts it. */
	bool autorelease;
};

/* Make the component with no builder. */
void fyai_transient_init(struct fyai_transient *sc);

/*
 * Make the builder if there is none. An explicit owner adopts a builder that
 * fyai_transient_get() made: the idle release must not take it while the owner
 * works. Returns 0.
 */
int fyai_transient_setup(struct fyai_transient *sc);

/* Destroy the builder and its allocator. Safe to call with none. */
void fyai_transient_cleanup(struct fyai_transient *sc);

/*
 * The builder for the current operation. When no owner has made one, make
 * one that the idle loop releases. NULL when it cannot be made.
 */
struct fy_generic_builder *fyai_transient_get(struct fyai_transient *sc);

#endif
