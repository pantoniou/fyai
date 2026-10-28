/*
 * fyai_fsview_linux.c - OverlayFS views of a captured project
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The Linux backend of the views (fyai_fsview_backend.h).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_fsview_backend.h"
#include "fyai_project_capture.h"
#include "fyai_scratch.h"
#include "fyai_wire.h"
#include "fyai_sandbox.h"

#ifdef FYAI_FSVIEW_BACKEND_LINUX

static fy_generic linux_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
				 char *error, size_t error_size);
#include <sched.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <dirent.h>
#include <libfyaml/libfyaml-blake3.h>
#include <sys/xattr.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/mount.h>
#include <linux/securebits.h>
/* The path of the node that is being verified, as bytes joined by '/'. */
struct view_walk {
	unsigned char bytes[PATH_MAX * 2];
	size_t length;
};

static int view_verify_lower(const struct fyai_fsview *view, const struct fyai_manifest *manifest,
			     int parent, const char *name,
			     const unsigned char digest[FYAI_CAS_HASH_SIZE], struct view_walk *walk,
			     int objects, int data, struct fy_blake3_hasher *hasher,
			     unsigned int depth, size_t *count)
{
	struct fyai_mobject object;
	struct fyai_mdir iterator;
	struct fyai_project_entry entry;
	struct stat st;
	char child[NAME_MAX + 1], actual[PATH_MAX];
	char redirect[FYAI_CAS_REDIRECT_SIZE];
	int64_t sec;
	uint32_t nsec;
	size_t mark, children = 0;
	DIR *directory;
	struct dirent *item;
	ssize_t amount;
	int fd = -1, payload = -1, rc = -1, saved;

	if (depth > 128 || ++*count > 100001 || !fyai_manifest_find(manifest, digest, &object)) {
		errno = EINVAL;
		return -1;
	}
	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW))
		return -1;
	if (!fyai_manifest_attribute(manifest, walk->bytes, walk->length, &sec, &nsec)) {
		sec = object.meta.mtime_sec;
		nsec = object.meta.mtime_nsec;
	}
	if ((st.st_mode & 07777) != object.meta.mode || st.st_uid != object.meta.uid ||
	    st.st_gid != object.meta.gid || st.st_mtim.tv_sec != sec ||
	    (uint32_t)st.st_mtim.tv_nsec != nsec) {
		errno = EIO;
		return -1;
	}
	if (object.kind == FYAI_PROJECT_SYMLINK) {
		if (!S_ISLNK(st.st_mode)) {
			errno = EIO;
			return -1;
		}
		amount = readlinkat(parent, name, actual, sizeof(actual));
		if (amount < 0)
			return -1;
		if ((size_t)amount != object.target_length || memcmp(actual, object.target, amount)) {
			errno = EIO;
			return -1;
		}
		return 0;
	}
	fd = openat(parent, name,
		    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
			    (object.kind == FYAI_PROJECT_DIRECTORY ? O_DIRECTORY : 0));
	if (fd < 0)
		return -1;
	if (object.kind == FYAI_PROJECT_DIRECTORY) {
		directory = fdopendir(dup(fd));
		if (!directory)
			goto out;
		errno = 0;
		while ((item = readdir(directory))) {
			if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..") ||
			    (!depth && !strcmp(item->d_name, ".fyai")))
				continue;
			children++;
		}
		saved = errno;
		closedir(directory);
		if (saved || children != object.entry_count || fyai_mdir_open(&object, &iterator)) {
			errno = saved ? saved : EIO;
			goto out;
		}
		while (fyai_mdir_next(&iterator, &entry)) {
			if (!entry.name_length || entry.name_length > NAME_MAX ||
			    memchr(entry.name, '/', entry.name_length) ||
			    memchr(entry.name, 0, entry.name_length) ||
			    (entry.name_length == 1 && entry.name[0] == '.') ||
			    (entry.name_length == 2 && !memcmp(entry.name, "..", 2)) ||
			    walk->length + 1 + entry.name_length > sizeof(walk->bytes)) {
				errno = EINVAL;
				goto out;
			}
			memcpy(child, entry.name, entry.name_length);
			child[entry.name_length] = '\0';
			mark = walk->length;
			if (walk->length)
				walk->bytes[walk->length++] = '/';
			memcpy(walk->bytes + walk->length, entry.name, entry.name_length);
			walk->length += entry.name_length;
			rc = view_verify_lower(view, manifest, fd, child, entry.digest, walk, objects,
					       data, hasher, depth + 1, count);
			walk->length = mark;
			if (rc)
				goto out;
		}
		rc = 0;
		goto out;
	}
	if (object.kind != FYAI_PROJECT_FILE || !S_ISREG(st.st_mode)) {
		errno = EIO;
		goto out;
	}
	rc = fyai_cas_verify_hasher(objects, &object.blob, hasher);
	if (rc)
		goto out;
	if (!view->metacopy) {
		rc = fyai_cas_verify_file(fd, &object.blob, hasher);
		goto out;
	}
	fyai_cas_redirect(redirect, &object.blob);
	amount = fgetxattr(fd, "user.overlay.redirect", actual, sizeof(actual));
	if (st.st_size != (off_t)object.blob.size || amount != (ssize_t)strlen(redirect) ||
	    memcmp(actual, redirect, amount > 0 ? (size_t)amount : 0) ||
	    fgetxattr(fd, "user.overlay.metacopy", NULL, 0) != 0) {
		errno = EIO;
		rc = -1;
		goto out;
	}
	payload = openat(data, redirect + 1, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	rc = payload < 0 ? -1 : fyai_cas_verify_file(payload, &object.blob, hasher);
out:
	saved = errno;
	if (payload >= 0)
		close(payload);
	close(fd);
	errno = saved;
	return rc;
}

static int linux_recover(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			fy_generic expected, char *error, size_t error_size)
{
	struct fy_blake3_hasher_cfg cfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher = NULL;
	struct fyai_fsview recovery = *view;
	struct fyai_manifest baseline = { 0 }, wanted = { 0 }, found = { 0 };
	struct view_walk *walk = NULL;
	fy_generic result;
	const char *path;
	size_t count = 0;
	int runtime = -1, objects = -1, data = -1, rc = -1, saved;

	if (fyai_fsview_manifest_open(view->storage, view->baseline, &baseline) ||
	    fyai_fsview_manifest_open(view->storage, expected, &wanted))
		goto out;
	walk = calloc(1, sizeof(*walk));
	if (!walk) {
		errno = ENOMEM;
		goto out;
	}
	runtime = open(view->runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (runtime < 0)
		goto out;
	path = fy_sprintfa("%s/objects/blake3", view->storage);
	objects = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (objects < 0)
		goto out;
	if (view->metacopy) {
		data = openat(runtime, "data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (data < 0)
			goto out;
	}
	hasher = fy_blake3_hasher_create(&cfg);
	if (!hasher) {
		errno = ENOMEM;
		goto out;
	}
	rc = view_verify_lower(view, &baseline, runtime, "baseline", baseline.root, walk, objects,
			       data, hasher, 0, &count);
	if (rc)
		goto out;
	recovery.verify = true;
	recovery.lazy = true;
	result = linux_snapshot(gb, &recovery, error, error_size);
	if (!fy_is_mapping(result) || fyai_fsview_manifest_open(view->storage, result, &found) ||
	    !fyai_manifest_equal(&found, &wanted)) {
		if (fy_is_mapping(result) || !errno)
			errno = EIO;
		rc = -1;
	}
out:
	saved = errno;
	if (hasher)
		fy_blake3_hasher_destroy(hasher);
	if (data >= 0)
		close(data);
	if (objects >= 0)
		close(objects);
	if (runtime >= 0)
		close(runtime);
	free(walk);
	fyai_manifest_close(&found);
	fyai_manifest_close(&wanted);
	fyai_manifest_close(&baseline);
	errno = saved;
	return rc;
}

static int view_write_file(const char *path, const char *value)
{
	int fd, rc = 0, saved;
	ssize_t written;
	size_t length = strlen(value);

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	written = write(fd, value, length);
	if (written != (ssize_t)length) {
		if (written >= 0)
			errno = EIO;
		rc = -1;
	}
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

static int view_namespace(bool pid_namespace)
{
	/* The identity must be read before unshare() maps it to the overflow ID. */
	uid_t uid = getuid();
	gid_t gid = getgid();
	int rc;

	rc = unshare(CLONE_NEWUSER | CLONE_NEWNS | (pid_namespace ? CLONE_NEWPID : 0));
	if (rc)
		return -1;
	rc = view_write_file("/proc/self/setgroups", "deny\n");
	if (rc)
		return -1;
	rc = view_write_file("/proc/self/uid_map",
			     fy_sprintfa("%u %u 1\n", uid, uid));
	if (rc)
		return -1;
	rc = view_write_file("/proc/self/gid_map",
			     fy_sprintfa("%u %u 1\n", gid, gid));
	if (rc)
		return -1;
	return mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
}

static const char *view_overlay_options(bool metacopy)
{
	return metacopy ? "lowerdir=baseline::data,upperdir=upper,workdir=work,"
			  "userxattr,index=off" :
			  "lowerdir=baseline,upperdir=upper,workdir=work,"
			  "userxattr,index=off,redirect_dir=nofollow,metacopy=off";
}

static int view_overlay(const char *target, bool metacopy)
{
	return mount("overlay", target, "overlay", MS_NOSUID | MS_NODEV,
		     view_overlay_options(metacopy));
}

static int view_metacopy_probe(void)
{
	static const char bytes[] = "fyai CAS probe";
	char buffer[sizeof(bytes)];
	struct stat st;
	int data = -1, metadata = -1, merged = -1, rc = -1;
	bool mounted = false;

	data = open("data/.probe", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	metadata = open("baseline/.probe", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (data < 0 || metadata < 0)
		goto out;
	if (write(data, bytes, sizeof(bytes)) != sizeof(bytes) || fchmod(data, 0444) ||
	    ftruncate(metadata, sizeof(bytes)) || fchmod(metadata, 0644) ||
	    fsetxattr(metadata, "user.overlay.metacopy", "", 0, XATTR_CREATE) ||
	    fsetxattr(metadata, "user.overlay.redirect", "/.probe", strlen("/.probe"),
		      XATTR_CREATE))
		goto out;
	if (view_overlay("merged", true))
		goto out;
	mounted = true;
	merged = open("merged/.probe", O_RDONLY | O_CLOEXEC);
	if (merged < 0 || fstat(merged, &st) || st.st_size != sizeof(bytes) ||
	    (st.st_mode & 07777) != 0644 ||
	    pread(merged, buffer, sizeof(buffer), 0) != sizeof(buffer) ||
	    memcmp(bytes, buffer, sizeof(bytes)))
		goto out;
	close(merged);
	merged = open("merged/.probe", O_RDWR | O_CLOEXEC);
	if (merged < 0 || pwrite(merged, "changed", 7, 0) != 7 ||
	    pread(data, buffer, sizeof(buffer), 0) != sizeof(buffer) ||
	    memcmp(bytes, buffer, sizeof(bytes)))
		goto out;
	rc = 0;
out:
	if (merged >= 0)
		close(merged);
	if (mounted && umount("merged"))
		rc = -1;
	if (metadata >= 0)
		close(metadata);
	if (data >= 0)
		close(data);
	unlink("upper/.probe");
	unlink("baseline/.probe");
	unlink("data/.probe");
	return rc;
}

static int linux_metacopy_check(const char *runtime)
{
	pid_t child, waited;
	int status;

	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		if (view_namespace(false) || chdir(runtime))
			_exit(1);
		_exit(view_metacopy_probe() ? 1 : 0);
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

static bool linux_namespace_usable(void)
{
	pid_t child, waited;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child)
		_exit(view_namespace(true) ? 1 : 0);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	return waited == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

/*
 * Probe the namespaces of linux_hide() in a child, which owns the
 * namespaces it makes.
 */
static bool linux_hide_usable(void)
{
	pid_t child, waited;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child)
		_exit(view_namespace(true) ? 1 : 0);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	return waited == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

static int view_cover(const char *source, const char *target)
{
	int rc;

	rc = mount(source, target, NULL, MS_BIND, NULL);
	if (rc)
		return -1;
	return mount(NULL, target, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV,
		     NULL);
}

static int view_drop_capabilities(void);

/* Cover one path: a directory by an empty mount, a file by /dev/null. */
static int view_hide_one(const char *path)
{
	struct stat st;
	unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC;
	int rc;

	rc = stat(path, &st);
	if (rc)
		return errno == ENOENT ? 0 : -1;
	if (S_ISDIR(st.st_mode))
		rc = mount("tmpfs", path, "tmpfs", flags, "mode=0000,size=4k");
	else
		rc = mount("/dev/null", path, NULL, MS_BIND, NULL);
	if (rc)
		return -1;
	return mount(NULL, path, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | flags, NULL);
}

/* The exit status that a wait status stands for, as a shell reports it. */
static int view_wait_status(pid_t waited, int status)
{
	return waited < 0	? 127 :
	       WIFEXITED(status) ? WEXITSTATUS(status) :
				   128 + WTERMSIG(status);
}

/*
 * The init of the PID namespace: it runs the program as its only child, reaps
 * what the program leaves, and ends with its status. When the namespace ends,
 * the kernel ends every process in it. Never returns.
 */
static void view_pid_init(pid_t program)
{
	int status;
	pid_t waited;

	for (;;) {
		waited = waitpid(-1, &status, 0);
		if (waited == program)
			_exit(view_wait_status(waited, status));
		if (waited < 0 && errno != EINTR)
			_exit(127);
	}
}

/* The magic number of overlayfs, as statfs() reports it. */
#define VIEW_OVERLAY_MAGIC 0x794c7630

/* Whether the target shows the mount already: a scope shares one namespace. */
static bool view_mount_exists(const struct fyai_sandbox_mount *m)
{
	struct stat source, target;
	struct statvfs vfs;
	struct statfs fs;
	struct statx sx;

	if (m->kind == FYAI_MOUNT_OVERLAY)
		return !statfs(m->target, &fs) && fs.f_type == VIEW_OVERLAY_MAGIC;
	if (!strcmp(m->source, m->target)) {
		/* A bind onto itself: the path is a mount of its own, read-only when asked. */
		if (statx(AT_FDCWD, m->target, 0, 0, &sx) ||
		    !(sx.stx_attributes & STATX_ATTR_MOUNT_ROOT))
			return false;
		return !m->readonly || (!statvfs(m->target, &vfs) && (vfs.f_flag & ST_RDONLY));
	}
	return !stat(m->source, &source) && !stat(m->target, &target) &&
	       source.st_dev == target.st_dev && source.st_ino == target.st_ino;
}

static int view_mount_one(const struct fyai_sandbox_mount *m)
{
	char options[3 * PATH_MAX + 128];
	unsigned long flags = MS_NOSUID | MS_NODEV;
	struct statvfs vfs;
	int len;

	if (view_mount_exists(m))
		return 0;
	if (m->kind == FYAI_MOUNT_OVERLAY) {
		len = snprintf(options, sizeof(options),
			       "lowerdir=%s,upperdir=%s,workdir=%s,userxattr,index=off,"
			       "redirect_dir=nofollow,metacopy=off", m->source, m->upper,
			       m->work);
		if (len < 0 || (size_t)len >= sizeof(options))
			return -1;
		return mount("overlay", m->target, "overlay", flags, options);
	}
	/* The mounts below a directory go with it. */
	if (mount(m->source, m->target, NULL, MS_BIND | MS_REC, NULL))
		return -1;
	if (!m->readonly)
		return 0;
	/* A remount in a user namespace must keep the flags that the mount has. */
	if (statvfs(m->target, &vfs))
		return -1;
	flags = MS_REMOUNT | MS_BIND | MS_RDONLY;
	if (vfs.f_flag & ST_NOSUID)
		flags |= MS_NOSUID;
	if (vfs.f_flag & ST_NODEV)
		flags |= MS_NODEV;
	if (vfs.f_flag & ST_NOEXEC)
		flags |= MS_NOEXEC;
	if (vfs.f_flag & ST_NOATIME)
		flags |= MS_NOATIME;
	if (vfs.f_flag & ST_NODIRATIME)
		flags |= MS_NODIRATIME;
	if (vfs.f_flag & ST_RELATIME)
		flags |= MS_RELATIME;
	return mount(NULL, m->target, NULL, flags, NULL);
}

static int linux_mounts(const struct fyai_sandbox_mount *mounts, size_t n, bool late)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (mounts[i].late == late && view_mount_one(&mounts[i]))
			return -1;
	return 0;
}

static void (*view_parent_exit)(void *arg);
static void *view_parent_exit_arg;
static pid_t view_parent_exit_pid;

static void linux_on_parent_exit(void (*fn)(void *arg), void *arg)
{
	view_parent_exit = fn;
	view_parent_exit_arg = arg;
	view_parent_exit_pid = getpid();
}

static int linux_outer(void)
{
	pid_t child, waited;
	int status;

	/* A child that forked after the registration is not the process that waits. */
	if (!view_parent_exit || view_parent_exit_pid != getpid())
		return 0;
	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		view_parent_exit = NULL;
		return 0;
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	view_parent_exit(view_parent_exit_arg);
	_exit(view_wait_status(waited, status));
}

static int linux_hide(const char *const *paths, size_t n,
		     const struct fyai_sandbox_mount *mounts, size_t mounts_n)
{
	pid_t child, program, waited;
	int status;
	size_t i;
	int rc;

	if (linux_outer())
		return -1;
	rc = view_namespace(true);
	if (rc)
		return -1;
	for (i = 0; i < n; i++)
		if (view_hide_one(paths[i]))
			return -1;
	if (linux_mounts(mounts, mounts_n, false))
		return -1;
	/*
	 * The processes of the tool are the children of this one, in a PID
	 * namespace where this process and its parent do not exist. This process
	 * waits for the init of the namespace and ends with its status, so the
	 * caller sees one child, as it did.
	 */
	child = fork();
	if (child < 0)
		return -1;
	if (child) {
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		_exit(view_wait_status(waited, status));
	}
	/* The namespace ends with this process, and so do the programs in it. */
	if (prctl(PR_SET_PDEATHSIG, SIGKILL))
		_exit(127);
	/*
	 * The host procfs lists the processes of the host. A tool that cannot mount
	 * its own, because a container masks /proc, keeps the host one: the
	 * confinement of the process still holds.
	 */
	(void)mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (linux_mounts(mounts, mounts_n, true))
		_exit(127);
	program = fork();
	if (program < 0)
		_exit(127);
	if (program)
		view_pid_init(program);
	return view_drop_capabilities();
}

/* Time that the init of a scope gives its processes to end after SIGTERM. */
static long view_holder_grace_ms = 2000;

static void linux_set_grace(long ms)
{
	view_holder_grace_ms = ms;
}

/*
 * Reap the zombies of the PID namespace, and say whether any process but this
 * one is left in it. A process that a tool started has a parent outside the
 * namespace, so wait() alone does not see it: the procfs of the namespace does.
 */
static bool view_holder_children(void)
{
	struct dirent *de;
	DIR *dir;
	bool alive = false;

	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	dir = opendir("/proc");
	if (!dir)
		return false;
	while ((de = readdir(dir))) {
		if (de->d_name[0] < '1' || de->d_name[0] > '9' ||
		    !strcmp(de->d_name, "1"))
			continue;
		alive = true;
		break;
	}
	closedir(dir);
	return alive;
}

/*
 * The init of a scope. It reaps what the processes of the namespace leave,
 * and waits for the pipe to end. Then it asks the processes to stop, gives them
 * a grace time, and ends them. Never returns.
 */
static void view_holder_init(int hold)
{
	struct signalfd_siginfo si;
	struct pollfd fds[2];
	sigset_t set;
	char buf[64];
	ssize_t got;
	int sfd, left;

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &set, NULL))
		_exit(127);
	sfd = signalfd(-1, &set, SFD_CLOEXEC);
	if (sfd < 0)
		_exit(127);
	fds[0].fd = hold;
	fds[0].events = POLLIN;
	fds[1].fd = sfd;
	fds[1].events = POLLIN;
	for (;;) {
		if (poll(fds, 2, -1) < 0 && errno != EINTR)
			_exit(127);
		if (fds[1].revents & POLLIN) {
			if (read(sfd, &si, sizeof(si)) < 0 && errno != EAGAIN)
				_exit(127);
			(void)view_holder_children();
		}
		if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		got = read(hold, buf, sizeof(buf));
		if (got < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (got > 0)
			continue;
		break;
	}
	/* The owner is gone: stop the processes, then end them. */
	(void)kill(-1, SIGTERM);
	for (left = view_holder_grace_ms; left > 0; left -= 20) {
		if (!view_holder_children())
			_exit(0);
		(void)poll(NULL, 0, 20);
	}
	(void)kill(-1, SIGKILL);
	_exit(0);
}

static int linux_holder(const char *const *paths, size_t n, int hold,
		       int owner, pid_t *waiter, pid_t *init)
{
	pid_t first, second, waited;
	int report[2], status, rc;
	size_t i;

	if (pipe2(report, O_CLOEXEC))
		return -1;
	first = fork();
	if (first < 0) {
		close(report[0]);
		close(report[1]);
		return -1;
	}
	if (!first) {
		close(report[0]);
		/* The holder must not keep the pipe that ends it: it ends when the owner closes it. */
		close(owner);
		/* The terminal sends no signal to the scope: it has a group of its own. */
		(void)setpgid(0, 0);
		if (prctl(PR_SET_PDEATHSIG, SIGKILL))
			_exit(127);
		if (view_namespace(true))
			_exit(127);
		for (i = 0; i < n; i++)
			if (view_hide_one(paths[i]))
				_exit(127);
		second = fork();
		if (second < 0)
			_exit(127);
		if (!second) {
			if (prctl(PR_SET_PDEATHSIG, SIGKILL))
				_exit(127);
			(void)mount("proc", "/proc", "proc",
				    MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
			view_holder_init(hold);
		}
		/* The pid that the owner joins, as the owner names it. */
		if (write(report[1], &second, sizeof(second)) != sizeof(second))
			_exit(127);
		close(report[1]);
		do {
			waited = waitpid(second, &status, 0);
		} while (waited < 0 && errno == EINTR);
		_exit(view_wait_status(waited, status));
	}
	close(report[1]);
	do {
		rc = (int)read(report[0], &second, sizeof(second));
	} while (rc < 0 && errno == EINTR);
	close(report[0]);
	if (rc != (int)sizeof(second) || second <= 0) {
		(void)waitpid(first, NULL, 0);
		errno = EPERM;
		return -1;
	}
	*waiter = first;
	*init = second;
	return 0;
}

static int linux_drop_capabilities(void)
{
	return view_drop_capabilities();
}

static int view_drop_capabilities(void)
{
	struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct data[2] = { 0 };
	int cap, rc;

	rc = prctl(PR_SET_SECUREBITS, SECBIT_NOROOT | SECBIT_NOROOT_LOCKED);
	if (rc)
		return -1;
	for (cap = 0; cap <= CAP_LAST_CAP; cap++) {
		rc = prctl(PR_CAPBSET_DROP, cap);
		if (rc)
			return -1;
	}
	return syscall(SYS_capset, &header, data);
}

/* Pseudo filesystems that the view mounts over the host copies. */
static const struct view_pseudo {
	const char *type;
	const char *target;
	unsigned long flags;
} view_pseudo_mounts[] = {
	{ "proc", "/proc", MS_NOSUID | MS_NODEV | MS_NOEXEC },
};

static int view_mount_pseudo(void)
{
	size_t i;
	int rc;

	for (i = 0; i < sizeof(view_pseudo_mounts) / sizeof(view_pseudo_mounts[0]); i++) {
		rc = mount(view_pseudo_mounts[i].type, view_pseudo_mounts[i].target,
			   view_pseudo_mounts[i].type, view_pseudo_mounts[i].flags, NULL);
		if (rc)
			return -1;
	}
	return 0;
}

/*
 * Report the process that runs tool code to the supervisor: send a pidfd of
 * this process. A pidfd names a process in any namespace, so the supervisor and
 * the transport, which are in an ancestor namespace, take its PID there. The
 * process then waits for the supervisor to release it, and takes the variable
 * that the release carries into its environment: the supervisor registers the
 * process first, so it sends nothing before it is known.
 */
static int view_announce(int fd)
{
	char byte = 0, release[512], *name, *value, *end;
	ssize_t got;
	union {
		struct cmsghdr align;
		char raw[CMSG_SPACE(sizeof(int))];
	} ctl = { 0 };
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = ctl.raw, .msg_controllen = sizeof(ctl.raw),
	};
	struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
	int pidfd, saved;
	ssize_t n;

	pidfd = syscall(SYS_pidfd_open, getpid(), 0);
	if (pidfd < 0)
		return -1;
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &pidfd, sizeof(int));
	do {
		n = sendmsg(fd, &mh, MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	saved = errno;
	close(pidfd);
	errno = saved;
	if (n != 1)
		return -1;
	do {
		got = recv(fd, release, sizeof(release) - 1, 0);
	} while (got < 0 && errno == EINTR);
	if (got < 1 || release[0] != 1) {
		errno = got < 0 ? errno : ECONNABORTED;
		return -1;
	}
	release[got] = '\0';
	end = release + got;
	for (name = release + 1; name < end; name = value + strlen(value) + 1) {
		value = name + strlen(name) + 1;
		if (value >= end || setenv(name, value, 1)) {
			errno = EPROTO;
			return -1;
		}
	}
	return 0;
}

static int linux_init_release(int fd, const char *name, const char *value)
{
	char release[512];
	size_t n = 1;
	ssize_t sent;

	release[0] = 1;
	if (name && value) {
		if (strlen(name) + strlen(value) + 4 > sizeof(release)) {
			errno = E2BIG;
			return -1;
		}
		n += (size_t)snprintf(release + n, sizeof(release) - n, "%s", name) + 1;
		n += (size_t)snprintf(release + n, sizeof(release) - n, "%s", value) + 1;
	}
	do {
		sent = send(fd, release, n, MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	return sent == (ssize_t)n ? 0 : -1;
}

static int linux_init_pidfd(int fd, int *pidfd)
{
	char byte;
	union {
		struct cmsghdr align;
		char raw[CMSG_SPACE(sizeof(int))];
	} ctl = { 0 };
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = ctl.raw, .msg_controllen = sizeof(ctl.raw),
	};
	struct cmsghdr *c;
	ssize_t n;

	do {
		n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -1;
	c = CMSG_FIRSTHDR(&mh);
	if (n != 1 || (mh.msg_flags & MSG_CTRUNC) || !c || c->cmsg_level != SOL_SOCKET ||
	    c->cmsg_type != SCM_RIGHTS || c->cmsg_len != CMSG_LEN(sizeof(int))) {
		errno = n ? EPROTO : ECONNRESET;
		return -1;
	}
	memcpy(pidfd, CMSG_DATA(c), sizeof(int));
	return 0;
}

static int linux_arena_adopt(struct fyai_cfg *cfg, int fd)
{
	const char *previous = cfg->view_arena, *path;
	int rc;

	rc = fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (rc)
		return -1;
	/* A tool child in the same Landlock domain could open /proc/PID/fd of this process. */
	rc = prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	if (rc)
		return -1;
	path = fy_gb_intern_string(cfg->gb, fy_sprintfa("/proc/self/fd/%d", fd));
	if (!path) {
		errno = ENOMEM;
		return -1;
	}
	cfg->view_arena_fd = fd;
	cfg->view_arena = path;
	if (previous && cfg->arena_dir && !strcmp(cfg->arena_dir, previous))
		cfg->arena_dir = path;
	return 0;
}

static void linux_arena_drop(struct fyai_cfg *cfg)
{
	if (cfg->view_arena_fd >= 0)
		close(cfg->view_arena_fd);
	cfg->view_arena_fd = -1;
	cfg->view_arena = NULL;
}

static int linux_arena_pass(const struct fyai_cfg *cfg)
{
	if (cfg->view_arena_fd < 0)
		return 0;
	return fcntl(cfg->view_arena_fd, F_SETFD, 0);
}

static int linux_enter(struct fyai_cfg *cfg, const struct fyai_fsview *view,
		      int status_fd, int announce_fd)
{
	const char *backing = fy_sprintfa("%s/" FYAI_FSVIEW_BACKING_NAME, view->scratch);
	struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
	struct mount_attr attrs = { .attr_set = MOUNT_ATTR_RDONLY };
	struct fyai_sandbox_path allowed[2] = {
		{ .path = view->scratch, .mode = FYAI_SB_RW },
		{ .path = NULL, .mode = FYAI_SB_RW },
	};
	struct fyai_sandbox_spec sandbox = {
		.strict = true, .read_all = true, .allow = allowed, .allow_n = 1
	};
	const char *deny[4], *protected;
	struct stat project_stat;
	struct timespec project_times[2];
	int runtime, rc, saved, status, tree = -1, arena_tree = -1;
	size_t denied = 0;
	pid_t child, waited, parent = getppid();

	rc = fyai_fsview_verify(view, NULL, 0);
	if (rc)
		return -1;
	runtime = open(view->runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (runtime < 0)
		return -1;
	rc = view_namespace(true);
	if (rc)
		goto out;
	rc = prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (rc)
		goto out;
	if (getppid() != parent) {
		errno = EPIPE;
		rc = -1;
		goto out;
	}
	child = fork();
	if (child < 0) {
		rc = -1;
		goto out;
	}
	if (child) {
		/* Only the namespace init executes tool code; its exit kills
		 * descendants. */
		if (status_fd >= 0)
			close(status_fd);
		if (announce_fd >= 0)
			close(announce_fd);
		close(runtime);
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		_exit(waited < 0	? 127 :
		      WIFEXITED(status) ? WEXITSTATUS(status) :
					  128 + WTERMSIG(status));
	}
	rc = prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (rc)
		goto out;
	if (view->terminal) {
		sigaction(SIGTTOU, &ignore, &previous);
		rc = setpgid(0, 0);
		if (!rc)
			rc = tcsetpgrp(STDIN_FILENO, getpgrp());
		sigaction(SIGTTOU, &previous, NULL);
		if (rc)
			goto out;
	}
	close(runtime);
	runtime = open(view->runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (runtime < 0) {
		rc = -1;
		goto out;
	}
	tree = syscall(SYS_open_tree, runtime, "",
		       OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	if (tree < 0) {
		rc = -1;
		goto out;
	}
	if (view->agent) {
		/* Taken before the outside mounts become read-only. */
		arena_tree = syscall(SYS_open_tree, AT_FDCWD, view->arena,
				     OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
		if (arena_tree < 0) {
			rc = -1;
			goto out;
		}
	}
	rc = mount("/", "/", NULL, MS_BIND | MS_REC, NULL);
	if (rc)
		goto out;
	rc = syscall(SYS_mount_setattr, AT_FDCWD, "/", AT_RECURSIVE, &attrs, sizeof(attrs));
	if (rc)
		goto out;
	rc = mount("tmpfs", view->scratch, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=1G");
	if (rc)
		goto out;
	rc = mkdir(backing, 0700);
	if (rc)
		goto out;
	rc = syscall(SYS_move_mount, tree, "", AT_FDCWD, backing, MOVE_MOUNT_F_EMPTY_PATH);
	if (rc)
		goto out;
	rc = mount(NULL, backing, NULL, MS_REMOUNT | MS_BIND | MS_NOSUID | MS_NODEV, NULL);
	if (rc)
		goto out;
	rc = chdir(backing);
	if (rc)
		goto out;
	if (fyai_path_beneath(view->project, view->scratch)) {
		rc = fyai_mkdir_p(view->project);
		if (rc)
			goto out;
	}
	rc = view_overlay(view->project, view->metacopy);
	if (rc)
		goto out;
	protected = fy_sprintfa("%s/.fyai", view->project);
	rc = stat(view->project, &project_stat);
	if (rc)
		goto out;
	/*
	 * A whiteout hides the storage and leaves the project root a plain
	 * directory, which the sandbox grants whole. Keep the timestamp of the
	 * root, so a snapshot does not record the whiteout as a change.
	 */
	rc = rmdir(protected);
	if (rc && errno != ENOENT)
		goto out;
	if (!rc) {
		project_times[0] = project_stat.st_atim;
		project_times[1] = project_stat.st_mtim;
		rc = utimensat(AT_FDCWD, view->project, project_times, 0);
		if (rc)
			goto out;
	}
	if (view->agent && !fyai_path_beneath(view->arena, protected) &&
	    !fyai_path_beneath(view->arena, view->scratch)) {
		rc = view_cover("cover", view->arena);
		if (rc)
			goto out;
	}
	rc = view_cover("cover", backing);
	if (rc)
		goto out;
	/*
	 * The arena of an agent runtime is mounted at no path of the view. The
	 * runtime reaches the detached mount through its descriptor, which no
	 * other program receives.
	 */
	if (view->agent) {
		rc = linux_arena_adopt(cfg, arena_tree);
		if (rc)
			goto out;
		arena_tree = -1;
		allowed[1].path = cfg->view_arena;
		sandbox.allow_n = 2;
	}
	cfg->view_project = fy_gb_intern_string(cfg->gb, view->project);
	cfg->view_scratch = fy_gb_intern_string(cfg->gb, view->scratch);
	if (!cfg->view_project || !cfg->view_scratch) {
		rc = -1;
		goto out;
	}
	rc = view_mount_pseudo();
	if (rc)
		goto out;
	if (!fyai_path_beneath(view->storage, protected) && !fyai_path_beneath(view->storage, view->scratch))
		deny[denied++] = view->storage;
	if (!view->agent && !fyai_path_beneath(view->arena, protected) &&
	    !fyai_path_beneath(view->arena, view->scratch))
		deny[denied++] = view->arena;
	sandbox.project_root = view->project;
	sandbox.deny = deny;
	sandbox.deny_n = sandbox.deny_global_n = denied;
	rc = chdir(view->project);
	if (rc)
		goto out;
	close(runtime);
	runtime = -1;
	close(tree);
	tree = -1;
	rc = fyai_sandbox_apply(&sandbox);
	if (!rc)
		rc = view_drop_capabilities();
	if (!rc) {
		close(tree);
		tree = -1;
		child = fork();
		if (child < 0) {
			rc = -1;
			goto out;
		}
		if (child) {
			linux_arena_drop(cfg);
			if (status_fd >= 0)
				close(status_fd);
			if (announce_fd >= 0)
				close(announce_fd);
			do {
				waited = waitpid(child, &status, 0);
			} while (waited < 0 && errno == EINTR);
			_exit(waited < 0	? 127 :
			      WIFEXITED(status) ? WEXITSTATUS(status) :
						  128 + WTERMSIG(status));
		}
		if (view->terminal) {
			sigaction(SIGTTOU, &ignore, &previous);
			rc = setpgid(0, 0);
			if (!rc)
				rc = tcsetpgrp(STDIN_FILENO, getpgrp());
			sigaction(SIGTTOU, &previous, NULL);
		}
		if (!rc && announce_fd >= 0) {
			rc = view_announce(announce_fd);
			saved = errno;
			close(announce_fd);
			errno = saved;
		}
	}
out:
	saved = errno;
	if (tree >= 0)
		close(tree);
	if (arena_tree >= 0)
		close(arena_tree);
	if (runtime >= 0)
		close(runtime);
	errno = saved;
	return rc;
}

static int view_mount_identity(const char *target, struct fyai_fsview_mount *identity)
{
	struct stat namespace;
	struct statx root, cover;
	const char *protected;
	int rc;

	rc = stat("/proc/self/ns/mnt", &namespace);
	if (rc)
		return -1;
	rc = statx(AT_FDCWD, target, AT_SYMLINK_NOFOLLOW, STATX_INO | STATX_MNT_ID, &root);
	if (rc)
		return -1;
	protected = fy_sprintfa("%s/.fyai", target);
	rc = statx(AT_FDCWD, protected, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &cover);
	if (rc && errno != ENOENT)
		return -1;
	identity->namespace_device = namespace.st_dev;
	identity->namespace_inode = namespace.st_ino;
	identity->mount_id = root.stx_mnt_id;
	identity->root_inode = root.stx_ino;
	identity->cover_id = rc || cover.stx_mnt_id == root.stx_mnt_id ? 0 : cover.stx_mnt_id;
	return 0;
}

static int view_mount_source(const struct fyai_fsview *view, uint64_t mount_id)
{
	const char *base = strrchr(view->runtime, '/');
	char *line = NULL, *separator, *source;
	const char *expected;
	FILE *file;
	size_t capacity = 0, length;
	unsigned long long id;
	int rc = -1, saved;

	rc = fyai_fsview_verify(view, NULL, 0);
	if (rc)
		return -1;
	base = base ? base + 1 : view->runtime;
	expected = fy_sprintfa("fyai-%s", base);
	file = fopen("/proc/self/mountinfo", "re");
	if (!file)
		return -1;
	rc = -1;
	errno = ESTALE;
	while (getline(&line, &capacity, file) >= 0) {
		id = strtoull(line, NULL, 10);
		if (id != mount_id)
			continue;
		separator = strstr(line, " - overlay ");
		if (!separator)
			break;
		source = separator + strlen(" - overlay ");
		length = strcspn(source, " ");
		if (length == strlen(expected) && !memcmp(source, expected, length))
			rc = 0;
		break;
	}
	saved = errno;
	free(line);
	fclose(file);
	errno = saved;
	return rc;
}

static int linux_mount(const struct fyai_fsview *view, const char *target,
		      struct fyai_fsview_mount *identity)
{
	const char *base = strrchr(view->runtime, '/');
	const char *source, *protected;
	int cwd = -1, runtime = -1, rc = -1, saved;
	bool mounted = false, covered = false;

	rc = fyai_fsview_verify(view, NULL, 0);
	if (rc)
		return -1;
	base = base ? base + 1 : view->runtime;
	source = fy_sprintfa("fyai-%s", base);
	protected = fy_sprintfa("%s/.fyai", target);
	cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	runtime = open(view->runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (cwd < 0 || runtime < 0) {
		rc = -1;
		goto out;
	}
	rc = fchdir(runtime);
	if (rc)
		goto out;
	rc = mount(source, target, "overlay", MS_RDONLY | MS_NOSUID | MS_NODEV,
		   view_overlay_options(view->metacopy));
	if (rc)
		goto out;
	mounted = true;
	rc = view_cover("cover", protected);
	if (rc && errno != ENOENT)
		goto out;
	covered = !rc;
	rc = view_mount_identity(target, identity);
out:
	saved = errno;
	if (rc && covered)
		umount2(protected, 0);
	if (rc && mounted)
		umount2(target, 0);
	if (cwd >= 0) {
		if (fchdir(cwd) && !rc) {
			rc = -1;
			saved = errno;
		}
		close(cwd);
	}
	if (runtime >= 0)
		close(runtime);
	errno = saved;
	return rc;
}

static int linux_unmount(const struct fyai_fsview *view, const char *target,
			struct fyai_fsview_mount *identity)
{
	struct fyai_fsview_mount current;
	const char *protected;
	int rc;

	rc = view_mount_identity(target, &current);
	if (rc)
		return -1;
	if (current.namespace_device != identity->namespace_device ||
	    current.namespace_inode != identity->namespace_inode ||
	    current.mount_id != identity->mount_id || current.cover_id != identity->cover_id ||
	    current.root_inode != identity->root_inode) {
		errno = ESTALE;
		return -1;
	}
	rc = view_mount_source(view, current.mount_id);
	if (rc)
		return -1;
	protected = fy_sprintfa("%s/.fyai", target);
	if (identity->cover_id) {
		rc = umount2(protected, 0);
		if (rc)
			return -1;
		identity->cover_id = 0;
	}
	return umount2(target, 0);
}

static int view_transfer(int fd, void *data, size_t length, bool writing)
{
	unsigned char *p = data;
	ssize_t amount;

	while (length) {
		amount = writing ? write(fd, p, length) : read(fd, p, length);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0) {
			if (!amount)
				errno = EIO;
			return -1;
		}
		p += amount;
		length -= (size_t)amount;
	}
	return 0;
}

/*
 * The exit child writes the manifest of the result into the project storage and
 * tells its parent on a pipe: this header, then the payload. A frame of the ok
 * status holds the root, the number of objects and the name of the manifest
 * file; one of the error status holds the errno and the path that the capture
 * reported.
 */
struct view_frame {
	uint32_t magic;
	uint16_t version;
	uint16_t status;
	uint64_t length;
};

#define VIEW_FRAME_MAGIC UINT32_C(0x57565946)
#define VIEW_FRAME_VERSION 2
#define VIEW_FRAME_OK 0
#define VIEW_FRAME_ERROR 1
#define VIEW_FRAME_MAX_LENGTH 8192

static void view_ok_put(struct fyai_wire_writer *writer, const struct fyai_manifest *manifest,
			const char *name)
{
	fyai_wire_put_bytes(writer, manifest->root, FYAI_CAS_HASH_SIZE);
	fyai_wire_put_varint(writer, manifest->object_total);
	fyai_wire_put_varint(writer, strlen(name));
	fyai_wire_put_bytes(writer, name, strlen(name));
}

static void view_error_put(struct fyai_wire_writer *writer, int error, const char *path)
{
	fyai_wire_put_varint(writer, (uint64_t)error);
	fyai_wire_put_varint(writer, strlen(path));
	fyai_wire_put_bytes(writer, path, strlen(path));
}

/* Send a frame: one pass counts the payload and one pass writes it. */
static int view_frame_send(int fd, bool ok, const struct fyai_manifest *manifest, const char *name,
			   int error, const char *path)
{
	struct fyai_wire_writer counter, writer;
	struct view_frame frame = { .magic = VIEW_FRAME_MAGIC,
				    .version = VIEW_FRAME_VERSION,
				    .status = ok ? VIEW_FRAME_OK : VIEW_FRAME_ERROR };

	fyai_wire_writer_init(&counter, -1);
	if (ok)
		view_ok_put(&counter, manifest, name);
	else
		view_error_put(&counter, error, path);
	frame.length = counter.total;
	if (view_transfer(fd, &frame, sizeof(frame), true))
		return -1;
	fyai_wire_writer_init(&writer, fd);
	if (ok)
		view_ok_put(&writer, manifest, name);
	else
		view_error_put(&writer, error, path);
	return fyai_wire_flush(&writer);
}

/*
 * Read a frame. A frame of the error status sets errno and the path of error and
 * returns -1; so does a damaged frame.
 */
static fy_generic view_frame_receive(int fd, struct fy_generic_builder *gb,
				     struct fyai_scratch *scratch, char *error, size_t error_size)
{
	struct fyai_wire_reader reader;
	struct view_frame frame;
	unsigned char *payload;
	uint64_t code, objects, length;
	unsigned char root[FYAI_CAS_HASH_SIZE];
	char *name;

	if (view_transfer(fd, &frame, sizeof(frame), false))
		return fy_invalid;
	if (frame.magic != VIEW_FRAME_MAGIC || frame.version != VIEW_FRAME_VERSION ||
	    (frame.status != VIEW_FRAME_OK && frame.status != VIEW_FRAME_ERROR)) {
		errno = EBADMSG;
		return fy_invalid;
	}
	if (frame.length > VIEW_FRAME_MAX_LENGTH) {
		errno = EFBIG;
		return fy_invalid;
	}
	payload = fyai_scratch_alloc(scratch, frame.length + 1);
	if (!payload || view_transfer(fd, payload, frame.length, false))
		return fy_invalid;
	fyai_wire_reader_init(&reader, payload, frame.length);
	if (frame.status == VIEW_FRAME_ERROR) {
		if (fyai_wire_get_varint(&reader, &code) || fyai_wire_get_varint(&reader, &length) ||
		    length > (uint64_t)(reader.end - reader.position)) {
			errno = EBADMSG;
			return fy_invalid;
		}
		if (error && error_size)
			snprintf(error, error_size, "%.*s", (int)length,
				 (const char *)reader.position);
		errno = code ? (int)code : EIO;
		return fy_invalid;
	}
	if ((size_t)(reader.end - reader.position) < sizeof(root))
		goto damaged;
	memcpy(root, reader.position, sizeof(root));
	reader.position += sizeof(root);
	if (fyai_wire_get_varint(&reader, &objects) || fyai_wire_get_varint(&reader, &length) ||
	    length != FYAI_CAS_DIGEST_SIZE - 1 || length != (uint64_t)(reader.end - reader.position))
		goto damaged;
	name = (char *)payload + (reader.position - payload);
	name[length] = '\0';
	if (!fyai_fsview_manifest_name_valid(name))
		goto damaged;
	return fyai_fsview_reference(gb, root, objects, name);
damaged:
	errno = EBADMSG;
	return fy_invalid;
}

static fy_generic linux_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
				char *error, size_t error_size)
{
	struct fyai_manifest baseline = { 0 }, result = { 0 }, verified = { 0 };
	struct fyai_project_capture_opts opts = { .baseline_fd = -1,
						  .incremental = true,
						  .upper_fd = -1,
						  .mapped_owner = true,
						  .host_uid = getuid(),
						  .host_gid = getgid(),
						  .verify = view->verify,
						  .defer_sync = view->lazy };
	struct fyai_scratch scratch = { 0 };
	fy_generic snapshot = fy_invalid;
	char path[PATH_MAX] = "", name[FYAI_MANIFEST_NAME_SIZE];
	const char *objects;
	int pipefd[2], rc, status, saved;
	pid_t child, waited;

	if (fyai_fsview_manifest_open(view->storage, view->baseline, &baseline))
		return fy_invalid;
	opts.baseline_manifest = &baseline;
	objects = fy_sprintfa("%s/objects/blake3", view->storage);
	rc = pipe2(pipefd, O_CLOEXEC);
	if (rc) {
		saved = errno;
		fyai_manifest_close(&baseline);
		errno = saved;
		return fy_invalid;
	}
	child = fork();
	if (child < 0) {
		saved = errno;
		close(pipefd[0]);
		close(pipefd[1]);
		fyai_manifest_close(&baseline);
		errno = saved;
		return fy_invalid;
	}
	if (!child) {
		close(pipefd[0]);
		rc = view_namespace(false);
		if (rc)
			goto child_error;
		rc = chdir(view->runtime);
		if (rc)
			goto child_error;
		rc = view_overlay("merged", view->metacopy);
		if (rc)
			goto child_error;
		opts.upper_fd = open("upper", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		opts.source_fd = open("merged", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		opts.objects_fd = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (opts.source_fd < 0 || opts.objects_fd < 0 || opts.upper_fd < 0)
			goto child_error;
		/*
		 * The result is stored as the difference from the baseline: an exit that
		 * changed little writes little. A baseline that is a delta has no full file
		 * to compare with, so that exit stores the whole tree.
		 */
		if (baseline.delta)
			rc = fyai_project_capture_manifest(&opts, &result, path, sizeof(path));
		else
			rc = fyai_project_capture_delta(&opts, fy_get(view->baseline, "manifest", ""),
							&result, path, sizeof(path));
		if (rc)
			goto child_error;
		if (view->verify) {
			opts.incremental = false;
			rc = fyai_project_capture_manifest(&opts, &verified, path, sizeof(path));
			/* A delta in memory reads through its baseline, which it does not own. */
			if (!rc && result.delta)
				result.base = &baseline;
			if (rc || !fyai_manifest_equal(&verified, &result)) {
				if (!rc)
					errno = EIO;
				goto child_error;
			}
		}
		rc = fyai_fsview_manifest_publish(view->storage, &result, !view->lazy, name);
		if (rc)
			goto child_error;
		rc = view_frame_send(pipefd[1], true, &result, name, 0, "");
		_exit(rc ? 1 : 0);
	child_error:
		view_frame_send(pipefd[1], false, NULL, NULL, errno ? errno : EIO, path);
		_exit(1);
	}
	close(pipefd[1]);
	fyai_manifest_close(&baseline);
	rc = fyai_scratch_open(&scratch);
	if (!rc)
		snapshot = view_frame_receive(pipefd[0], gb, &scratch, error, error_size);
	saved = errno;
	close(pipefd[0]);
	fyai_scratch_close(&scratch);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
		snapshot = fy_invalid;
		if (!saved)
			saved = EIO;
	}
	errno = saved;
	return snapshot;
}

static int linux_sync_backing(const char *runtime, const char *storage)
{
	int fd, rc, saved;

	(void)storage;
	fd = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		rc = syncfs(fd);
	} while (rc && errno == EINTR);
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

const struct fyai_fsview_backend fyai_fsview_backend_linux = {
	.name = "linux",
	.arena_adopt = linux_arena_adopt,
	.arena_drop = linux_arena_drop,
	.arena_pass = linux_arena_pass,
	.metacopy_check = linux_metacopy_check,
	.namespace_usable = linux_namespace_usable,
	.hide_usable = linux_hide_usable,
	.hide = linux_hide,
	.mounts = linux_mounts,
	.holder = linux_holder,
	.on_parent_exit = linux_on_parent_exit,
	.outer = linux_outer,
	.set_grace = linux_set_grace,
	.drop_capabilities = linux_drop_capabilities,
	.mount = linux_mount,
	.unmount = linux_unmount,
	.recover = linux_recover,
	.enter = linux_enter,
	.init_pidfd = linux_init_pidfd,
	.init_release = linux_init_release,
	.snapshot = linux_snapshot,
	.sessions = true,
	.sync_backing = linux_sync_backing,
};

#endif /* FYAI_FSVIEW_BACKEND_LINUX */
