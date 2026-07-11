/*
 * fyai_secret.c - dependency-free machine-local secret storage
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

#ifdef __linux__
#include <linux/keyctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include "fyai_secret.h"
#include "fyai.h"

void fyai_secret_clear(void *value, size_t len)
{
	volatile unsigned char *p = value;

	while (len--)
		*p++ = 0;
}

void fyai_secret_clear_and_free(char **value, size_t *len)
{
	if (value && *value) {
		fyai_secret_clear(*value, *len);
		free(*value);
	}

	if (value)
		*value = NULL;
	if (len)
		*len = 0;
}

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

int fyai_secret_kernel_get(const char *name, char **value, size_t *len)
{
	long ring, key, need, got;
	char *buf;

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

int fyai_secret_kernel_set(const char *name, const void *value, size_t len)
{
	long ring, key;

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

int fyai_secret_kernel_delete(const char *name)
{
	long ring, key;

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
#define FYAI_SECRET_BACKEND_NAME "kernel keyring (volatile across reboot)"
#elif defined(__APPLE__)
/*
 * macOS: store secrets as generic-password keychain items, keyed by a fixed
 * service and the secret name as the account, via the modern SecItem* API.
 * Persistent across reboot, unlike the Linux kernel keyring.
 */
#define FYAI_SECRET_SERVICE "org.fyai.Secret"

static CFDictionaryRef secret_query(const char *name)
{
	const void *keys[3];
	const void *vals[3];
	CFStringRef svc, acct;
	CFDictionaryRef query;

	svc = CFStringCreateWithCString(NULL, FYAI_SECRET_SERVICE,
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

int fyai_secret_kernel_get(const char *name, char **value, size_t *len)
{
	CFMutableDictionaryRef query;
	CFDictionaryRef base;
	CFTypeRef result;
	OSStatus status;
	char *buf;

	*value = NULL;
	*len = 0;

	base = secret_query(name);
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

int fyai_secret_kernel_set(const char *name, const void *value, size_t len)
{
	CFDictionaryRef query, update, add;
	CFDataRef data;
	const void *ukey, *uval;
	OSStatus status;

	query = secret_query(name);
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

int fyai_secret_kernel_delete(const char *name)
{
	CFDictionaryRef query;
	OSStatus status;

	query = secret_query(name);
	if (!query)
		return -1;

	status = SecItemDelete(query);
	CFRelease(query);

	if (status == errSecItemNotFound)
		return FYAI_SECRET_NOT_FOUND;
	return status == errSecSuccess ? FYAI_SECRET_OK : -1;
}
#define FYAI_SECRET_BACKEND_NAME "macOS keychain"
#else
int fyai_secret_kernel_get(const char *name, char **value, size_t *len)
{
	(void)name; (void)value; (void)len;
	return FYAI_SECRET_UNSUPPORTED;
}
int fyai_secret_kernel_set(const char *name, const void *value, size_t len)
{
	(void)name; (void)value; (void)len;
	return FYAI_SECRET_UNSUPPORTED;
}
int fyai_secret_kernel_delete(const char *name)
{
	(void)name;
	return FYAI_SECRET_UNSUPPORTED;
}
#define FYAI_SECRET_BACKEND_NAME "unavailable"
#endif

int fyai_secret_action(enum fyai_secret_command command, const char *name,
		       bool stdin_value)
{
	const char *stored;
	char *value = NULL, *probe = NULL;
	size_t len = 0, cap = 0, probe_len = 0;
	FILE *fp = NULL;
	struct termios old, noecho;
	bool restore = false;
	int fd = -1, rc = -1;

	if (command == FYAI_SECRET_STATUS && (!name || !*name)) {
		rc = fyai_secret_kernel_get("fyai:__probe__", &probe, &probe_len);
		fyai_secret_clear_and_free(&probe, &probe_len);
		printf("secret backend: %s\n",
		       rc == FYAI_SECRET_UNSUPPORTED ? "unavailable" :
		       FYAI_SECRET_BACKEND_NAME);
		rc = rc == FYAI_SECRET_UNSUPPORTED ? -1 : 0;
		goto out;
	}
	stored = fy_sprintfa("fyai:%s", name);
	if (command == FYAI_SECRET_STATUS) {
		rc = fyai_secret_kernel_get(stored, &probe, &probe_len);
		fyai_secret_clear_and_free(&probe, &probe_len);
		printf("secret %s: %s\n", name,
		       rc == FYAI_SECRET_OK ? "present" :
		       rc == FYAI_SECRET_NOT_FOUND ? "absent" : "unavailable");
		rc = rc == FYAI_SECRET_UNSUPPORTED || rc < 0 ? -1 : 0;
		goto out;
	}
	if (command == FYAI_SECRET_DELETE) {
		rc = fyai_secret_kernel_delete(stored);
		if (rc == FYAI_SECRET_OK || rc == FYAI_SECRET_NOT_FOUND) {
			printf("secret: removed %s\n", name);
			rc = 0;
			goto out;
		}
		fprintf(stderr, "secret: %s backend is unavailable\n",
			FYAI_SECRET_BACKEND_NAME);
		rc = -1;
		goto out;
	}
	fp = stdin_value ? stdin : fopen("/dev/tty", "r+");
	if (!fp) {
		fprintf(stderr, "secret: cannot open /dev/tty; use --stdin\n");
		goto out;
	}
	fd = fileno(fp);
	if (!stdin_value && !tcgetattr(fd, &old)) {
		noecho = old;
		noecho.c_lflag &= ~ECHO;
		if (!tcsetattr(fd, TCSAFLUSH, &noecho))
			restore = true;
		fprintf(fp, "Secret %s: ", name);
		fflush(fp);
	}
	if (getline(&value, &cap, fp) >= 0)
		len = strcspn(value, "\r\n");
	if (restore) {
		(void)tcsetattr(fd, TCSAFLUSH, &old);
		restore = false;
		fprintf(fp, "\n");
	}
	if (!stdin_value) {
		fclose(fp);
		fp = NULL;
	}
	if (!len) {
		fprintf(stderr, "secret: empty value\n");
		rc = -1;
	} else {
		rc = fyai_secret_kernel_set(stored, value, len);
		if (rc == FYAI_SECRET_OK) {
			printf("secret: stored %s\n", name);
			rc = 0;
		} else {
			fprintf(stderr, "secret: %s backend is unavailable\n",
			FYAI_SECRET_BACKEND_NAME);
			rc = -1;
		}
	}
out:
	if (restore && fp)
		(void)tcsetattr(fd, TCSAFLUSH, &old);
	if (fp && !stdin_value)
		fclose(fp);
	fyai_secret_clear_and_free(&probe, &probe_len);
	fyai_secret_clear_and_free(&value, &cap);
	return rc;
}

int fyai_secret_execute(struct fyai_ctx *ctx)
{
	struct fyai_secret_args *a = &ctx->cfg->cmd.args.secret;

	return fyai_secret_action(a->command, a->name, a->stdin_value);
}
