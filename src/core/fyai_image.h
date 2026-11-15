/*
 * fyai_image.h - prebuilt images of the embedded YAML documents
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_IMAGE_H
#define FYAI_IMAGE_H

#include <stdlib.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

/*
 * The build parses the large embedded documents one time into a relocatable
 * image of libfyaml (fy_generic_image_create()), and embeds it in writable data
 * aligned to FY_GENERIC_IMAGE_ALIGN. A load moves the pointers in place. See
 * doc/embedded-images.md.
 *
 * Return 0 and set @rootp, or -1 when the image is not usable or the
 * environment variable FYAI_NO_IMAGE is set. The caller then parses the text.
 */
static inline int fyai_image_load(void *image, size_t len, fy_generic *rootp)
{
	if (getenv("FYAI_NO_IMAGE"))
		return -1;
	return fy_generic_image_load(image, len, rootp);
}

#endif
