/*
 * fyai_home.c - the home directory of a sandboxed tool
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_host.h"
#include "fyai.h"
#include "fyai_cas.h"
#include "fyai_cmd.h"
#include "fyai_config.h"
#include "fyai_cover.h"
#include "fyai_diag.h"
#include "fyai_fsview.h"
#include "fyai_home.h"
#include "fyai_tools.h"

#define FYAI_MODULE FYAIEM_TOOLS

/* Deepest directory, most files and most bytes that a copy takes. */
#define HOME_DEPTH_MAX 16
#define HOME_FILES_MAX 20000
#define HOME_BYTES_MAX (512ULL << 20)

struct home_copy {
	struct fyai_cas_copy_state state;
	size_t files;
	unsigned long long bytes;
	bool full;
};

/* Copy one file: the blocks are shared where the file system can. */
static int home_copy_file(struct home_copy *hc, const char *src, const char *dst,
			  const struct stat *st)
{
	int sfd, dfd, rc;

	sfd = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (sfd < 0)
		return errno == ENOENT ? 0 : -1;
	dfd = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (dfd < 0) {
		/* A file that exists stays as it is. */
		rc = errno == EEXIST ? 0 : -1;
		close(sfd);
		return rc;
	}
	rc = fyai_cas_clone_state(sfd, dfd, (uint64_t)st->st_size, &hc->state);
	if (!rc)
		rc = fchmod(dfd, st->st_mode & 0777);
	close(sfd);
	if (close(dfd) && !rc)
		rc = -1;
	if (rc)
		(void)unlink(dst);
	return rc;
}

static int home_copy_tree(struct home_copy *hc, const char *src, const char *dst,
			  unsigned int depth)
{
	char from[PATH_MAX], to[PATH_MAX], target[PATH_MAX];
	struct dirent *de;
	struct stat st;
	ssize_t n;
	DIR *d;
	int rc = 0;

	if (lstat(src, &st))
		return errno == ENOENT ? 0 : -1;
	if (S_ISLNK(st.st_mode)) {
		n = readlink(src, target, sizeof(target) - 1);
		if (n < 0)
			return -1;
		target[n] = '\0';
		return symlink(target, dst) && errno != EEXIST ? -1 : 0;
	}
	if (S_ISREG(st.st_mode)) {
		if (hc->files >= HOME_FILES_MAX ||
		    hc->bytes + (unsigned long long)st.st_size > HOME_BYTES_MAX) {
			hc->full = true;
			return 0;
		}
		hc->files++;
		hc->bytes += (unsigned long long)st.st_size;
		return home_copy_file(hc, src, dst, &st);
	}
	if (!S_ISDIR(st.st_mode) || depth >= HOME_DEPTH_MAX)
		return 0;
	if (mkdir(dst, 0700) && errno != EEXIST)
		return -1;
	d = opendir(src);
	if (!d)
		return -1;
	while (!rc && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if ((size_t)snprintf(from, sizeof(from), "%s/%s", src, de->d_name) >=
			    sizeof(from) ||
		    (size_t)snprintf(to, sizeof(to), "%s/%s", dst, de->d_name) >=
			    sizeof(to))
			continue;
		rc = home_copy_tree(hc, from, to, depth + 1);
	}
	closedir(d);
	return rc;
}

/* Make @path and the directories above it, private to the user. */
static int home_mkdirs(const char *path)
{
	char buf[PATH_MAX], *p;

	if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
		return -1;
	for (p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0700) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(buf, 0700) && errno != EEXIST ? -1 : 0;
}

/* Make the directories above @path. */
static int home_mkparent(const char *path)
{
	char parent[PATH_MAX], *slash;

	if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
		return -1;
	slash = strrchr(parent, '/');
	if (!slash || slash == parent)
		return 0;
	*slash = '\0';
	return home_mkdirs(parent);
}

/* Make the file @path, empty, and the directories above it. */
static int home_mkfile(const char *path)
{
	int fd;

	if (home_mkparent(path))
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	return close(fd);
}

/* The string, kept by @h until it is released. Returns NULL when there is no room. */
static const char *home_keep(struct fyai_home *h, char *s)
{
	char **grown;

	if (!s)
		return NULL;
	grown = realloc(h->strings, (h->strings_n + 1) * sizeof(*grown));
	if (!grown) {
		free(s);
		return NULL;
	}
	h->strings = grown;
	h->strings[h->strings_n++] = s;
	return s;
}

static const char *home_keepf(struct fyai_home *h, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static const char *home_keepf(struct fyai_home *h, const char *fmt, ...)
{
	va_list ap;
	char *s = NULL;
	int rc;

	va_start(ap, fmt);
	rc = vasprintf(&s, fmt, ap);
	va_end(ap);
	return rc < 0 ? NULL : home_keep(h, s);
}

/* Append a mount; the strings stay with @h. */
static int home_mount_add(struct fyai_home *h, enum fyai_sandbox_mount_kind kind,
			  const char *source, const char *target, const char *upper,
			  const char *work, bool readonly)
{
	struct fyai_sandbox_mount *grown;

	/* The caller frees what it passes: the mount keeps copies. */
	source = home_keepf(h, "%s", source);
	target = home_keepf(h, "%s", target);
	upper = upper ? home_keepf(h, "%s", upper) : NULL;
	work = work ? home_keepf(h, "%s", work) : NULL;
	if (!source || !target || (upper && !upper[0]))
		return -1;
	grown = realloc(h->mount, (h->mount_n + 1) * sizeof(*grown));
	if (!grown)
		return -1;
	h->mount = grown;
	h->mount[h->mount_n++] = (struct fyai_sandbox_mount){
		.kind = kind, .source = source, .target = target, .upper = upper,
		.work = work, .readonly = readonly,
	};
	return 0;
}

/* Add an overlay of @source at @target, with its upper layer in the private directory. */
static int home_overlay_add(struct fyai_home *h, const char *source, const char *target)
{
	const char *upper, *work;

	upper = home_keepf(h, "%s/.fyai-overlay/%zu/upper", h->dir, h->mount_n);
	work = home_keepf(h, "%s/.fyai-overlay/%zu/work", h->dir, h->mount_n);
	if (!upper || !work || home_mkdirs(upper) || home_mkdirs(work) || home_mkdirs(target))
		return -1;
	return home_mount_add(h, FYAI_MOUNT_OVERLAY, source, target, upper, work, false);
}

/* Whether the mount options of an overlay can carry this path. */
static bool home_overlay_path_ok(const char *path)
{
	return !strpbrk(path, ",:\\");
}

/* What the entry names, and how it is made private. */
struct home_entry {
	struct fyai_ctx *ctx;
	struct fyai_home *h;
	struct home_copy *hc;
	const char *real;	/* the real home */
	bool overlay;		/* a directory is an overlay, else a copy */
	char *const *deny;
	size_t deny_n;
};

static bool home_denied(const struct home_entry *e, const char *path)
{
	size_t i;

	for (i = 0; i < e->deny_n; i++)
		if (fyai_path_beneath(path, e->deny[i]))
			return true;
	return false;
}

/*
 * Give the tools a private version of @src at the place where it is. Under the
 * real home it goes into the private tree, which is mounted over the home later.
 * Elsewhere it is mounted over @src itself. Returns 0, or -1 with errno set.
 */
static int home_entry_place(struct home_entry *e, const char *src)
{
	struct fyai_home *h = e->h;
	struct stat st;
	const char *target, *slash;
	bool under;

	if (stat(src, &st))
		return 0;
	under = strcmp(e->real, "/") && fyai_path_beneath(src, e->real) && strcmp(src, e->real);
	if (under) {
		target = home_keepf(h, "%s%s", h->dir, src + strlen(e->real));
	} else {
		slash = strrchr(src, '/');
		target = home_keepf(h, "%s/ext/%zu/%s", h->dir, h->mount_n,
				    slash ? slash + 1 : src);
	}
	if (!target)
		return -1;
	if (S_ISDIR(st.st_mode) && e->overlay && home_overlay_path_ok(src) &&
	    home_overlay_path_ok(h->dir)) {
		if (under)
			return home_overlay_add(h, src, target);
		/* The overlay shows the original at its own place. */
		return home_overlay_add(h, src, src);
	}
	if (S_ISDIR(st.st_mode) ? home_mkdirs(target) : home_mkparent(target))
		return -1;
	if (home_copy_tree(e->hc, src, target, 0))
		return -1;
	if (under)
		return 0;
	return home_mount_add(h, FYAI_MOUNT_BIND, target, src, NULL, NULL, false);
}

/* Show a path that the sandbox grants under the home inside the private tree. */
static int home_rebind(struct home_entry *e, const char *path)
{
	struct stat st;
	const char *target;

	if (!strcmp(e->real, "/") || !fyai_path_beneath(path, e->real) || !strcmp(path, e->real) ||
	    stat(path, &st))
		return 0;
	target = home_keepf(e->h, "%s%s", e->h->dir, path + strlen(e->real));
	if (!target)
		return -1;
	/* A private version of the same path is there already. */
	if (!access(target, F_OK))
		return 0;
	if (S_ISDIR(st.st_mode) ? home_mkdirs(target) : home_mkfile(target))
		return -1;
	return home_mount_add(e->h, FYAI_MOUNT_BIND, path, target, NULL, NULL, false);
}

static void home_remove_tree(const char *path, unsigned int depth);

static void home_exit_remove(void *dir)
{
	home_remove_tree(dir, 0);
}

/* Make the directory of the home: the one that `dir` names, else a private one in $TMPDIR. */
static int home_dir_make(struct home_entry *e, fy_generic home, const char *root,
			 bool private)
{
	struct fyai_ctx *ctx = e->ctx;
	fy_generic dir = fy_get(home, "dir", fy_invalid);
	const char *tmp;
	char *path;
	int r;

	if (private && fy_is_string(dir)) {
		path = fyai_sandbox_resolve_path(root, fy_castp(&dir, ""));
		fyai_error_check(ctx, path, err_out,
				 "sandbox: could not resolve the home directory");
		if (home_mkdirs(path)) {
			fyai_error(ctx, "sandbox: could not make the home directory '%s': %s",
				   path, strerror(errno));
			free(path);
			return -1;
		}
	} else {
		tmp = getenv("TMPDIR");
		if (!tmp || !*tmp)
			tmp = "/tmp";
		r = asprintf(&path, "%s/fyai-home-XXXXXX", tmp);
		fyai_error_check(ctx, r >= 0, err_out,
				 "sandbox: could not name the home directory");
		if (!mkdtemp(path)) {
			fyai_error(ctx, "sandbox: could not make a home directory in '%s': %s",
				   tmp, strerror(errno));
			free(path);
			return -1;
		}
		e->h->owned = true;
	}
	e->h->dir = path;
	return 0;
err_out:
	return -1;
}

/* Keep the paths that a copy must not give: they are denied in the sandbox. */
static int home_deny_load(struct home_entry *e, const char *root)
{
	fy_generic deny = fy_get(e->ctx->cfg->sandbox, "deny", fy_invalid), it;
	const char *ps;
	size_t n = 0;

	e->deny = calloc(fy_is_sequence(deny) ? fy_len(deny) + 1 : 1, sizeof(*e->deny));
	fyai_error_check(e->ctx, e->deny, err_out, "sandbox: could not allocate the deny list");
	fy_foreach(it, deny) {
		ps = fy_castp(&it, "");
		((char **)e->deny)[n] = fyai_sandbox_resolve_path(root, ps);
		if (e->deny[n])
			n++;
	}
	e->deny_n = n;
	return 0;
err_out:
	return -1;
}

/*
 * The name and the value of an entry of `env`: "NAME=PATH", or "NAME" to take the
 * path from the environment. Returns the path, or NULL when the entry gives none.
 */
static const char *home_env_value(const char *entry, char *name, size_t size)
{
	const char *eq = strchr(entry, '=');
	size_t len = eq ? (size_t)(eq - entry) : strlen(entry);
	const char *value;

	if (!len || len >= size)
		return NULL;
	memcpy(name, entry, len);
	name[len] = '\0';
	value = eq ? eq + 1 : getenv(name);
	return value && *value ? value : NULL;
}

/* Give the tool a private version of each path of `copy` and of each variable of `env`. */
static int home_copy_entries(struct home_entry *e, fy_generic home, const char *root)
{
	struct fyai_ctx *ctx = e->ctx;
	char name[FYAI_ENV_NAME_MAX];
	const char *ps, *value;
	fy_generic it;
	char *res;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		fy_foreach(it, fy_get(home, pass ? "env" : "copy", fy_invalid)) {
			ps = fy_castp(&it, "");
			value = pass ? home_env_value(ps, name, sizeof(name)) : ps;
			if (!value)
				continue;
			res = fyai_sandbox_resolve_path(root, value);
			fyai_error_check(ctx, res, err_out,
					 "sandbox: could not resolve the home path '%s'", ps);
			if (home_denied(e, res)) {
				fyai_warning(ctx, "sandbox: '%s' is denied and is not given to "
					     "the home directory", ps);
				free(res);
				continue;
			}
			if (home_entry_place(e, res))
				fyai_warning(ctx, "sandbox: could not give '%s' to the home "
					     "directory: %s", ps, strerror(errno));
			free(res);
		}
	}
	return 0;
err_out:
	return -1;
}

/* What the sandbox grants under the home, the project and the scratch directory stay visible. */
static int home_show_granted(struct home_entry *e, const char *root)
{
	struct fyai_ctx *ctx = e->ctx;
	fy_generic it, pv;
	char *res;

	fy_foreach(it, fy_get(ctx->cfg->sandbox, "allow", fy_invalid)) {
		pv = fy_is_mapping(it) ? fy_get(it, "path", fy_invalid) : it;
		res = fyai_sandbox_resolve_path(root, fy_castp(&pv, ""));
		if (res && home_rebind(e, res)) {
			free(res);
			fyai_error(ctx, "sandbox: could not show an allowed path in the home "
				   "directory: %s", strerror(errno));
			return -1;
		}
		free(res);
	}
	res = root ? strdup(root) : NULL;
	if (res && home_rebind(e, res)) {
		free(res);
		fyai_error(ctx, "sandbox: could not show the project in the home "
			   "directory: %s", strerror(errno));
		return -1;
	}
	free(res);
	if (ctx->cfg->view_scratch && home_rebind(e, ctx->cfg->view_scratch)) {
		fyai_error(ctx, "sandbox: could not show the scratch directory in the "
			   "home directory: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* The mounts of the profile follow the home, so a target under the home lands in it. */
static int home_profile_mounts(struct home_entry *e, fy_generic mounts, const char *root)
{
	struct fyai_ctx *ctx = e->ctx;
	const char *ps, *src, *target;
	fy_generic it, kind, v;
	char *res, *res2;
	int r;

	fy_foreach(it, mounts) {
		kind = fy_get(it, "kind", fy_invalid);
		v = fy_get(it, "source", fy_invalid);
		ps = fy_castp(&v, "");
		res = fyai_sandbox_resolve_path(root, ps);
		v = fy_get(it, "target", fy_invalid);
		res2 = fyai_sandbox_resolve_path(root, fy_is_string(v) ? fy_castp(&v, "") : ps);
		fyai_error_check(ctx, res && res2, err_out,
				 "sandbox: could not resolve the mount of '%s'", ps);
		src = home_keep(e->h, res);
		target = home_keep(e->h, res2);
		fyai_error_check(ctx, src && target, err_out,
				 "sandbox: could not keep the mount");
		if (home_denied(e, src)) {
			fyai_warning(ctx, "sandbox: '%s' is denied and is not mounted", src);
			continue;
		}
		if (access(src, F_OK) || access(target, F_OK)) {
			fyai_warning(ctx, "sandbox: '%s' or '%s' does not exist: not mounted",
				     src, target);
			continue;
		}
		if (fy_equal(kind, "overlay")) {
			r = home_overlay_add(e->h, src, target);
		} else {
			r = home_mount_add(e->h, FYAI_MOUNT_BIND, src, target, NULL, NULL,
					   !strcmp(fy_get(it, "mode", "rw"), "ro"));
		}
		fyai_error_check(ctx, !r, err_out, "sandbox: could not add the mount of '%s'", src);
	}
	return 0;
err_out:
	return -1;
}

/* The paths of /proc that change the kernel: read-only below the write grant. */
static const char *const home_proc_masked[] = {
	"/proc/sys", "/proc/sysrq-trigger", "/proc/irq", "/proc/bus", "/proc/fs",
};

/* True when the profile exposes a user namespace to the tool. */
static bool home_wants_userns(const struct fyai_ctx *ctx)
{
	fy_generic it;

	fy_foreach(it, fy_get(ctx->cfg->sandbox, "expose", fy_invalid))
		if (fy_equal(it, "userns"))
			return true;
	return false;
}

/* Mount the paths of /proc that change the kernel read-only, after /proc is mounted. */
static int home_mask_proc(struct home_entry *e)
{
	size_t i;
	int r;

	e->h->userns = true;
	for (i = 0; i < ARRAY_SIZE(home_proc_masked); i++) {
		if (access(home_proc_masked[i], F_OK))
			continue;
		r = home_mount_add(e->h, FYAI_MOUNT_BIND, home_proc_masked[i],
				   home_proc_masked[i], NULL, NULL, true);
		if (!r)
			e->h->mount[e->h->mount_n - 1].late = true;
		fyai_error_check(e->ctx, !r, err_out, "sandbox: could not add the mount of '%s'",
				 home_proc_masked[i]);
	}
	return 0;
err_out:
	return -1;
}

int fyai_home_ensure(struct fyai_ctx *ctx)
{
	struct home_copy hc = { .state = { .backend = FYAI_CAS_METHOD_NONE } };
	struct home_entry e = { .ctx = ctx, .hc = &hc };
	fy_generic home, mounts;
	char *root = NULL, *real_copy = NULL;
	struct fyai_home *h = NULL;
	bool private, userns;
	size_t i;
	int rc = -1, r;

	if (ctx->home || ctx->cfg->tool_child || !ctx->cfg->enable_sandbox)
		return 0;
	home = fy_get(ctx->cfg->sandbox, "home", fy_invalid);
	mounts = fy_get(ctx->cfg->sandbox, "mounts", fy_invalid);
	private = fy_get(home, "private", false);
	userns = home_wants_userns(ctx);
	if (!private && !userns && !(fy_is_sequence(mounts) && fy_len(mounts)))
		return 0;

	h = calloc(1, sizeof(*h));
	fyai_error_check(ctx, h, err_out, "sandbox: could not allocate the mounts");
	root = fyai_discover_project_root();
	real_copy = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	fyai_error_check(ctx, !private || (real_copy && real_copy[0] == '/'), err_out,
			 "sandbox: a private home needs $HOME to name the real one");
	e.real = real_copy ? real_copy : "/";
	e.h = h;
	e.overlay = strcmp(fy_get(home, "method", "overlay"), "copy") != 0;

	if (home_dir_make(&e, home, root, private))
		goto err_out;
	ctx->home = h;
	/* The tool verb runs in the namespace, so the process that waits for it removes the tree. */
	if (h->owned && ctx->cfg->cmd.reg.path && !strncmp(ctx->cfg->cmd.reg.path, "tool", 4))
		fyai_fsview_on_parent_exit(home_exit_remove, h->dir);

	/* A copy of a denied file would give the tool what the deny keeps from it. */
	if (home_deny_load(&e, root))
		goto err_out;
	if (private) {
		if (home_copy_entries(&e, home, root) || home_show_granted(&e, root))
			goto err_out;
		/* The private tree is the home. */
		r = home_mount_add(h, FYAI_MOUNT_BIND, h->dir, e.real, NULL, NULL, false);
		fyai_error_check(ctx, !r, err_out, "sandbox: could not add the home mount");
	}
	if (home_profile_mounts(&e, mounts, root) || (userns && home_mask_proc(&e)))
		goto err_out;
	if (hc.full)
		fyai_warning(ctx, "sandbox: the copy to the home directory stopped at "
			     "%d files or %d MiB", HOME_FILES_MAX,
			     (int)(HOME_BYTES_MAX >> 20));
	rc = 0;
err_out:
	for (i = 0; i < e.deny_n; i++)
		free(e.deny[i]);
	free((char **)e.deny);
	free(root);
	free(real_copy);
	if (rc) {
		if (ctx->home)
			fyai_home_release(ctx);
		else if (h)
			free(h);
	}
	return rc ? -1 : 0;
}

/*
 * True when @path is a mount root, or when the kernel cannot say. Only Linux
 * makes the mounts of a home, so another system has none to find.
 */
static bool home_is_mount_root(const char *path)
{
	return fyai_host_mount_root(path);
}

/*
 * Remove the tree at @path. An overlay leaves a directory that nobody may open
 * (its work directory), so each one gets its access back first.
 */
static void home_remove_tree(const char *path, unsigned int depth)
{
	char child[PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d = NULL;
	int tries;

	if (lstat(path, &st))
		return;
	if (!S_ISDIR(st.st_mode) || depth >= 64) {
		(void)unlink(path);
		return;
	}
	/*
	 * Never remove through a mount: a bind shows the files of the project. Only
	 * a tree that no mount covers is emptied.
	 */
	if (home_is_mount_root(path))
		goto out;
	(void)chmod(path, 0700);
	d = opendir(path);
	if (d) {
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			if ((size_t)snprintf(child, sizeof(child), "%s/%s", path,
					     de->d_name) < sizeof(child))
				home_remove_tree(child, depth + 1);
		}
		closedir(d);
	}
out:
	/* The kernel lets go of a mount a moment after its last process ends. */
	for (tries = 0; rmdir(path) && errno == EBUSY && tries < 100; tries++)
		sched_yield();
}

void fyai_home_release(struct fyai_ctx *ctx)
{
	struct fyai_home *h;

	if (!ctx || !ctx->home)
		return;
	h = ctx->home;
	/* A forked child holds a copy of the record, and leaves the directory to its owner. */
	if (ctx->cfg->tool_child) {
		fyai_home_abandon(ctx);
		return;
	}
	/* A mount point can go only when the namespace that mounted on it is gone. */
	if (h->mount_n)
		fyai_cover_drain(&ctx->cover);
	/*
	 * A process that is inside the namespace does not remove the tree: its
	 * mounts show the project. The process that waits for it does.
	 */
	if (h->owned && !ctx->sandbox_applied)
		home_remove_tree(h->dir, 0);
	fyai_home_abandon(ctx);
}

void fyai_home_abandon(struct fyai_ctx *ctx)
{
	struct fyai_home *h;
	size_t i;

	if (!ctx || !ctx->home)
		return;
	h = ctx->home;
	for (i = 0; i < h->strings_n; i++)
		free(h->strings[i]);
	free(h->strings);
	free(h->mount);
	free(h->dir);
	free(h);
	ctx->home = NULL;
}
