/*
 * fyai_project_capture_none.c - the capture backend of a platform without capture
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include "fyai_project_capture_backend.h"

#ifdef FYAI_PROJECT_CAPTURE_BACKEND_NONE
/* The capture entry points are unsupported on this platform and use no member. */
const struct fyai_project_capture_backend fyai_project_capture_backend_none = {
	.name = "none",
};
#endif
