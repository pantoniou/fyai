/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
	snprintf(mapping, sizeof(mapping), "0 %u 1\n", (unsigned int)uid);
	rc = view_write_file("/proc/self/uid_map", mapping);
	if (rc)
		return -1;
	snprintf(mapping, sizeof(mapping), "0 %u 1\n", (unsigned int)gid);
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

int fyai_fsview_enter(const struct fyai_fsview *view, int status_fd)
{
	static const char backing[] = "/tmp/.fyai-view-runtime";
	struct mount_attr attrs = { .attr_set = MOUNT_ATTR_RDONLY };
	struct fyai_sandbox_path scratch = { .path = "/tmp", .mode = FYAI_SB_RW };
	struct fyai_sandbox_spec sandbox = { .strict = true, .read_all = true, .allow = &scratch, .allow_n = 1 };
	const char *deny[4];
	char protected[PATH_MAX];
	int runtime, rc, saved, status, tree = -1;
	pid_t child, waited;

	runtime = open(view->runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (runtime < 0)
		return -1;
	rc = view_namespace(true);
	if (rc)
		goto out;
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
	deny[0] = backing;
	deny[1] = view->storage;
	deny[2] = view->arena;
	deny[3] = protected;
	sandbox.project_root = view->project;
	sandbox.deny = deny;
	sandbox.deny_n = sandbox.deny_global_n = 4;
	rc = chdir(view->project);
	if (rc)
		goto out;
	close(runtime);
	runtime = -1;
	rc = fyai_sandbox_apply(&sandbox);
	if (!rc)
		rc = view_drop_capabilities();
out:
	saved = errno;
	if (tree >= 0)
		close(tree);
	if (runtime >= 0)
		close(runtime);
	errno = saved;
	return rc;
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
		.mapped_owner = true, .host_uid = getuid(), .host_gid = getgid() };
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
