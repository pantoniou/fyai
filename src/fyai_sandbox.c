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

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fyai_cover.h"
#include "fyai_orphan.h"
#include "fyai_fsview.h"
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

/* True if @name of @len is one of the variables a tool may keep. */
static bool env_keep_name(const char *name, size_t len,
			  const char *const *keep)
{
	size_t i;

	for (; keep && *keep; keep++)
		if (!strncmp(name, *keep, len) && (*keep)[len] == '\0')
			return true;

	for (i = 0; i < sizeof(fyai_env_keep) / sizeof(*fyai_env_keep); i++)
		if (!strncmp(name, fyai_env_keep[i], len) &&
		    fyai_env_keep[i][len] == '\0')
			return true;
	/* Keep locale overrides (LC_*) too. */
	if (len >= 3 && !strncmp(name, "LC_", 3))
		return true;
	return false;
}

int fyai_env_sanitize(const char *const *keep)
{
	extern char **environ;
	char name[FYAI_ENV_NAME_MAX];
	const char *eq;
	size_t i, len;
	bool removed;
	int rc = 0;

	/* Avoid allocation in the child; restart after unsetenv() moves entries. */
	do {
		removed = false;
		for (i = 0; environ && environ[i]; i++) {
			eq = strchr(environ[i], '=');
			len = eq ? (size_t)(eq - environ[i]) :
				   strlen(environ[i]);
			if (env_keep_name(environ[i], len, keep))
				continue;
			if (len >= sizeof(name)) {
				/* Too long to name, so it cannot be removed. */
				rc = -1;
				continue;
			}
			memcpy(name, environ[i], len);
			name[len] = '\0';
			if (unsetenv(name) < 0) {
				rc = -1;
				continue;
			}
			removed = true;
			break;
		}
	} while (removed);

	return rc;
}

int fyai_sandbox_mode_parse(const char *name, enum fyai_sandbox_mode *modep)
{
	if (!name || !modep)
		return -1;
	if (!strcmp(name, "rw"))
		*modep = FYAI_SB_RW;
	else if (!strcmp(name, "ro"))
		*modep = FYAI_SB_RO;
	else if (!strcmp(name, "edit"))
		*modep = FYAI_SB_EDIT;
	else if (!strcmp(name, "append"))
		*modep = FYAI_SB_APPEND;
	else
		return -1;
	return 0;
}

/*
 * Landlock is Linux-only. On any other platform (notably macOS, which is a
 * supported target) this file compiles to the no-op stubs at the bottom, so
 * none of the Linux-specific headers or syscalls are referenced. A macOS
 * Seatbelt back-end (sandbox_init) would slot in behind the same three-function
 * interface.
 */
#if defined(__linux__) && __has_include(<linux/landlock.h>)
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <asm/unistd.h>
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

/* ABI 10 rights may be absent from the build host's kernel headers. */
#ifndef LANDLOCK_ACCESS_NET_BIND_UDP
#define LANDLOCK_ACCESS_NET_BIND_UDP (1ULL << 2)
#endif
#ifndef LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP
#define LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP (1ULL << 3)
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

/* Ancestors of denied paths retain only directory-listing access. */
#define FYAI_FS_DIR_LIST	LANDLOCK_ACCESS_FS_READ_DIR

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

	if (!rights)
		return 0;			/* nothing to grant */
	dfd = open(path, O_PATH | O_CLOEXEC);
	if (dfd < 0)
		return errno == ENOENT ? 0 : -1;	/* absent path: skip */
	/* A regular file may only carry file-applicable rights. */
	if (!fstat(dfd, &st) && !S_ISDIR(st.st_mode))
		rights &= FYAI_FS_FILE;
	/* An empty right set makes landlock_add_rule reject the rule. */
	if (!rights) {
		close(dfd);
		return 0;
	}
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
	/* Keep the listing right on the ancestor itself. */
	rc = grant_path(fd, dir, rights & FYAI_FS_DIR_LIST);
	if (rc) {
		closedir(d);
		return rc;
	}
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

#if defined(__x86_64__)
#define FYAI_SECCOMP_ARCH AUDIT_ARCH_X86_64
#define FYAI_SECCOMP_X32 __X32_SYSCALL_BIT
#elif defined(__aarch64__)
#define FYAI_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#define FYAI_SECCOMP_X32 0
#endif

#if defined(FYAI_SECCOMP_ARCH) && defined(__NR_socket) && defined(__NR_io_uring_setup)
#include "embedded_seccomp_udp.inc"
#endif

#ifdef FYAI_SECCOMP_UDP_GENERATED
#define FYAI_HAVE_SECCOMP_UDP 1
/*
 * fyai_seccomp_udp[] denies every datagram and raw IP socket, a syscall of
 * another architecture, and io_uring_setup(), because a ring creates sockets
 * without socket(). Unix and netlink sockets, and IP stream sockets, stay
 * available. A socket that the process inherited is not closed.
 */
static int seccomp_deny_udp(void)
{
	struct sock_fprog prog = {
		.len = sizeof(fyai_seccomp_udp) / sizeof(fyai_seccomp_udp[0]),
		.filter = fyai_seccomp_udp,
	};

	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}
#endif

bool fyai_sandbox_udp_deniable(void)
{
#ifdef FYAI_HAVE_SECCOMP_UDP
	/* EINVAL: the kernel has no seccomp. */
	return prctl(PR_GET_SECCOMP) >= 0;
#else
	return false;
#endif
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

bool fyai_sandbox_net_restrictable(int abi)
{
	if (abi < 0)
		abi = fyai_sandbox_abi();
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	return abi >= 4;
#else
	(void)abi;
	return false;
#endif
}

bool fyai_sandbox_udp_restrictable(int abi)
{
	if (abi < 0)
		abi = fyai_sandbox_abi();
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	return abi >= 10;
#else
	(void)abi;
	return false;
#endif
}

/* True when @path is @root or beneath it. */
static bool path_within(const char *path, const char *root)
{
	size_t n = strlen(root);

	while (n > 1 && root[n - 1] == '/')
		n--;
	if (n == 1 && root[0] == '/')
		return true;
	return !strncmp(path, root, n) && (path[n] == '/' || path[n] == '\0');
}

bool fyai_sandbox_path_visible(const struct fyai_sandbox_spec *spec,
			       const char *dir)
{
	const char *const *p;
	const char *s;
	size_t i;
	bool seen;

	if (!dir || dir[0] != '/')
		return false;
	for (s = dir; *s; s++)
		if (s[0] == '/' && s[1] == '.' && s[2] == '.' &&
		    (s[3] == '/' || s[3] == '\0'))
			return false;
	for (i = 0; i < spec->deny_n; i++)
		if (path_within(dir, spec->deny[i]))
			return false;
	for (i = 0; i < spec->hide_n; i++)
		if (path_within(dir, spec->hide[i]))
			return false;
	seen = false;
	for (p = fyai_sys_ro; *p && !seen; p++)
		seen = path_within(dir, *p);
	for (p = fyai_scratch_rw; *p && !seen; p++)
		seen = path_within(dir, *p);
	if (spec->project_root && path_within(dir, spec->project_root))
		seen = true;
	if (spec->scratch_root && path_within(dir, spec->scratch_root))
		seen = true;
	for (i = 0; i < spec->allow_n && !seen; i++)
		seen = path_within(dir, spec->allow[i].path);
	return seen;
}

/*
 * Rewrite PATH to the entries that the grants reach. The child owns its
 * environment, and the buffer is on the stack: no allocation after the fork.
 * Nothing left selects the system directories.
 */
static void sandbox_filter_path(const struct fyai_sandbox_spec *spec)
{
	static const char fallback[] = "/usr/local/bin:/usr/bin:/bin";
	char out[8192], dir[4096];
	const char *path = getenv("PATH"), *end;
	size_t len, used = 0;

	if (!path)
		return;
	out[0] = '\0';
	while (*path) {
		end = strchr(path, ':');
		len = end ? (size_t)(end - path) : strlen(path);
		if (len && len < sizeof(dir)) {
			memcpy(dir, path, len);
			dir[len] = '\0';
			if (fyai_sandbox_path_visible(spec, dir) &&
			    used + len + 2 <= sizeof(out)) {
				if (used)
					out[used++] = ':';
				memcpy(out + used, dir, len + 1);
				used += len;
			}
		}
		path = end ? end + 1 : path + len;
	}
	setenv("PATH", used ? out : fallback, 1);
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
	/* Fail closed when this build or kernel cannot restrict egress. */
	if ((spec->restrict_tcp || spec->restrict_tcp_bind) &&
	    !fyai_sandbox_net_restrictable(abi))
		return -1;
	if (spec->restrict_udp && !fyai_sandbox_udp_restrictable(abi))
		return -1;
	if (spec->deny_udp && !fyai_sandbox_udp_deniable())
		return -1;

	/* The cover comes first: the ruleset then sees the tree as the tool does. */
	if (spec->cover_holder > 0) {
		if (fyai_cover_join(spec->cover_holder, spec->mount, spec->mount_n))
			return -1;
	} else if ((spec->hide_n || spec->mount_n) &&
		   fyai_fsview_hide(spec->hide, spec->hide_n, spec->mount, spec->mount_n)) {
		return -1;
	}
	/* The processes below are in a namespace: their pids mean nothing outside it. */
	if (spec->cover_holder > 0 || spec->hide_n || spec->mount_n)
		fyai_orphan_namespaced();

	mask = fs_mask(abi);
	attr.handled_access_fs = mask;
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	if (spec->restrict_tcp)
		attr.handled_access_net |= LANDLOCK_ACCESS_NET_CONNECT_TCP;
	if (spec->restrict_tcp_bind)
		attr.handled_access_net |= LANDLOCK_ACCESS_NET_BIND_TCP;
	if (spec->restrict_udp)
		attr.handled_access_net |= LANDLOCK_ACCESS_NET_BIND_UDP |
					   LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP;
#endif

	fd = (int)ll_create_ruleset(&attr, sizeof(attr), 0);
	if (fd < 0)
		return -1;

	if (spec->read_all && grant_except(fd, "/", FYAI_FS_READ & mask,
					    spec->deny, spec->deny_global_n))
		goto out;

	if (spec->list_root &&
	    grant_path(fd, "/", FYAI_FS_DIR_LIST & mask))
		goto out;

	/* Carve configured denies out of every granted hierarchy. */
	for (p = fyai_sys_ro; *p; p++)
		if (grant_except(fd, *p, FYAI_FS_READ & mask,
				 spec->deny, spec->deny_global_n))
			goto out;
	for (p = fyai_scratch_rw; *p; p++)
		if (grant_except(fd, *p, mask, spec->deny, spec->deny_global_n))
			goto out;
	if (spec->scratch_root && grant_except(fd, spec->scratch_root, mask,
					    spec->deny, spec->deny_global_n))
		goto out;

	if (spec->project_root &&
	    grant_except(fd, spec->project_root, mask, spec->deny, spec->deny_n))
		goto out;

	/* An explicit allow wins over a deny, so it is granted whole. */
	for (i = 0; i < spec->allow_n; i++)
		if (grant_path(fd, spec->allow[i].path,
			       mode_rights(spec->allow[i].mode, mask)))
			goto out;

#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
	if (spec->restrict_tcp) {
		for (i = 0; i < spec->tcp_ports_n; i++) {
			np.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP;
			np.port = spec->tcp_ports[i];

			if (ll_add_rule(fd, LANDLOCK_RULE_NET_PORT, &np, 0))
				goto out;
		}
	}
	if (spec->restrict_tcp_bind) {
		for (i = 0; i < spec->tcp_bind_ports_n; i++) {
			np.allowed_access = LANDLOCK_ACCESS_NET_BIND_TCP;
			np.port = spec->tcp_bind_ports[i];
			if (ll_add_rule(fd, LANDLOCK_RULE_NET_PORT, &np, 0))
				goto out;
		}
	}
	if (spec->restrict_udp) {
		for (i = 0; i < spec->udp_ports_n; i++) {
			np.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP;
			np.port = spec->udp_ports[i];
			if (ll_add_rule(fd, LANDLOCK_RULE_NET_PORT, &np, 0))
				goto out;
		}
		if (spec->udp_ports_n) {
			np.allowed_access = LANDLOCK_ACCESS_NET_BIND_UDP;
			np.port = 0;
			if (ll_add_rule(fd, LANDLOCK_RULE_NET_PORT, &np, 0))
				goto out;
		}
		for (i = 0; i < spec->udp_bind_ports_n; i++) {
			np.allowed_access = LANDLOCK_ACCESS_NET_BIND_UDP;
			np.port = spec->udp_bind_ports[i];
			if (ll_add_rule(fd, LANDLOCK_RULE_NET_PORT, &np, 0))
				goto out;
		}
	}
#endif

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		goto out;
	if (ll_restrict_self(fd, 0))
		goto out;
#ifdef FYAI_HAVE_SECCOMP_UDP
	if (spec->deny_udp && seccomp_deny_udp())
		goto out;
#endif
	if (spec->filter_path)
		sandbox_filter_path(spec);
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

bool fyai_sandbox_net_restrictable(int abi)
{
	(void)abi;
	return false;
}

bool fyai_sandbox_udp_restrictable(int abi)
{
	(void)abi;
	return false;
}

bool fyai_sandbox_udp_deniable(void)
{
	return false;
}

bool fyai_sandbox_path_visible(const struct fyai_sandbox_spec *spec,
			       const char *dir)
{
	(void)spec;
	return dir && dir[0] == '/';
}

int fyai_sandbox_apply(const struct fyai_sandbox_spec *spec)
{
	/* No Landlock on this platform: fail-closed only in strict mode. */
	return spec->strict ? -1 : 0;
}

#endif /* FYAI_HAVE_LANDLOCK */
