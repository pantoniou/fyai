/*
 * fyai_cover_linux.c - hide paths from a tool in a mount namespace
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The holder is a bubblewrap process that makes user, mount and PID namespaces,
 * covers the paths in the mount namespace, and runs `cat` on a pipe until the
 * process that started it ends. A tool joins the namespaces with setns(): a
 * process of the user that made a user namespace holds every capability in it,
 * so nothing here needs privilege. The holder is not a daemon: it belongs to one
 * invocation and ends with it, and so does every tool in its PID namespace.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "fyai_host_backend.h"
#include "fyai_cover.h"
#include "fyai_fsview.h"

#ifdef FYAI_HOST_BACKEND_LINUX
#include <sched.h>

#define COVER_ENV_HOLDER	"FYAI_COVER_HOLDER"
#define COVER_ENV_PATHS		"FYAI_COVER_PATHS"
#define COVER_INFO_MAX		1024
/* A shell redirects only a single digit: the program says that it is ready on this one. */
#define COVER_READY_FD		8

void fyai_cover_init(struct fyai_cover *cover)
{
	const char *pid = getenv(COVER_ENV_HOLDER), *paths = getenv(COVER_ENV_PATHS);

	memset(cover, 0, sizeof(*cover));
	cover->hold_fd = -1;
	cover->userns = -1;
	if (pid && paths && atoi(pid) > 0) {
		cover->holder = (pid_t)atoi(pid);
		cover->paths = strdup(paths);
	}
}

void fyai_cover_abandon(struct fyai_cover *cover)
{
	if (cover->hold_fd >= 0)
		close(cover->hold_fd);
	cover->hold_fd = -1;
	cover->owner = false;
}

/* Reap the holders that ended; the others wait for a later call. */
static void cover_reap(struct fyai_cover *cover)
{
	size_t i;

	for (i = 0; i < FYAI_COVER_RETIRED_MAX; i++) {
		if (cover->retired[i] > 0 &&
		    waitpid(cover->retired[i], NULL, WNOHANG) != 0)
			cover->retired[i] = 0;
	}
}

/* Keep the child of an ended holder to reap: it ends after its grace time. */
static void cover_retire(struct fyai_cover *cover, pid_t pid)
{
	size_t i;

	if (pid <= 0)
		return;
	for (i = 0; i < FYAI_COVER_RETIRED_MAX; i++) {
		if (cover->retired[i] <= 0) {
			cover->retired[i] = pid;
			return;
		}
	}
	/* The list is full: wait for the oldest, so that no holder stays a zombie. */
	(void)waitpid(cover->retired[0], NULL, 0);
	memmove(&cover->retired[0], &cover->retired[1],
		(FYAI_COVER_RETIRED_MAX - 1) * sizeof(cover->retired[0]));
	cover->retired[FYAI_COVER_RETIRED_MAX - 1] = pid;
}

void fyai_cover_drain(struct fyai_cover *cover)
{
	size_t i;
	pid_t pid;
	int rc;

	for (i = 0; i < FYAI_COVER_RETIRED_MAX; i++) {
		pid = cover->retired[i];
		if (pid <= 0)
			continue;
		do {
			rc = waitpid(pid, NULL, 0);
		} while (rc < 0 && errno == EINTR);
		cover->retired[i] = 0;
	}
}

void fyai_cover_release(struct fyai_cover *cover)
{
	/* The pipe ends, the holder reads the end, and the namespaces go with it. */
	if (cover->owner) {
		(void)unsetenv(COVER_ENV_HOLDER);
		(void)unsetenv(COVER_ENV_PATHS);
		cover_retire(cover, cover->waiter);
	}
	cover->waiter = 0;
	fyai_cover_abandon(cover);
	free(cover->paths);
	cover->paths = NULL;
	cover->holder = 0;
	cover_reap(cover);
}

static bool holder_alive(pid_t pid)
{
	char path[64];

	if (pid <= 0)
		return false;
	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", (int)pid);
	return !access(path, R_OK);
}

static bool list_has(const char *list, const char *path)
{
	size_t n = strlen(path);
	const char *p = list;

	while (list && *p) {
		if (!strncmp(p, path, n) && (p[n] == '\n' || !p[n]))
			return true;
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
	}
	return false;
}

bool fyai_cover_covers(const struct fyai_cover *cover, const char *path)
{
	return cover->holder > 0 && list_has(cover->paths, path);
}

size_t fyai_cover_members(const struct fyai_cover *cover, pid_t *pids, size_t max)
{
	char path[64], want[64], got[64];
	struct dirent *de;
	ssize_t n;
	size_t count = 0;
	DIR *dir;
	long pid;

	if (cover->holder <= 0 || !max)
		return 0;
	snprintf(path, sizeof(path), "/proc/%d/ns/pid", (int)cover->holder);
	n = readlink(path, want, sizeof(want) - 1);
	if (n <= 0)
		return 0;
	want[n] = '\0';
	dir = opendir("/proc");
	if (!dir)
		return 0;
	while (count < max && (de = readdir(dir))) {
		pid = strtol(de->d_name, NULL, 10);
		if (pid <= 0 || pid == cover->holder)
			continue;
		snprintf(path, sizeof(path), "/proc/%ld/ns/pid", pid);
		n = readlink(path, got, sizeof(got) - 1);
		if (n <= 0)
			continue;
		got[n] = '\0';
		if (!strcmp(got, want))
			pids[count++] = (pid_t)pid;
	}
	closedir(dir);
	return count;
}

/* Find the bubblewrap program in the usual places; a tool has no PATH to search. */
static const char *bwrap_path(void)
{
	static const char *const dirs[] = {
		"/usr/bin/bwrap", "/bin/bwrap", "/usr/local/bin/bwrap", NULL,
	};
	const char *const *p;

	for (p = dirs; *p; p++)
		if (!access(*p, X_OK))
			return *p;
	return NULL;
}

/* The exit status that a wait status stands for, as a shell reports it. */
static int cover_wait_status(pid_t waited, int status)
{
	return waited < 0	? 127 :
	       WIFEXITED(status) ? WEXITSTATUS(status) :
				   128 + WTERMSIG(status);
}

int fyai_cover_join(pid_t holder, const struct fyai_sandbox_mount *mount,
		    size_t mount_n)
{
	static const struct {
		const char *name;
		int type;
	} ns[] = {
		{ "user", CLONE_NEWUSER }, { "mnt", CLONE_NEWNS },
		{ "pid", CLONE_NEWPID },
	};
	char path[64], cwd[PATH_MAX];
	int fd[3], i, saved, status;
	pid_t child, waited;

	for (i = 0; i < 3; i++)
		fd[i] = -1;
	if (fyai_fsview_outer())
		return -1;
	if (!getcwd(cwd, sizeof(cwd)))
		cwd[0] = '\0';
	for (i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)holder, ns[i].name);
		fd[i] = open(path, O_RDONLY | O_CLOEXEC);
		if (fd[i] < 0)
			goto err;
	}
	/* The user namespace first: its capabilities let the process join the others. */
	for (i = 0; i < 3; i++)
		if (setns(fd[i], ns[i].type))
			goto err;
	/* Joining a mount namespace resets the root and the directory of the process. */
	if (cwd[0] && chdir(cwd))
		goto err;
	for (i = 0; i < 3; i++)
		close(fd[i]);
	child = fork();
	if (child < 0)
		return -1;
	if (child) {
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		_exit(cover_wait_status(waited, status));
	}
	/* The joined namespaces may hold these already: a mounted one is kept. */
	if (fyai_fsview_mounts(mount, mount_n, false) || fyai_fsview_mounts(mount, mount_n, true))
		return -1;
	return fyai_fsview_drop_capabilities();
err:
	saved = errno;
	for (i = 0; i < 3; i++)
		if (fd[i] >= 0)
			close(fd[i]);
	errno = saved;
	return -1;
}

/* Make a pipe whose descriptors are above the one that the holder uses. */
static int cover_pipe(int fds[2])
{
	int i, j, high;

	if (pipe2(fds, O_CLOEXEC))
		return -1;
	for (i = 0; i < 2; i++) {
		high = fcntl(fds[i], F_DUPFD_CLOEXEC, COVER_READY_FD + 2);
		if (high < 0) {
			for (j = 0; j < 2; j++)
				close(fds[j]);
			fds[0] = fds[1] = -1;
			return -1;
		}
		close(fds[i]);
		fds[i] = high;
	}
	return 0;
}

/* The pid that bubblewrap reports in its JSON information, or 0. */
static pid_t holder_child_pid(const char *info)
{
	const char *p = strstr(info, "\"child-pid\"");

	if (!p)
		return 0;
	p = strchr(p, ':');
	return p ? (pid_t)atoi(p + 1) : 0;
}

/*
 * The arguments of the holder: the whole tree again, a procfs of the new PID
 * namespace, and a cover for each path. A directory gets an empty read-only
 * tmpfs and a file gets /dev/null.
 */
static int holder_argv(const char *bwrap, const char *const *paths, size_t n,
		       int info_fd, int ready_fd, char *num, size_t num_size,
		       char *script, size_t script_size, char ***argvp)
{
	char **argv;
	struct stat st;
	size_t i, argc = 0;

	argv = calloc(24 + 5 * n, sizeof(*argv));
	if (!argv)
		return -1;
	snprintf(num, num_size, "%d", info_fd);
	/* The program runs when the mounts stand: it says so, and then waits on the pipe. */
	snprintf(script, script_size, "echo >&%d; exec cat", ready_fd);
	argv[argc++] = (char *)bwrap;
	argv[argc++] = "--unshare-user";
	argv[argc++] = "--unshare-pid";
	argv[argc++] = "--bind";
	argv[argc++] = "/";
	argv[argc++] = "/";
	argv[argc++] = "--dev-bind";
	argv[argc++] = "/dev";
	argv[argc++] = "/dev";
	argv[argc++] = "--proc";
	argv[argc++] = "/proc";
	for (i = 0; i < n; i++) {
		if (stat(paths[i], &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			argv[argc++] = "--tmpfs";
			argv[argc++] = (char *)paths[i];
			argv[argc++] = "--remount-ro";
			argv[argc++] = (char *)paths[i];
		} else {
			argv[argc++] = "--ro-bind";
			argv[argc++] = "/dev/null";
			argv[argc++] = (char *)paths[i];
		}
	}
	argv[argc++] = "--die-with-parent";
	argv[argc++] = "--info-fd";
	argv[argc++] = num;
	argv[argc++] = "--";
	argv[argc++] = "sh";
	argv[argc++] = "-c";
	argv[argc++] = script;
	argv[argc] = NULL;
	*argvp = argv;
	return 0;
}

/*
 * Start a holder that covers @paths. Returns 0 with the holder in `cover`, or -1.
 * The holder reads a pipe that this process keeps open, so it ends with it.
 */
static int holder_start(struct fyai_cover *cover, const char *const *paths, size_t n)
{
	int hold[2] = { -1, -1 }, info[2] = { -1, -1 }, ready[2] = { -1, -1 };
	char buf[COVER_INFO_MAX], num[32], arg_info[16], arg_script[64], ch, **argv = NULL;
	const char *bwrap = bwrap_path();
	size_t used = 0, i, len = 1;
	ssize_t got;
	pid_t child, pid;
	char *joined;

	if (!bwrap || cover_pipe(hold) || cover_pipe(info) || cover_pipe(ready))
		goto err;
	if (holder_argv(bwrap, paths, n, info[1], COVER_READY_FD, arg_info, sizeof(arg_info),
			arg_script, sizeof(arg_script), &argv))
		goto err;
	child = fork();
	if (child < 0)
		goto err;
	if (!child) {
		/*
		 * The holder reads the pipe, and reports on the information descriptor.
		 * Its own words go nowhere: a host that cannot make the namespace says
		 * so through the caller.
		 */
		int null = open("/dev/null", O_WRONLY);

		if (dup2(hold[0], STDIN_FILENO) < 0 || fcntl(info[1], F_SETFD, 0) < 0 ||
		    dup2(ready[1], COVER_READY_FD) < 0 || null < 0 ||
		    dup2(null, STDOUT_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0)
			_exit(126);
		execv(bwrap, argv);
		_exit(127);
	}
	close(hold[0]);
	hold[0] = -1;
	close(info[1]);
	info[1] = -1;
	close(ready[1]);
	ready[1] = -1;
	/* bubblewrap writes the information when the namespaces stand, and ends if they cannot. */
	while (used + 1 < sizeof(buf) && (got = read(info[0], buf + used, sizeof(buf) - 1 - used)) != 0) {
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			break;
		used += (size_t)got;
		buf[used] = '\0';
		if (strchr(buf, '}'))
			break;
	}
	buf[used] = '\0';
	close(info[0]);
	info[0] = -1;
	pid = holder_child_pid(buf);
	if (pid <= 0) {
		(void)waitpid(child, NULL, WNOHANG);
		goto err;
	}
	/* The mounts stand when the program says so; the pipe ends if bubblewrap fails. */
	do {
		got = read(ready[0], &ch, 1);
	} while (got < 0 && errno == EINTR);
	if (got != 1) {
		(void)waitpid(child, NULL, WNOHANG);
		goto err;
	}
	close(ready[0]);
	ready[0] = -1;
	for (i = 0; i < n; i++)
		len += strlen(paths[i]) + 1;
	joined = calloc(1, len);
	if (!joined)
		goto err;
	for (i = 0; i < n; i++) {
		strcat(joined, paths[i]);
		strcat(joined, "\n");
	}
	free(cover->paths);
	cover->holder = pid;
	cover->owner = true;
	cover->hold_fd = hold[1];
	cover->paths = joined;
	cover->waiter = child;
	snprintf(num, sizeof(num), "%d", (int)pid);
	(void)setenv(COVER_ENV_HOLDER, num, 1);
	(void)setenv(COVER_ENV_PATHS, joined, 1);
	free(argv);
	return 0;
err:
	if (hold[0] >= 0)
		close(hold[0]);
	if (hold[1] >= 0)
		close(hold[1]);
	if (info[0] >= 0)
		close(info[0]);
	if (info[1] >= 0)
		close(info[1]);
	if (ready[0] >= 0)
		close(ready[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	free(argv);
	return -1;
}

static void holder_stop(struct fyai_cover *cover)
{
	fyai_cover_release(cover);
}

/* Whether a process can join the holder: tried in a child, which owns the result. */
static bool holder_joinable(pid_t holder)
{
	pid_t child, waited;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child)
		_exit(fyai_cover_join(holder, NULL, 0) ? 1 : 0);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	return waited == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

static bool userns_ok(struct fyai_cover *cover)
{
	if (cover->userns < 0)
		cover->userns = fyai_fsview_hide_usable();
	return cover->userns;
}

/*
 * Start a holder that this program makes, where it may make namespaces. The init
 * of the holder reads a pipe that this process keeps open and ends with it.
 */
static int userns_holder_start(struct fyai_cover *cover, const char *const *paths, size_t n)
{
	int hold[2] = { -1, -1 };
	char num[32], *joined;
	size_t i, len = 1;
	pid_t waiter, init;

	if (cover_pipe(hold))
		return -1;
	if (fyai_fsview_holder(paths, n, hold[0], hold[1], &waiter, &init)) {
		close(hold[0]);
		close(hold[1]);
		return -1;
	}
	close(hold[0]);
	for (i = 0; i < n; i++)
		len += strlen(paths[i]) + 1;
	joined = calloc(1, len);
	if (!joined) {
		close(hold[1]);
		cover_retire(cover, waiter);
		return -1;
	}
	for (i = 0; i < n; i++) {
		strcat(joined, paths[i]);
		strcat(joined, "\n");
	}
	free(cover->paths);
	cover->holder = init;
	cover->owner = true;
	cover->hold_fd = hold[1];
	cover->paths = joined;
	cover->waiter = waiter;
	snprintf(num, sizeof(num), "%d", (int)init);
	(void)setenv(COVER_ENV_HOLDER, num, 1);
	(void)setenv(COVER_ENV_PATHS, joined, 1);
	return 0;
}

enum fyai_cover_method fyai_cover_select(struct fyai_cover *cover, const char *mode,
				  const char *const *paths, size_t n, pid_t *holder,
				  enum fyai_cover_start start)
{
	bool auto_mode = !mode || !strcmp(mode, "auto");

	*holder = 0;
	cover_reap(cover);
	if (mode && !strcmp(mode, "none"))
		return FYAI_COVER_NONE;
	/* The holder of this process or an ancestor serves each tool that it starts. */
	if (holder_alive(cover->holder)) {
		*holder = cover->holder;
		return FYAI_COVER_BWRAP;
	}
	if ((auto_mode || !strcmp(mode, "userns")) && userns_ok(cover)) {
		/* The scope of an exchange: one init for every call of it. */
		if (start == FYAI_COVER_START_SCOPE && !cover->userns_failed) {
			if (!userns_holder_start(cover, paths, n)) {
				if (holder_joinable(cover->holder)) {
					*holder = cover->holder;
					return FYAI_COVER_BWRAP;
				}
				holder_stop(cover);
			}
			cover->userns_failed = true;
		}
		return FYAI_COVER_USERNS;
	}
	if ((auto_mode || !strcmp(mode, "bwrap")) && !cover->bwrap_failed &&
	    start != FYAI_COVER_JOIN_ONLY) {
		if (!holder_start(cover, paths, n)) {
			if (holder_joinable(cover->holder)) {
				*holder = cover->holder;
				return FYAI_COVER_BWRAP;
			}
			holder_stop(cover);
		}
		cover->bwrap_failed = true;
	}
	return FYAI_COVER_NONE;
}

void fyai_cover_probe(struct fyai_cover *cover, bool *userns, bool *bwrap)
{
	const char *none[1] = { NULL };

	*userns = userns_ok(cover);
	*bwrap = false;
	if (*userns)
		return;
	if (holder_alive(cover->holder)) {
		*bwrap = true;
		return;
	}
	if (cover->bwrap_failed)
		return;
	if (!holder_start(cover, none, 0)) {
		*bwrap = holder_joinable(cover->holder);
		holder_stop(cover);
	}
	cover->bwrap_failed = !*bwrap;
}

#endif /* FYAI_HOST_BACKEND_LINUX */
