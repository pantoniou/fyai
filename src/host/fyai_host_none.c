/*
 * fyai_host_none.c - the host backend of a platform with no special primitives
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include "fyai_host_backend.h"

#ifdef FYAI_HOST_BACKEND_NONE
/* Every member is NULL: the core uses its portable defaults. */
const struct fyai_host_backend fyai_host_backend_none = {
	.name = "none",
};
#endif
