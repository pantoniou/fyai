/*
 * fyai_secret_store.c - the choice of the secret store of the platform
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_secret_store.h"

const struct fyai_secret_store *fyai_secret_store_system(void)
{
#if defined(__APPLE__)
	return &fyai_secret_store_keychain;
#elif defined(__linux__)
	return &fyai_secret_store_keyctl;
#else
	return &fyai_secret_store_none;
#endif
}

const struct fyai_secret_store *fyai_secret_store_keyring(void)
{
#if defined(__APPLE__)
	return &fyai_secret_store_keychain;
#elif defined(HAVE_LIBSECRET)
	return &fyai_secret_store_libsecret;
#else
	return &fyai_secret_store_none;
#endif
}
