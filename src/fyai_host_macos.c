/*
 * fyai_host_macos.c - host primitives of macOS
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_host_backend.h"

#ifdef FYAI_HOST_BACKEND_MACOS
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <util.h>

static ssize_t mac_executable(char *path, size_t capacity)
{
	char raw[PATH_MAX], resolved[PATH_MAX];
	uint32_t size = sizeof(raw);
	size_t length;
	int rc;

	rc = _NSGetExecutablePath(raw, &size);
	if (rc || !realpath(raw, resolved))
		return -1;
	length = strlen(resolved);
	if (length >= capacity) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(path, resolved, length + 1);
	return length;
}

static int mac_boot_id(char id[37])
{
	size_t size = 37;
	int rc;

	rc = sysctlbyname("kern.bootsessionuuid", id, &size, NULL, 0);
	if (rc)
		return -1;
	if (size != 37 || id[36]) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int mac_exec_self(const char *const argv[])
{
	char executable[PATH_MAX];
	uint32_t size = sizeof(executable);

	if (_NSGetExecutablePath(executable, &size)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return execv(executable, (char *const *)argv);
}

static int mac_openpty(int *master, int *slave)
{
	return openpty(master, slave, NULL, NULL, NULL);
}

static void mac_random(void *buf, size_t size)
{
	arc4random_buf(buf, size);
}

/* macOS takes the argument first, in the call and in the comparator. */
struct mac_sort {
	int (*compare)(const void *a, const void *b, void *arg);
	void *arg;
};

static int mac_sort_compare(void *context, const void *a, const void *b)
{
	struct mac_sort *sort = context;

	return sort->compare(a, b, sort->arg);
}

static void mac_sort(void *base, size_t count, size_t size,
		     int (*compare)(const void *a, const void *b, void *arg), void *arg)
{
	struct mac_sort sort = { compare, arg };

	qsort_r(base, count, size, &sort, mac_sort_compare);
}

const struct fyai_host_backend fyai_host_backend_macos = {
	.name = "macos",
	.exec_self = mac_exec_self,
	.executable = mac_executable,
	.boot_id = mac_boot_id,
	.random = mac_random,
	.openpty = mac_openpty,
	.sort = mac_sort,
	.open_url = "open",
};

#endif /* FYAI_HOST_BACKEND_MACOS */
