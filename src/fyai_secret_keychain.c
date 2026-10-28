/*
 * fyai_secret_keychain.c - secrets in the macOS keychain
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Each secret is a generic-password item of the SecItem API, keyed by the
 * service and the account. It survives a reboot.
 */
#ifdef __APPLE__
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "fyai_secret.h"
#include "fyai_secret_store.h"

static CFDictionaryRef secret_query(const char *service, const char *name)
{
	const void *keys[3];
	const void *vals[3];
	CFStringRef svc, acct;
	CFDictionaryRef query;

	svc = CFStringCreateWithCString(NULL, service,
					kCFStringEncodingUTF8);
	acct = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	if (!svc || !acct) {
		if (svc)
			CFRelease(svc);
		if (acct)
			CFRelease(acct);
		return NULL;
	}

	keys[0] = kSecClass;
	vals[0] = kSecClassGenericPassword;
	keys[1] = kSecAttrService;
	vals[1] = svc;
	keys[2] = kSecAttrAccount;
	vals[2] = acct;

	query = CFDictionaryCreate(NULL, keys, vals, 3,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(svc);
	CFRelease(acct);

	return query;
}

static int kn_get(const char *service, const char *name, char **value, size_t *len)
{
	CFMutableDictionaryRef query;
	CFDictionaryRef base;
	CFTypeRef result;
	OSStatus status;
	char *buf;

	*value = NULL;
	*len = 0;

	base = secret_query(service, name);
	if (!base)
		return -1;
	query = CFDictionaryCreateMutableCopy(NULL, 0, base);
	CFRelease(base);
	if (!query)
		return -1;
	CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

	result = NULL;
	status = SecItemCopyMatching(query, &result);
	CFRelease(query);
	if (status == errSecItemNotFound)
		return FYAI_SECRET_NOT_FOUND;
	if (status != errSecSuccess || !result)
		return -1;

	*len = (size_t)CFDataGetLength((CFDataRef)result);
	buf = malloc(*len + 1);
	if (!buf) {
		CFRelease(result);
		*len = 0;
		return -1;
	}
	memcpy(buf, CFDataGetBytePtr((CFDataRef)result), *len);
	buf[*len] = '\0';
	CFRelease(result);

	*value = buf;
	return FYAI_SECRET_OK;
}

static int kn_set(const char *service, const char *label, const char *name, const void *value,
		  size_t len)
{
	CFDictionaryRef query, update, add;
	CFDataRef data;
	const void *ukey, *uval;
	OSStatus status;

	(void)label;
	query = secret_query(service, name);
	if (!query)
		return -1;

	data = CFDataCreate(NULL, (const UInt8 *)value, (CFIndex)len);
	if (!data) {
		CFRelease(query);
		return -1;
	}

	ukey = kSecValueData;
	uval = data;
	update = CFDictionaryCreate(NULL, &ukey, &uval, 1,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	status = SecItemUpdate(query, update);
	if (update)
		CFRelease(update);

	if (status == errSecItemNotFound) {
		add = CFDictionaryCreateMutableCopy(NULL, 0, query);
		if (add) {
			CFDictionarySetValue((CFMutableDictionaryRef)add,
				kSecValueData, data);
			status = SecItemAdd(add, NULL);
			CFRelease(add);
		} else {
			status = errSecAllocate;
		}
	}

	CFRelease(data);
	CFRelease(query);

	return status == errSecSuccess ? FYAI_SECRET_OK : -1;
}

static int kn_del(const char *service, const char *name)
{
	CFDictionaryRef query;
	OSStatus status;

	query = secret_query(service, name);
	if (!query)
		return -1;

	status = SecItemDelete(query);
	CFRelease(query);

	if (status == errSecItemNotFound)
		return FYAI_SECRET_NOT_FOUND;
	return status == errSecSuccess ? FYAI_SECRET_OK : -1;
}

const struct fyai_secret_store fyai_secret_store_keychain = {
	.name = "macOS keychain",
	.get = kn_get,
	.set = kn_set,
	.del = kn_del,
};
#endif
