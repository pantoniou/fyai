/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_fsview.h"
#include "fyai_project_capture.h"
#include "fyai_sandbox.h"

#ifdef __linux__
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/mount.h>
#include <linux/securebits.h>

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
	char mapping[64];
	uid_t uid = getuid();
	gid_t gid = getgid();
	int rc;

	rc = unshare(CLONE_NEWUSER | CLONE_NEWNS | (pid_namespace ? CLONE_NEWPID : 0));
	if (rc)
		return -1;
	rc = view_write_file("/proc/self/setgroups", "deny\n");
	if (rc)
		return -1;
	snprintf(mapping, sizeof(mapping), "%u %u 1\n", (unsigned int)uid, (unsigned int)uid);
	rc = view_write_file("/proc/self/uid_map", mapping);
	if (rc)
		return -1;
	snprintf(mapping, sizeof(mapping), "%u %u 1\n", (unsigned int)gid, (unsigned int)gid);
	rc = view_write_file("/proc/self/gid_map", mapping);
	if (rc)
		return -1;
	return mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
}

static int view_overlay(const char *target)
{
	return mount("overlay", target, "overlay", MS_NOSUID | MS_NODEV,
		     "lowerdir=baseline,upperdir=upper,workdir=work,userxattr,index=off,redirect_dir=nofollow");
}

static int view_mkdirs(const char *path)
{
	char buffer[PATH_MAX], *p;
	int rc;

	if (strlen(path) >= sizeof(buffer)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(buffer, path);
	for (p = buffer + 1; ; p++) {
		if (*p && *p != '/')
			continue;
		if (!*p) {
			rc = mkdir(buffer, 0700);
			return rc && errno != EEXIST ? -1 : 0;
		}
		*p = '\0';
		rc = mkdir(buffer, 0700);
		*p = '/';
		if (rc && errno != EEXIST)
			return -1;
	}
}

static int view_cover(const char *source, const char *target)
{
	int rc;

	rc = mount(source, target, NULL, MS_BIND, NULL);
	if (rc)
		return -1;
	return mount(NULL, target, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL);
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

static bool view_beneath(const char *path, const char *parent)
{
	size_t length = strlen(parent);

	return !strncmp(path, parent, length) && (!path[length] || path[length] == '/');
}

int fyai_fsview_enter(const struct fyai_fsview *view, int status_fd)
{
	static const char backing[] = "/tmp/.fyai-view-runtime";
	struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
	struct mount_attr attrs = { .attr_set = MOUNT_ATTR_RDONLY };
	struct fyai_sandbox_path scratch = { .path = "/tmp", .mode = FYAI_SB_RW };
	struct fyai_sandbox_spec sandbox = { .strict = true, .read_all = true, .allow = &scratch, .allow_n = 1 };
	const char *deny[4];
	char protected[PATH_MAX];
	int runtime, rc, saved, status, tree = -1;
	size_t denied = 0;
	pid_t child, waited, parent = getppid();

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
		/* Only the namespace init executes tool code; its exit kills descendants. */
		if (status_fd >= 0)
			close(status_fd);
		close(runtime);
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		_exit(waited < 0 ? 127 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
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
	tree = syscall(SYS_open_tree, runtime, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	if (tree < 0) {
		rc = -1;
		goto out;
	}
	rc = mount("/", "/", NULL, MS_BIND | MS_REC, NULL);
	if (rc)
		goto out;
	rc = syscall(SYS_mount_setattr, AT_FDCWD, "/", AT_RECURSIVE, &attrs, sizeof(attrs));
	if (rc)
		goto out;
	rc = mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=1G");
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
	if (!strncmp(view->project, "/tmp/", 5)) {
		rc = view_mkdirs(view->project);
		if (rc)
			goto out;
	}
	rc = view_overlay(view->project);
	if (rc)
		goto out;
	rc = snprintf(protected, sizeof(protected), "%s/.fyai", view->project);
	if (rc < 0 || rc >= (int)sizeof(protected)) {
		errno = ENAMETOOLONG;
		rc = -1;
		goto out;
	}
	rc = view_cover("cover", protected);
	if (rc)
		goto out;
	rc = view_cover("cover", backing);
	if (rc)
		goto out;
	rc = mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (rc)
		goto out;
	if (!view_beneath(view->storage, protected) && !view_beneath(view->storage, "/tmp"))
		deny[denied++] = view->storage;
	if (!view_beneath(view->arena, protected) && !view_beneath(view->arena, "/tmp"))
		deny[denied++] = view->arena;
	sandbox.project_root = view->project;
	sandbox.deny = deny;
	sandbox.deny_n = sandbox.deny_global_n = denied;
	rc = chdir(view->project);
	if (rc)
		goto out;
	close(runtime);
	runtime = -1;
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
			if (status_fd >= 0)
				close(status_fd);
			do {
				waited = waitpid(child, &status, 0);
			} while (waited < 0 && errno == EINTR);
			_exit(waited < 0 ? 127 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
		}
		if (view->terminal) {
			sigaction(SIGTTOU, &ignore, &previous);
			rc = setpgid(0, 0);
			if (!rc)
				rc = tcsetpgrp(STDIN_FILENO, getpgrp());
			sigaction(SIGTTOU, &previous, NULL);
		}
	}
out:
	saved = errno;
	if (tree >= 0)
		close(tree);
	if (runtime >= 0)
		close(runtime);
	errno = saved;
	return rc;
}

static int view_mount_identity(const char *target, struct fyai_fsview_mount *identity)
{
	struct stat namespace;
	struct statx root, cover;
	char protected[PATH_MAX];
	int rc;

	rc = stat("/proc/self/ns/mnt", &namespace);
	if (rc)
		return -1;
	rc = statx(AT_FDCWD, target, AT_SYMLINK_NOFOLLOW, STATX_INO | STATX_MNT_ID, &root);
	if (rc)
		return -1;
	rc = snprintf(protected, sizeof(protected), "%s/.fyai", target);
	if (rc < 0 || rc >= (int)sizeof(protected)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	rc = statx(AT_FDCWD, protected, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &cover);
	if (rc)
		return -1;
	identity->namespace_device = namespace.st_dev;
	identity->namespace_inode = namespace.st_ino;
	identity->mount_id = root.stx_mnt_id;
	identity->root_inode = root.stx_ino;
	identity->cover_id = cover.stx_mnt_id == root.stx_mnt_id ? 0 : cover.stx_mnt_id;
	return 0;
}

static int view_mount_source(const struct fyai_fsview *view, uint64_t mount_id)
{
	const char *base = strrchr(view->runtime, '/');
	char *line = NULL, *separator, *source;
	char expected[128];
	FILE *file;
	size_t capacity = 0, length;
	unsigned long long id;
	int rc = -1, saved;

	base = base ? base + 1 : view->runtime;
	rc = snprintf(expected, sizeof(expected), "fyai-%s", base);
	if (rc < 0 || rc >= (int)sizeof(expected)) {
		errno = ENAMETOOLONG;
		return -1;
	}
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

int fyai_fsview_mount(const struct fyai_fsview *view, const char *target,
		     struct fyai_fsview_mount *identity)
{
	const char *base = strrchr(view->runtime, '/');
	char source[128], protected[PATH_MAX];
	int cwd = -1, runtime = -1, rc = -1, saved;
	bool mounted = false, covered = false;

	base = base ? base + 1 : view->runtime;
	rc = snprintf(source, sizeof(source), "fyai-%s", base);
	if (rc < 0 || rc >= (int)sizeof(source)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	rc = snprintf(protected, sizeof(protected), "%s/.fyai", target);
	if (rc < 0 || rc >= (int)sizeof(protected)) {
		errno = ENAMETOOLONG;
		return -1;
	}
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
		"lowerdir=baseline,upperdir=upper,workdir=work,userxattr,index=off,redirect_dir=nofollow");
	if (rc)
		goto out;
	mounted = true;
	rc = view_cover("cover", protected);
	if (rc)
		goto out;
	covered = true;
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

int fyai_fsview_unmount(const struct fyai_fsview *view, const char *target,
		       struct fyai_fsview_mount *identity)
{
	struct fyai_fsview_mount current;
	char protected[PATH_MAX];
	int rc;

	rc = view_mount_identity(target, &current);
	if (rc)
		return -1;
	if (current.namespace_device != identity->namespace_device ||
	    current.namespace_inode != identity->namespace_inode ||
	    current.mount_id != identity->mount_id ||
	    current.cover_id != identity->cover_id ||
	    current.root_inode != identity->root_inode) {
		errno = ESTALE;
		return -1;
	}
	rc = view_mount_source(view, current.mount_id);
	if (rc)
		return -1;
	rc = snprintf(protected, sizeof(protected), "%s/.fyai", target);
	if (rc < 0 || rc >= (int)sizeof(protected)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (identity->cover_id) {
		rc = umount2(protected, 0);
		if (rc)
			return -1;
		identity->cover_id = 0;
	}
	return umount2(target, 0);
}

struct view_capture_reply {
	int error;
	uint64_t length;
	char path[PATH_MAX];
};

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

fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb,
		const struct fyai_fsview *view, char *error, size_t error_size)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };
	struct fy_generic_builder *child_gb;
	struct fyai_project_capture_opts opts = { .baseline_fd = -1,
		.mapped_owner = true, .host_uid = getuid(), .host_gid = getgid(),
		.verify = view->verify };
	struct view_capture_reply reply = { 0 };
	fy_generic snapshot = fy_invalid, emitted;
	fy_generic_sized_string input;
	const char *text;
	char *buffer = NULL, objects[PATH_MAX];
	int pipefd[2], rc, status, saved;
	pid_t child, waited;

	rc = snprintf(objects, sizeof(objects), "%s/objects/blake3", view->storage);
	if (rc < 0 || rc >= (int)sizeof(objects)) {
		errno = ENAMETOOLONG;
		return fy_invalid;
	}
	rc = pipe2(pipefd, O_CLOEXEC);
	if (rc)
		return fy_invalid;
	child = fork();
	if (child < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return fy_invalid;
	}
	if (!child) {
		close(pipefd[0]);
		child_gb = fy_generic_builder_create(&cfg);
		if (!child_gb) {
			errno = ENOMEM;
			goto child_error;
		}
		rc = view_namespace(false);
		if (rc)
			goto child_error;
		rc = chdir(view->runtime);
		if (rc)
			goto child_error;
		rc = view_overlay("merged");
		if (rc)
			goto child_error;
		opts.source_fd = open("merged", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		opts.objects_fd = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (opts.source_fd < 0 || opts.objects_fd < 0)
			goto child_error;
		snapshot = fyai_project_capture(child_gb, &opts, reply.path, sizeof(reply.path));
		if (!fy_is_valid(snapshot))
			goto child_error;
		emitted = fy_emit(child_gb, snapshot, FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_FLOW | FYOPEF_WIDTH_INF, NULL);
		if (!fy_is_string(emitted)) {
			errno = ENOMEM;
			goto child_error;
		}
		text = fy_castp(&emitted, "");
		reply.length = strlen(text);
		rc = view_transfer(pipefd[1], &reply, sizeof(reply), true);
		if (!rc)
			rc = view_transfer(pipefd[1], (void *)text, reply.length, true);
		_exit(rc ? 1 : 0);
child_error:
		reply.error = errno ? errno : EIO;
		view_transfer(pipefd[1], &reply, sizeof(reply), true);
		_exit(1);
	}
	close(pipefd[1]);
	rc = view_transfer(pipefd[0], &reply, sizeof(reply), false);
	if (rc)
		goto out;
	if (reply.error) {
		errno = reply.error;
		if (error && error_size)
			snprintf(error, error_size, "%s", reply.path);
		goto out;
	}
	if (reply.length > 128U * 1024U * 1024U) {
		errno = EFBIG;
		goto out;
	}
	buffer = malloc((size_t)reply.length + 1);
	if (!buffer)
		goto out;
	rc = view_transfer(pipefd[0], buffer, reply.length, false);
	if (rc)
		goto out;
	buffer[reply.length] = '\0';
	input = (fy_generic_sized_string){ .data = buffer, .size = reply.length };
	snapshot = fy_parse(gb, input, FYOPPF_DISABLE_DIRECTORY |
		FYOPPF_INPUT_TYPE_STRING | FYOPPF_MODE_YAML_1_2, NULL);
	if (!fy_is_mapping(snapshot)) {
		errno = EIO;
		snapshot = fy_invalid;
	}
out:
	saved = errno;
	close(pipefd[0]);
	free(buffer);
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
#else
int fyai_fsview_mount(const struct fyai_fsview *view, const char *target,
		     struct fyai_fsview_mount *identity)
{
	(void)view;
	(void)target;
	(void)identity;
	errno = ENOTSUP;
	return -1;
}

int fyai_fsview_unmount(const struct fyai_fsview *view, const char *target,
		       struct fyai_fsview_mount *identity)
{
	(void)view;
	(void)target;
	(void)identity;
	errno = ENOTSUP;
	return -1;
}

int fyai_fsview_enter(const struct fyai_fsview *view, int status_fd)
{
	(void)view;
	(void)status_fd;
	errno = ENOTSUP;
	return -1;
}

fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb,
		const struct fyai_fsview *view, char *error, size_t error_size)
{
	(void)gb;
	(void)view;
	(void)error;
	(void)error_size;
	errno = ENOTSUP;
	return fy_invalid;
}
#endif
