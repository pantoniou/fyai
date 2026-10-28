/*
 * fyai_secret_keyctl.c - secrets in the kernel keyring of Linux
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The persistent keyring of the user holds each secret as a "user" key whose
 * description is the account. The keyring is volatile across a reboot.
 */
#include <errno.h>
#include <stdlib.h>

#include "fyai_secret.h"
#include "fyai_secret_store.h"

#if defined(__linux__) && __has_include(<linux/keyctl.h>)
#include <linux/keyctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(__linux__) && defined(SYS_keyctl) && defined(SYS_add_key)
static long keyctl_call(long cmd, unsigned long a2, unsigned long a3,
			unsigned long a4, unsigned long a5)
{
	return syscall(SYS_keyctl, cmd, a2, a3, a4, a5);
}

static long persistent_keyring(void)
{
	return keyctl_call(KEYCTL_GET_PERSISTENT, (unsigned long)getuid(),
			   (unsigned long)KEY_SPEC_USER_KEYRING, 0, 0);
}

static int kernel_error(void)
{
	if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EPERM ||
	    errno == EACCES)
		return FYAI_SECRET_UNSUPPORTED;
	if (errno == ENOKEY || errno == EKEYEXPIRED || errno == EKEYREVOKED)
		return FYAI_SECRET_NOT_FOUND;
	return -1;
}

static long find_key(long ring, const char *name)
{
	return keyctl_call(KEYCTL_SEARCH, (unsigned long)ring,
			   (unsigned long)"user", (unsigned long)name, 0);
}

static int kc_get(const char *service, const char *name, char **value, size_t *len)
{
	long ring, key, need, got;
	char *buf;

	(void)service;
	*value = NULL;
	*len = 0;
	ring = persistent_keyring();
	if (ring < 0)
		return kernel_error();
	key = find_key(ring, name);
	if (key < 0)
		return kernel_error();
	need = keyctl_call(KEYCTL_READ, (unsigned long)key, 0, 0, 0);
	if (need < 0)
		return kernel_error();
	buf = malloc((size_t)need + 1);
	if (!buf)
		return -1;
	got = keyctl_call(KEYCTL_READ, (unsigned long)key,
			  (unsigned long)buf, (unsigned long)need, 0);
	if (got < 0) {
		free(buf);
		return kernel_error();
	}
	buf[got] = '\0';
	*value = buf;
	*len = (size_t)got;
	return FYAI_SECRET_OK;
}

static int kc_set(const char *service, const char *label, const char *name, const void *value,
		  size_t len)
{
	long ring, key;

	(void)service;
	(void)label;
	ring = persistent_keyring();
	if (ring < 0)
		return kernel_error();
	key = find_key(ring, name);
	if (key >= 0) {
		if (keyctl_call(KEYCTL_UPDATE, (unsigned long)key,
				(unsigned long)value, (unsigned long)len, 0) < 0)
			return kernel_error();
		return FYAI_SECRET_OK;
	}
	if (errno != ENOKEY)
		return kernel_error();
	key = syscall(SYS_add_key, "user", name, value, len, ring);
	return key < 0 ? kernel_error() : FYAI_SECRET_OK;
}

static int kc_del(const char *service, const char *name)
{
	long ring, key;

	(void)service;
	ring = persistent_keyring();
	if (ring < 0)
		return kernel_error();
	key = find_key(ring, name);
	if (key < 0)
		return kernel_error();
	if (keyctl_call(KEYCTL_UNLINK, (unsigned long)key,
			(unsigned long)ring, 0, 0) < 0)
		return kernel_error();
	return FYAI_SECRET_OK;
}

const struct fyai_secret_store fyai_secret_store_keyctl = {
	.name = "kernel keyring (volatile across reboot)",
	.get = kc_get,
	.set = kc_set,
	.del = kc_del,
};
#else
/* The build has no keyctl system call: the store is unsupported. */
static int kc_get(const char *service, const char *account, char **value, size_t *len)
{
	(void)service;
	(void)account;
	*value = NULL;
	*len = 0;
	return FYAI_SECRET_UNSUPPORTED;
}

static int kc_set(const char *service, const char *label, const char *account,
		  const void *value, size_t len)
{
	(void)service;
	(void)label;
	(void)account;
	(void)value;
	(void)len;
	return FYAI_SECRET_UNSUPPORTED;
}

static int kc_del(const char *service, const char *account)
{
	(void)service;
	(void)account;
	return FYAI_SECRET_UNSUPPORTED;
}

const struct fyai_secret_store fyai_secret_store_keyctl = {
	.name = "unavailable",
	.get = kc_get,
	.set = kc_set,
	.del = kc_del,
};
#endif
