/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-allocator.h>

#include "fyai_scratch.h"

int fyai_scratch_open(struct fyai_scratch *scratch)
{
	scratch->allocator = fy_allocator_create("mremap", NULL);
	if (!scratch->allocator) {
		errno = ENOMEM;
		return -1;
	}
	scratch->tag = fy_allocator_get_tag(scratch->allocator);
	if (scratch->tag == FY_ALLOC_TAG_ERROR) {
		fy_allocator_destroy(scratch->allocator);
		scratch->allocator = NULL;
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

void fyai_scratch_close(struct fyai_scratch *scratch)
{
	if (!scratch->allocator)
		return;
	fy_allocator_release_tag(scratch->allocator, scratch->tag);
	fy_allocator_destroy(scratch->allocator);
	scratch->allocator = NULL;
}

void *fyai_scratch_alloc(struct fyai_scratch *scratch, size_t size)
{
	void *memory;

	memory = fy_allocator_alloc(scratch->allocator, scratch->tag, size ? size : 1,
				    alignof(max_align_t));
	if (!memory)
		errno = ENOMEM;
	return memory;
}

void *fyai_scratch_calloc(struct fyai_scratch *scratch, size_t count, size_t size)
{
	void *memory;

	if (size && count > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	memory = fyai_scratch_alloc(scratch, count * size);
	if (memory)
		memset(memory, 0, count * size);
	return memory;
}

void *fyai_scratch_grow(struct fyai_scratch *scratch, void *old, size_t old_count,
			size_t new_count, size_t size)
{
	void *memory;

	if (size && new_count > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	memory = fyai_scratch_alloc(scratch, new_count * size);
	if (memory && old && old_count)
		memcpy(memory, old, old_count * size);
	return memory;
}

char *fyai_scratch_strndup(struct fyai_scratch *scratch, const char *text, size_t length)
{
	char *copy;

	copy = fyai_scratch_alloc(scratch, length + 1);
	if (!copy)
		return NULL;
	memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}
