/*
 * fyai_cas_none.c - the blob store backend of a platform without a store
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include <unistd.h>

#include "fyai_cas_backend.h"

#ifdef FYAI_CAS_BACKEND_NONE
static int none_sync_file(int fd)
{
	return fsync(fd);
}

const struct fyai_cas_backend fyai_cas_backend_none = {
	.name = "none",
	.sync_file = none_sync_file,
};
#endif
