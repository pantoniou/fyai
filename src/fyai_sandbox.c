/*
 * fyai_sandbox.c - Landlock confinement for tool sub-executions
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A ruleset is deny-by-default for every access *type* it handles: whatever
 * right goes in handled_access_fs/net is forbidden everywhere except the
 * paths/ports explicitly granted; unhandled rights are untouched. We probe the
 * kernel's ABI once and mask the handled set to it, since handling a right the
 * running kernel does not know makes landlock_restrict_self reject the ruleset.
 *
 * Deny is not a rule: a path is forbidden by not granting it. A deny-list under
 * an otherwise-granted project is honored by a carve-out walk (grant_except):
 * descend the project, granting whole any subtree that contains no denied path,
 * recursing into the one that does, and never granting a denied path itself.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fyai_sandbox.h"

/*
 * Environment variables preserved for a tool sub-execution. Everything else -
 * notably any <PROVIDER>_API_KEY and other secrets fyai reads - is dropped so
 * it cannot leak into a tool or the processes it spawns.
 */
static const char *const fyai_env_keep[] = {
	"PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TZ", "TMPDIR",
	"LANG", "LANGUAGE",
};

void fyai_env_sanitize(void)
{
	extern char **environ;
	char **names = NULL;
	size_t i, j, n = 0, cap = 0;
	char *eq;
	size_t len;
	size_t nc;
	char **nn;
	bool keep;

	/* Snapshot current names first: unsetenv() mutates environ in place. */
	for (i = 0; environ && environ[i]; i++) {
		eq = strchr(environ[i], '=');
		len = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);

		keep = false;
		for (j = 0; j < sizeof(fyai_env_keep) / sizeof(*fyai_env_keep); j++)
			if (!strncmp(environ[i], fyai_env_keep[j], len) &&
			    fyai_env_keep[j][len] == '\0') {
				keep = true;
				break;
			}
		/* Keep locale overrides (LC_*) too. */
		if (!keep && !strncmp(environ[i], "LC_", 3))
			keep = true;
		if (keep)
			continue;
		if (n == cap) {
			nc = cap ? cap * 2 : 16;
			nn = realloc(names, nc * sizeof(*names));

			if (!nn)
				break;
			names = nn;
			cap = nc;
		}
		names[n] = strndup(environ[i], len);
		if (names[n])
			n++;
	}
	for (i = 0; i < n; i++) {
		if (names[i]) {
			unsetenv(names[i]);
			free(names[i]);
		}
	}
	free(names);
}

enum fyai_sandbox_mode fyai_sandbox_mode_parse(const char *name)
{
	if (!name)
		return FYAI_SB_RW;
	if (!strcmp(name, "ro"))
		return FYAI_SB_RO;
	if (!strcmp(name, "edit"))
		return FYAI_SB_EDIT;
	if (!strcmp(name, "append"))
		return FYAI_SB_APPEND;
	return FYAI_SB_RW;	/* "rw" and anything unrecognized */
}

/*
 * Landlock is Linux-only. On any other platform (notably macOS, which is a
 * supported target) this file compiles to the no-op stubs at the bottom, so
 * none of the Linux-specific headers or syscalls are referenced. A macOS
 * Seatbelt back-end (sandbox_init) would slot in behind the same three-function
 * interface; see tool-exec-sandbox.md.
 */
#if defined(__linux__) && __has_include(<linux/landlock.h>)
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/landlock.h>
#define FYAI_HAVE_LANDLOCK 1
#endif

#ifdef FYAI_HAVE_LANDLOCK

#ifndef O_PATH
#define O_PATH 010000000
#endif

/* older kernels don't have this */
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE 0
#endif

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER 0
#endif

static long ll_create_ruleset(const struct landlock_ruleset_attr *attr,
			      size_t size, uint32_t flags)
{
	return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

static long ll_add_rule(int fd, enum landlock_rule_type type,
			const void *attr, uint32_t flags)
{
	return syscall(__NR_landlock_add_rule, fd, type, attr, flags);
}

static long ll_restrict_self(int fd, uint32_t flags)
{
	return syscall(__NR_landlock_restrict_self, fd, flags);
}

/* Read-side rights (also used to make a hierarchy executable). */
#define FYAI_FS_READ ( \
	LANDLOCK_ACCESS_FS_READ_FILE | \
	LANDLOCK_ACCESS_FS_READ_DIR | \
	LANDLOCK_ACCESS_FS_EXECUTE)

/* Rights that apply to a regular file. Granting a directory-only right (e.g.
 * READ_DIR or a MAKE_*) on a non-directory path makes landlock_add_rule reject
 * the rule with EINVAL, so file grants are masked to this set. */
#define FYAI_FS_FILE ( \
	LANDLOCK_ACCESS_FS_READ_FILE | \
	LANDLOCK_ACCESS_FS_WRITE_FILE | \
	LANDLOCK_ACCESS_FS_EXECUTE | \
	LANDLOCK_ACCESS_FS_TRUNCATE)

/* Directory-entry mutation: create/delete/rename beneath a directory. */
#define FYAI_FS_DIR_MUTATE ( \
	LANDLOCK_ACCESS_FS_REMOVE_DIR | \
	LANDLOCK_ACCESS_FS_REMOVE_FILE | \
	LANDLOCK_ACCESS_FS_MAKE_CHAR | \
	LANDLOCK_ACCESS_FS_MAKE_DIR | \
	LANDLOCK_ACCESS_FS_MAKE_REG | \
	LANDLOCK_ACCESS_FS_MAKE_SOCK | \
	LANDLOCK_ACCESS_FS_MAKE_FIFO | \
	LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
	LANDLOCK_ACCESS_FS_MAKE_SYM | \
	LANDLOCK_ACCESS_FS_REFER)

/* Resolve a mode to its right bitmask, then intersect with @mask (the running
 * ABI's handled set). RW is the full handled set. */
static uint64_t mode_rights(enum fyai_sandbox_mode mode, uint64_t mask)
{
	uint64_t r;

	switch (mode) {
	case FYAI_SB_RO:
		r = FYAI_FS_READ;
		break;
	case FYAI_SB_EDIT:	/* modify existing, no create/delete/rename */
		r = FYAI_FS_READ | LANDLOCK_ACCESS_FS_WRITE_FILE |
		    LANDLOCK_ACCESS_FS_TRUNCATE;
		break;
	case FYAI_SB_APPEND:	/* write existing without truncating */
		r = FYAI_FS_READ | LANDLOCK_ACCESS_FS_WRITE_FILE;
		break;
	case FYAI_SB_RW:
	default:
		r = FYAI_FS_READ | LANDLOCK_ACCESS_FS_WRITE_FILE |
		    LANDLOCK_ACCESS_FS_TRUNCATE | FYAI_FS_DIR_MUTATE;
		break;
	}
	return r & mask;
}

/*
 * The full write-side set, up to the right introduced at each ABI. Masked to
 * the running ABI in fs_mask() so a newer bit is never handled on an older
 * kernel. REFER arrived in ABI 2, TRUNCATE in ABI 3.
 */
static uint64_t fs_mask(int abi)
{
	uint64_t m = FYAI_FS_READ |
		     LANDLOCK_ACCESS_FS_WRITE_FILE |
		     LANDLOCK_ACCESS_FS_REMOVE_DIR |
		     LANDLOCK_ACCESS_FS_REMOVE_FILE |
		     LANDLOCK_ACCESS_FS_MAKE_CHAR |
		     LANDLOCK_ACCESS_FS_MAKE_DIR |
		     LANDLOCK_ACCESS_FS_MAKE_REG |
		     LANDLOCK_ACCESS_FS_MAKE_SOCK |
		     LANDLOCK_ACCESS_FS_MAKE_FIFO |
		     LANDLOCK_ACCESS_FS_MAKE_BLOCK |
		     LANDLOCK_ACCESS_FS_MAKE_SYM;

	if (abi >= 2)
		m |= LANDLOCK_ACCESS_FS_REFER;
	if (abi >= 3)
		m |= LANDLOCK_ACCESS_FS_TRUNCATE;
	return m;
}

/* Standard hierarchies a tool needs read/exec on to run at all. Missing ones
 * are skipped silently (grant_path tolerates ENOENT). */
static const char *const fyai_sys_ro[] = {
	"/usr", "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/libx32",
	"/etc", "/opt", "/proc", "/sys", "/run", NULL,
};

/*
 * Read/write scratch hierarchies outside the project. /dev is here (not in the
 * read-only set) because tools routinely write /dev/null, /dev/tty, etc.;
 * without write access a shell redirect to /dev/null fails.
 */
static const char *const fyai_scratch_rw[] = {
	"/tmp", "/var/tmp", "/dev", "/dev/shm", NULL,
};

static int grant_path(int fd, const char *path, uint64_t rights)
{
	struct landlock_path_beneath_attr pb = {0};
	struct stat st;
	int rc, dfd;

	dfd = open(path, O_PATH | O_CLOEXEC);
	if (dfd < 0)
		return errno == ENOENT ? 0 : -1;	/* absent path: skip */
	/* A regular file may only carry file-applicable rights. */
	if (!fstat(dfd, &st) && !S_ISDIR(st.st_mode))
		rights &= FYAI_FS_FILE;
	pb.allowed_access = rights;
	pb.parent_fd = dfd;
	rc = ll_add_rule(fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
	close(dfd);
	return rc ? -1 : 0;
}

/* True when @path is exactly one of the denied paths. */
static bool is_denied(const char *path, const char *const *deny, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!strcmp(path, deny[i]))
			return true;
	return false;
}

/* True when some denied path is strictly beneath @dir (dir + '/' prefix). */
static bool deny_under(const char *dir, const char *const *deny, size_t n)
{
	size_t dl = strlen(dir), i;

	for (i = 0; i < n; i++)
		if (!strncmp(deny[i], dir, dl) && deny[i][dl] == '/')
			return true;
	return false;
}

/*
 * Grant @dir with @rights, carving out any denied path beneath it. If no denied
 * path is under @dir it is granted whole (one rule, recursive by Landlock's
 * path-beneath semantics). Otherwise each child is handled individually:
 * skipped if denied, recursed into if it contains a denied path, else granted
 * whole. Bounded by directory depth toward the denied leaves, not by tree size.
 */
static int grant_except(int fd, const char *dir, uint64_t rights,
			const char *const *deny, size_t deny_n)
{
	struct dirent *de;
	char path[4096];
	DIR *d;
	int rc = 0;

	if (is_denied(dir, deny, deny_n))
		return 0;
	if (!deny_under(dir, deny, deny_n))
		return grant_path(fd, dir, rights);

	d = opendir(dir);
	if (!d)
		return errno == ENOENT ? 0 : grant_path(fd, dir, rights);
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir,
				     de->d_name) >= sizeof(path))
			continue;
		if (is_denied(path, deny, deny_n))
			continue;
		if (deny_under(path, deny, deny_n))
			rc = grant_except(fd, path, rights, deny, deny_n);
		else
			rc = grant_path(fd, path, rights);
		if (rc)
			break;
	}
	closedir(d);
	return rc;
}

int fyai_sandbox_abi(void)
{
	long v = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);

	if (v < 0)
		return errno == ENOSYS ? 0 : -1;
	return (int)v;
}

bool fyai_sandbox_available(void)
{
	return fyai_sandbox_abi() >= 1;
}

int fyai_sandbox_apply(const struct fyai_sandbox_spec *spec)
{
	struct landlock_ruleset_attr attr = {0};
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	struct landlock_net_port_attr np;
#endif
	uint64_t mask;
	const char *const *p;
	size_t i;
	int abi, fd, rc = -1;

	abi = fyai_sandbox_abi();
	if (abi < 1)
		return spec->strict ? -1 : 0;	/* unconfined: floor is elsewhere */
	if (spec->restrict_net && abi < 4 && spec->strict)
		return -1;			/* asked for egress limits we can't do */

	mask = fs_mask(abi);
	attr.handled_access_fs = mask;
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	if (spec->restrict_net && abi >= 4)
		attr.handled_access_net = LANDLOCK_ACCESS_NET_CONNECT_TCP;
#endif

	fd = (int)ll_create_ruleset(&attr, sizeof(attr), 0);
	if (fd < 0)
		return -1;

	for (p = fyai_sys_ro; *p; p++)
		if (grant_path(fd, *p, FYAI_FS_READ & mask))
			goto out;
	for (p = fyai_scratch_rw; *p; p++)
		if (grant_path(fd, *p, mask))
			goto out;

	if (spec->project_root &&
	    grant_except(fd, spec->project_root, mask, spec->deny, spec->deny_n))
		goto out;

	for (i = 0; i < spec->allow_n; i++)
		if (grant_path(fd, spec->allow[i].path,
			       mode_rights(spec->allow[i].mode, mask)))
			goto out;

#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	if (spec->restrict_net && abi >= 4) {
		for (i = 0; i < spec->ports_n; i++) {
			np.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP;
			np.port = spec->ports[i];

			if (ll_add_rule(fd, LANDLOCK_RULE_NET_PORT, &np, 0))
				goto out;
		}
	}
#endif

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		goto out;
	if (ll_restrict_self(fd, 0))
		goto out;
	rc = 0;
out:
	close(fd);
	return rc;
}

#else /* !FYAI_HAVE_LANDLOCK */

int fyai_sandbox_abi(void)
{
	return 0;
}

bool fyai_sandbox_available(void)
{
	return false;
}

int fyai_sandbox_apply(const struct fyai_sandbox_spec *spec)
{
	/* No Landlock on this platform: fail-closed only in strict mode. */
	return spec->strict ? -1 : 0;
}

#endif /* FYAI_HAVE_LANDLOCK */
