/*
 * fyai_secret_none.c - the secret store of a system without one
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include "fyai_secret.h"
#include "fyai_secret_store.h"

static int none_get(const char *service, const char *account, char **value, size_t *len)
{
	(void)service;
	(void)account;
	*value = NULL;
	*len = 0;
	return FYAI_SECRET_UNSUPPORTED;
}

static int none_set(const char *service, const char *label, const char *account,
		    const void *value, size_t len)
{
	(void)service;
	(void)label;
	(void)account;
	(void)value;
	(void)len;
	return FYAI_SECRET_UNSUPPORTED;
}

static int none_del(const char *service, const char *account)
{
	(void)service;
	(void)account;
	return FYAI_SECRET_UNSUPPORTED;
}

const struct fyai_secret_store fyai_secret_store_none = {
	.name = "unavailable",
	.get = none_get,
	.set = none_set,
	.del = none_del,
};
