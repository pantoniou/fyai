/*
 * fyai_fsview_none.c - the view backend of a platform without views
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include "fyai_fsview_backend.h"

#ifdef FYAI_FSVIEW_BACKEND_NONE
/* Every member is NULL: the core reports each operation as unsupported. */
const struct fyai_fsview_backend fyai_fsview_backend_none = {
	.name = "none",
};
#endif
