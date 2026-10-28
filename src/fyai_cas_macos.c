/*
 * fyai_cas_macos.c - file system primitives of the blob store on macOS
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_cas_backend.h"

#ifdef FYAI_CAS_BACKEND_MACOS
#include <fcntl.h>

static int mac_sync_file(int fd)
{
	return fcntl(fd, F_FULLFSYNC);
}

/*
 * The kernel has no copy call and no clone of an open descriptor: APFS clones
 * need a new name, which the capture backend makes. The core copies through a
 * buffer.
 */
const struct fyai_cas_backend fyai_cas_backend_macos = {
	.name = "macos",
	.sync_file = mac_sync_file,
};

#endif /* FYAI_CAS_BACKEND_MACOS */
