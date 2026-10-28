/*
 * fyai_secret_libsecret.c - secrets in the Secret Service of a Linux desktop
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_LIBSECRET
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <libsecret/secret.h>

#include "fyai_secret.h"
#include "fyai_secret_store.h"

/* The schema is named by the service and holds the account as its one attribute. */
static void ls_schema(SecretSchema *schema, const char *service)
{
	memset(schema, 0, sizeof(*schema));
	schema->name = service;
	schema->flags = SECRET_SCHEMA_NONE;
	schema->attributes[0].name = "account";
	schema->attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
}

static int ls_get(const char *service, const char *account, char **value, size_t *len)
{
	SecretSchema schema;
	char *secret, *copy;

	*value = NULL;
	*len = 0;
	ls_schema(&schema, service);
	secret = secret_password_lookup_sync(&schema, NULL, NULL, "account", account, NULL);
	if (!secret)
		return FYAI_SECRET_NOT_FOUND;
	copy = strdup(secret);
	secret_password_free(secret);
	if (!copy)
		return -1;
	*value = copy;
	*len = strlen(copy);
	return FYAI_SECRET_OK;
}

static int ls_set(const char *service, const char *label, const char *account,
		  const void *value, size_t len)
{
	SecretSchema schema;
	char *text;
	gboolean ok;

	/* The service takes text only. */
	text = strndup(value, len);
	if (!text)
		return -1;
	ls_schema(&schema, service);
	ok = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT,
					label ? label : service, text, NULL, NULL,
					"account", account, NULL);
	free(text);
	return ok ? FYAI_SECRET_OK : -1;
}

static int ls_del(const char *service, const char *account)
{
	SecretSchema schema;

	ls_schema(&schema, service);
	return secret_password_clear_sync(&schema, NULL, NULL, "account", account, NULL) ?
	       FYAI_SECRET_OK : FYAI_SECRET_NOT_FOUND;
}

const struct fyai_secret_store fyai_secret_store_libsecret = {
	.name = "Secret Service",
	.get = ls_get,
	.set = ls_set,
	.del = ls_del,
};
#endif
