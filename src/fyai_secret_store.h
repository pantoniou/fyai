/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SECRET_STORE_H
#define FYAI_SECRET_STORE_H

#include <stddef.h>

/*
 * A store of small secrets that the system keeps for this user: a value for
 * each (service, account) pair. A store lives in one file and exports one
 * table. Each function returns FYAI_SECRET_OK, FYAI_SECRET_NOT_FOUND when the
 * value is absent, FYAI_SECRET_UNSUPPORTED when the system has no store, or -1
 * with errno set.
 *
 *   keyctl     fyai_secret_keyctl.c      Linux kernel keyring; volatile
 *   keychain   fyai_secret_keychain.c    macOS keychain; persistent
 *   libsecret  fyai_secret_libsecret.c   the Secret Service of a Linux desktop
 *   none       fyai_secret_none.c        no store
 */
struct fyai_secret_store {
	const char *name;
	/* On success *@value is a NUL-terminated copy that the caller frees. */
	int (*get)(const char *service, const char *account, char **value, size_t *len);
	/* @label names the item for the user where the store shows one; it can be NULL. */
	int (*set)(const char *service, const char *label, const char *account,
		   const void *value, size_t len);
	int (*del)(const char *service, const char *account);
};

extern const struct fyai_secret_store fyai_secret_store_keyctl;
extern const struct fyai_secret_store fyai_secret_store_keychain;
extern const struct fyai_secret_store fyai_secret_store_libsecret;
extern const struct fyai_secret_store fyai_secret_store_none;

/* The store that `secret` and the API keys use: the machine-local one. */
const struct fyai_secret_store *fyai_secret_store_system(void);

/*
 * The store of the login of the user, which survives a reboot: the keychain, or
 * the Secret Service where the build has it, else none.
 */
const struct fyai_secret_store *fyai_secret_store_keyring(void);

#endif
