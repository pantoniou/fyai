/*
 * fyai_sandbox_none.c - the backend of a platform without confinement
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include "fyai_sandbox_backend.h"

#ifdef FYAI_SANDBOX_BACKEND_NONE

static int none_abi(void)
{
	return 0;
}

static bool none_available(void)
{
	return false;
}

static bool none_restrictable(int abi)
{
	(void)abi;
	return false;
}

static bool none_udp_deniable(void)
{
	return false;
}

static bool none_path_visible(const struct fyai_sandbox_spec *spec, const char *dir)
{
	(void)spec;
	return dir && dir[0] == '/';
}

static int none_apply(const struct fyai_sandbox_spec *spec)
{
	/* Nothing to confine with: fail closed only in strict mode. */
	return spec->strict ? -1 : 0;
}

const struct fyai_sandbox_backend fyai_sandbox_backend_none = {
	.name = "none",
	.abi = none_abi,
	.available = none_available,
	.net_restrictable = none_restrictable,
	.udp_restrictable = none_restrictable,
	.udp_deniable = none_udp_deniable,
	.path_visible = none_path_visible,
	.apply = none_apply,
};

#endif
