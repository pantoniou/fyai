/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SCRATCH_H
#define FYAI_SCRATCH_H

#include <stddef.h>

struct fy_allocator;

/*
 * Bump arena for the temporaries of one operation. It uses a libfyaml
 * allocator without deduplication. An allocation is never freed alone: close
 * releases all of them. The arena is safe to use from several threads.
 */
struct fyai_scratch {
	struct fy_allocator *allocator;
	int tag;
};

/* Return 0, or -1 with errno set. */
int fyai_scratch_open(struct fyai_scratch *scratch);

/* Release every allocation. A closed arena can be closed again. */
void fyai_scratch_close(struct fyai_scratch *scratch);

/* Uninitialized memory, aligned for any type; NULL with errno set on failure. */
void *fyai_scratch_alloc(struct fyai_scratch *scratch, size_t size);

/* Zeroed memory for count items. */
void *fyai_scratch_calloc(struct fyai_scratch *scratch, size_t count, size_t size);

/*
 * Return an array of new_count items that starts with the old_count items of
 * old. The old array stays allocated until close. old can be NULL.
 */
void *fyai_scratch_grow(struct fyai_scratch *scratch, void *old, size_t old_count,
			size_t new_count, size_t size);

/* Copy of the length bytes of text and a NUL. */
char *fyai_scratch_strndup(struct fyai_scratch *scratch, const char *text, size_t length);

#endif
