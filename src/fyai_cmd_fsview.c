/* SPDX-License-Identifier: MIT */
#include "fyai.h"
#include "fyai_cmd.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef __linux__
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "fyai_branch.h"
#include "fyai_fsview.h"
#include "fyai_event.h"
#include "fyai_project_capture.h"
#include "fyai_storage.h"


static fy_generic view_store(struct fyai_ctx *ctx)
{
	struct fyai_branch branch;

	fyai_branch_decode(ctx->branch_prev, &branch);
	return fy_is_mapping(branch.store) ? branch.store : fy_map_empty;
}

static fy_generic view_find(struct fyai_cmd_call *call, const char *name)
{
	fy_generic views;

	views = fy_get(view_store(call->ctx), "views", fy_map_empty);
	return fy_get(views, name, fy_invalid);
}

static int view_save(struct fyai_ctx *ctx, const char *name, fy_generic view)
{
	fy_generic store, views;

	store = view_store(ctx);
	views = fy_get(store, "views", fy_map_empty);
	views = fy_assoc(ctx->gb, views, fy_value(ctx->gb, name), view);
	ctx->branch_store = fy_assoc(ctx->gb, store, "views", views);
	if (!fy_is_valid(ctx->branch_store)) {
		errno = ENOMEM;
		return -1;
	}
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMMAND, NULL);
	return fyai_publish_root(ctx, fy_invalid, fy_invalid, fy_invalid);
}

static fy_generic view_summary(struct fy_generic_builder *gb, const char *name, fy_generic view)
{
	fy_generic baseline, result, summary;

	baseline = fy_get(view, "baseline", fy_invalid);
	result = fy_get(view, "result", baseline);

	summary = fy_mapping(gb, "name", fy_value(gb, name),
		"project", fy_get(view, "project", ""),
		"baseline", fy_get(baseline, "root", ""),
		"root", fy_get(result, "root", ""),
		"materialization", "copy", "state", fy_get(view, "state", "ready"));
	if (fy_is_valid(fy_get(view, "mount", fy_invalid)))
		summary = fy_assoc(gb, summary, "mount", fy_get(view, "mount", fy_invalid));
	return summary;
}

static int view_writable(struct fyai_cmd_call *call)
{
	if (call->ctx->cfg->root_spec || call->ctx->gb != call->ctx->durable_gb) {
		fyai_error(call->ctx, "view: a writable durable arena is required; --root and --transient are read-only for views");
		return -1;
	}
	return 0;
}

static int view_storage(const char *project, char **storage, char **objects, char **views)
{
	int rc;

	rc = asprintf(storage, "%s/.fyai", project);
	if (rc < 0)
		return -1;
	rc = mkdir_private(*storage);
	if (rc)
		return -1;
	rc = asprintf(objects, "%s/objects", *storage);
	if (rc < 0)
		return -1;
	rc = mkdir_private(*objects);
	if (rc)
		return -1;
	free(*objects);
	*objects = NULL;
	rc = asprintf(objects, "%s/objects/blake3", *storage);
	if (rc < 0)
		return -1;
	rc = mkdir_private(*objects);
	if (rc)
		return -1;
	rc = asprintf(views, "%s/views", *storage);
	if (rc < 0)
		return -1;
	return mkdir_private(*views);
}

static bool view_storage_beneath(const char *project, const char *storage)
{
	size_t length = strlen(project);

	return !strncmp(project, storage, length) && storage[length] == '/' &&
	       strncmp(storage + length + 1, ".fyai/", 6) &&
	       strcmp(storage + length + 1, ".fyai");
}

static int view_capture(struct fyai_cmd_call *call, fy_generic *result, bool replace)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *project = fyai_cmd_arg_str(call, "project");
	struct fyai_project_capture_opts opts = { .source_fd = -1, .objects_fd = -1, .baseline_fd = -1 };
	fy_generic snapshot, record, previous, stored_project, stored_runtime;
	char *resolved = NULL, *storage = NULL, *objects = NULL, *views = NULL, *runtime = NULL;
	char error[PATH_MAX] = "", lockpath[PATH_MAX];
	const char *const directories[] = { "baseline", "upper", "work", "cover", "merged" };
	size_t i;
	int rc = -1, root = -1, lock = -1, saved;
	bool complete = false;

	if (view_writable(call))
		return -1;
	previous = view_find(call, name);
	if (!replace && fy_is_valid(previous)) {
		fyai_error(call->ctx, "view '%s' already exists", name);
		return -1;
	}
	if (replace && fy_is_valid(fy_get(previous, "mount", fy_invalid))) {
		fyai_error(call->ctx, "view '%s' is mounted; unmount it before updating", name);
		return -1;
	}
	if (replace) {
		if (!fy_is_mapping(previous)) {
			fyai_error(call->ctx, "view '%s' does not exist", name);
			return -1;
		}
		stored_project = fy_get(previous, "project", fy_invalid);
		stored_runtime = fy_get(previous, "runtime", fy_invalid);
		if (!fy_is_string(stored_project) || !fy_is_string(stored_runtime)) {
			fyai_error(call->ctx, "view '%s': invalid stored paths", name);
			return -1;
		}
		project = fy_castp(&stored_project, "");
		rc = snprintf(lockpath, sizeof(lockpath), "%s/lock", fy_castp(&stored_runtime, ""));
		if (rc < 0 || rc >= (int)sizeof(lockpath)) {
			errno = ENAMETOOLONG;
			goto out;
		}
		lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (lock < 0)
			goto out;
		rc = flock(lock, LOCK_EX | LOCK_NB);
		if (rc)
			goto out;
	}
	resolved = realpath(project, NULL);
	if (!resolved)
		goto out;
	if (!strcmp(resolved, "/") || !strcmp(resolved, "/tmp") ||
	    !strncmp(resolved, "/tmp/.fyai-view-runtime", 23)) {
		errno = EINVAL;
		goto out;
	}
	rc = view_storage(resolved, &storage, &objects, &views);
	if (rc)
		goto out;
	if (view_storage_beneath(resolved, storage) ||
	    view_storage_beneath(resolved, call->ctx->cfg->arena_dir)) {
		errno = EINVAL;
		goto out;
	}
	rc = asprintf(&runtime, "%s/view-XXXXXX", views);
	if (rc < 0)
		goto out;
	if (!mkdtemp(runtime))
		goto out;
	root = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root < 0)
		goto out;
	for (i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		rc = mkdirat(root, directories[i], 0700);
		if (rc)
			goto out;
	}
	opts.verify = fyai_cmd_arg_bool(call, "verify");
	opts.source_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	opts.objects_fd = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	opts.baseline_fd = openat(root, "baseline", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (opts.source_fd < 0 || opts.objects_fd < 0 || opts.baseline_fd < 0)
		goto out;
	snapshot = fyai_project_capture(call->ctx->gb, &opts, error, sizeof(error));
	if (!fy_is_valid(snapshot))
		goto out;
	rc = fsync(opts.baseline_fd);
	if (!rc)
		rc = fsync(root);
	if (rc)
		goto out;
	record = fy_mapping(call->ctx->gb, "version", 1LL, "name", fy_value(call->ctx->gb, name),
		"project", fy_value(call->ctx->gb, resolved), "storage", fy_value(call->ctx->gb, storage),
		"runtime", fy_value(call->ctx->gb, runtime), "baseline", snapshot,
		"state", "ready", "materialization", "copy");
	rc = view_save(call->ctx, name, record);
	if (rc)
		goto out;
	*result = view_summary(call->gb, name, record);
	complete = true;
out:
	saved = errno;
	if (opts.source_fd >= 0)
		close(opts.source_fd);
	if (opts.objects_fd >= 0)
		close(opts.objects_fd);
	if (opts.baseline_fd >= 0)
		close(opts.baseline_fd);
	if (root >= 0)
		close(root);
	if (lock >= 0)
		close(lock);
	if (!complete)
		fyai_error(call->ctx, "view '%s': cannot capture '%s'%s%s: %s", name, project,
			   *error ? " at " : "", error, strerror(saved));
	free(resolved);
	free(storage);
	free(objects);
	free(views);
	free(runtime);
	return complete ? 0 : -1;
}

int fyai_cmd_view_create(struct fyai_cmd_call *call, fy_generic *result)
{
	return view_capture(call, result, false);
}

int fyai_cmd_view_update(struct fyai_cmd_call *call, fy_generic *result)
{
	return view_capture(call, result, true);
}

int fyai_cmd_view_show(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	fy_generic view;

	view = view_find(call, name);
	if (!fy_is_mapping(view)) {
		fyai_error(call->ctx, "view '%s' does not exist", name);
		return -1;
	}
	*result = view_summary(call->gb, name, view);
	return 0;
}

int fyai_cmd_view_list(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name;
	fy_generic view, views;

	views = fy_get(view_store(call->ctx), "views", fy_map_empty);
	*result = fy_sequence(call->gb);
	fy_foreach_key_value(name, view, views)
		*result = fy_append(call->gb, *result, view_summary(call->gb, name, view));
	return fy_is_valid(*result) ? 0 : -1;
}

struct view_wait {
	struct fyai_event_source *child_source;
	pid_t child;
	int status;
	int cancelled;
	bool done;
};

static enum fyai_event_action view_exited(const struct fyai_event *event)
{
	struct view_wait *wait = event->userdata;

	wait->child_source = NULL;
	wait->status = event->status;
	wait->done = true;
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action view_terminated(const struct fyai_event *event)
{
	struct view_wait *wait = event->userdata;

	wait->cancelled = event->signo;
	kill(-wait->child, SIGKILL);
	return FYAIEA_CONTINUE;
}

static int view_wait_child(struct fyai_ctx *ctx, pid_t child, int *status, int *cancelled)
{
	struct fyai_event_loop *loop = fyai_ctx_loop(ctx);
	struct fyai_event_source *termination = NULL;
	struct view_wait wait = { .child = child };
	int rc = -1, saved;
	pid_t waited;

	if (!loop)
		goto out;
	rc = fyai_event_add_child(loop, child, view_exited, &wait, &wait.child_source);
	if (rc)
		goto out;
	rc = fyai_event_add_signal(loop, SIGTERM, view_terminated, &wait, &termination);
	if (rc)
		goto out;
	while (!wait.done) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx);
			if (!wait.cancelled)
				wait.cancelled = SIGINT;
			kill(-child, SIGKILL);
		}
		rc = fyai_event_loop_step(loop, -1);
		if (rc < 0)
			goto out;
	}
	rc = 0;
out:
	saved = errno;
	if (termination)
		fyai_event_source_remove(termination);
	if (wait.child_source)
		fyai_event_source_remove(wait.child_source);
	if (!wait.done) {
		kill(-child, SIGKILL);
		do {
			waited = waitpid(child, &wait.status, 0);
		} while (waited < 0 && errno == EINTR);
	}
	*status = wait.status;
	*cancelled = wait.cancelled;
	errno = saved;
	return rc;
}

static int view_exec(struct fyai_ctx *ctx, struct fyai_fsview *view, char *const argv[],
		      struct shell_command_result *result)
{
	struct fyai_child_spec spec = { .in_fd = -1, .out_fd = -1,
		.err_fd = -1, .ctty_fd = -1, .status_fd = -1, .view = view };
	struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
	struct termios terminal;
	const char *shell = getenv("SHELL");
	pid_t child, foreground = -1;
	int status_pipe[2], gate[2], status, rc, saved, cancelled = 0, waited;
	char ready;
	bool tty = isatty(STDIN_FILENO), term_saved = false;

	if (tty) {
		foreground = tcgetpgrp(STDIN_FILENO);
		if (foreground < 0)
			return -1;
		term_saved = !tcgetattr(STDIN_FILENO, &terminal);
	}
	rc = pipe2(status_pipe, O_CLOEXEC);
	if (rc)
		return -1;
	rc = pipe2(gate, O_CLOEXEC);
	if (rc) {
		close(status_pipe[0]);
		close(status_pipe[1]);
		return -1;
	}
	view->terminal = tty;
	child = fork();
	if (child < 0) {
		close(status_pipe[0]);
		close(status_pipe[1]);
		close(gate[0]);
		close(gate[1]);
		return -1;
	}
	if (!child) {
		close(status_pipe[0]);
		close(gate[1]);
		do {
			rc = read(gate[0], &ready, 1);
		} while (rc < 0 && errno == EINTR);
		close(gate[0]);
		if (rc != 1)
			_exit(FYAI_SHELL_EXIT_EXEC);
		spec.status_fd = status_pipe[1];
		rc = fyai_child_exec_prepare(ctx, &spec);
		if (rc)
			_exit(rc);
		if (argv && argv[0]) {
			execvp(argv[0], argv);
			fyai_child_status_report(3, FYAI_CHILD_STAGE_EXEC, errno);
		} else {
			fyai_exec_shell_command(3, NULL, shell, false);
		}
		_exit(FYAI_SHELL_EXIT_EXEC);
	}
	close(status_pipe[1]);
	close(gate[0]);
	rc = setpgid(child, child);
	if (!rc && tty) {
		sigaction(SIGTTOU, &ignore, &previous);
		rc = tcsetpgrp(STDIN_FILENO, child);
		sigaction(SIGTTOU, &previous, NULL);
	}
	if (!rc) {
		ready = 1;
		rc = write(gate[1], &ready, 1) == 1 ? 0 : -1;
	}
	saved = errno;
	close(gate[1]);
	waited = view_wait_child(ctx, child, &status, &cancelled);
	if (tty) {
		sigaction(SIGTTOU, &ignore, &previous);
		if (tcsetpgrp(STDIN_FILENO, foreground) && !rc) {
			rc = -1;
			saved = errno;
		}
		if (term_saved)
			tcsetattr(STDIN_FILENO, TCSADRAIN, &terminal);
		sigaction(SIGTTOU, &previous, NULL);
	}
	fyai_child_status_read(status_pipe[0], &result->start);
	close(status_pipe[0]);
	if (waited < 0) {
		rc = -1;
		saved = errno;
	} else {
		result->signaled = WIFSIGNALED(status);
		result->signal = result->signaled ? WTERMSIG(status) : 0;
		result->exit_code = cancelled ? 128 + cancelled :
			WIFEXITED(status) ? WEXITSTATUS(status) : 128 + result->signal;
	}
	errno = saved;
	return rc;
}

int fyai_cmd_view_enter(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	fy_generic arguments = fy_get(call->args, "command", fy_seq_empty);
	const char *argument;
	char **argv = NULL;
	size_t argc = fy_len(arguments), index = 0;
	struct fyai_fsview spec = { 0 };
	struct shell_command_result output = { 0 };
	fy_generic view, project, runtime, storage, snapshot, updated;
	char lockpath[PATH_MAX], error[PATH_MAX] = "", startup[FYAI_CHILD_START_TEXT_MAX];
	char *resolved;
	const char *why;
	int lock = -1, rc = -1, saved;
	bool complete = false, started = false;

	if (view_writable(call))
		return -1;
	view = view_find(call, name);
	if (!fy_is_mapping(view)) {
		fyai_error(call->ctx, "view '%s' does not exist", name);
		return -1;
	}
	if (fy_is_valid(fy_get(view, "mount", fy_invalid))) {
		fyai_error(call->ctx, "view '%s' is mounted; unmount it before entering", name);
		return -1;
	}
	project = fy_get(view, "project", fy_invalid);
	runtime = fy_get(view, "runtime", fy_invalid);
	storage = fy_get(view, "storage", fy_invalid);
	if (!fy_is_string(project) || !fy_is_string(runtime) || !fy_is_string(storage)) {
		fyai_error(call->ctx, "view '%s': invalid stored paths", name);
		return -1;
	}
	spec.project = fy_castp(&project, "");
	spec.runtime = fy_castp(&runtime, "");
	spec.storage = fy_castp(&storage, "");
	spec.arena = call->ctx->cfg->arena_dir;
	spec.verify = fyai_cmd_arg_bool(call, "verify");
	resolved = realpath(spec.runtime, NULL);
	if (!resolved)
		goto out;
	rc = strcmp(resolved, spec.runtime);
	free(resolved);
	if (rc || strncmp(spec.runtime, spec.storage, strlen(spec.storage)) ||
	    strncmp(spec.runtime + strlen(spec.storage), "/views/view-", 12)) {
		errno = EINVAL;
		goto out;
	}
	rc = snprintf(lockpath, sizeof(lockpath), "%s/lock", spec.runtime);
	if (rc < 0 || rc >= (int)sizeof(lockpath)) {
		errno = ENAMETOOLONG;
		goto out;
	}
	lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0)
		goto out;
	rc = flock(lock, LOCK_EX | LOCK_NB);
	if (rc)
		goto out;
	view = fy_assoc(call->ctx->gb, view, "state", "running");
	rc = view_save(call->ctx, name, view);
	if (rc)
		goto out;
	started = true;
	argv = calloc(argc + 1, sizeof(*argv));
	if (!argv)
		goto out;
	fy_foreach(argument, arguments)
		argv[index++] = (char *)argument;
	rc = view_exec(call->ctx, &spec, argv, &output);
	if (rc)
		goto out;
	why = fyai_child_start_text(&output.start, NULL, spec.project, startup, sizeof(startup));
	if (why) {
		fyai_error(call->ctx, "view '%s': %s", name, why);
		rc = -1;
		goto out;
	}
	snapshot = fyai_fsview_snapshot(call->ctx->gb, &spec, error, sizeof(error));
	if (!fy_is_valid(snapshot)) {
		rc = -1;
		goto out;
	}
	updated = fy_assoc(call->ctx->gb, view, "result", snapshot);
	updated = fy_assoc(call->ctx->gb, updated, "state", "ready");
	rc = view_save(call->ctx, name, updated);
	if (rc)
		goto out;
	call->ctx->cfg->exit_status = output.exit_code;
	*result = fy_invalid;
	complete = true;
out:
	saved = errno;
	if (started && !complete) {
		updated = fy_assoc(call->ctx->gb, view, "state", "incomplete");
		view_save(call->ctx, name, updated);
	}
	if (lock >= 0)
		close(lock);
	free(argv);
	shell_command_result_cleanup(&output);
	if (!complete)
		fyai_error(call->ctx, "view '%s': cannot complete execution%s%s: %s", name,
			   *error ? " at " : "", error, strerror(saved));
	return complete ? 0 : -1;
}

static fy_generic view_mount_record(struct fy_generic_builder *gb, const char *path,
				   const struct fyai_fsview_mount *identity)
{
	return fy_mapping(gb, "path", fy_value(gb, path), "readonly", true,
		"namespace_device", (long long)identity->namespace_device,
		"namespace_inode", (long long)identity->namespace_inode,
		"mount_id", (long long)identity->mount_id,
		"cover_id", (long long)identity->cover_id,
		"root_inode", (long long)identity->root_inode);
}

static int view_mount_paths(fy_generic view, struct fyai_fsview *spec)
{
	fy_generic project, runtime, storage;

	project = fy_get(view, "project", fy_invalid);
	runtime = fy_get(view, "runtime", fy_invalid);
	storage = fy_get(view, "storage", fy_invalid);
	if (!fy_is_string(project) || !fy_is_string(runtime) || !fy_is_string(storage)) {
		errno = EINVAL;
		return -1;
	}
	/* Cast pointers name arena members, not local generic copies. */
	spec->project = fy_get(view, "project", "");
	spec->runtime = fy_get(view, "runtime", "");
	spec->storage = fy_get(view, "storage", "");
	return 0;
}

static bool view_path_contains(const char *parent, const char *path)
{
	size_t length = strlen(parent);

	return !strncmp(parent, path, length) && (!path[length] || path[length] == '/');
}

int fyai_cmd_view_mount(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *target = fyai_cmd_arg_str(call, "path");
	struct fyai_fsview spec = { 0 };
	struct fyai_fsview_mount identity = { 0 };
	fy_generic view, updated;
	char *resolved = NULL, lockpath[PATH_MAX];
	struct dirent *entry;
	DIR *directory = NULL;
	int lock = -1, rc = -1, saved;
	bool complete = false, mounted = false;

	if (view_writable(call))
		return -1;
	view = view_find(call, name);
	if (!fy_is_mapping(view)) {
		fyai_error(call->ctx, "view '%s' does not exist", name);
		return -1;
	}
	if (fy_is_valid(fy_get(view, "mount", fy_invalid))) {
		fyai_error(call->ctx, "view '%s' is already mounted; unmount it first", name);
		return -1;
	}
	rc = view_mount_paths(view, &spec);
	if (rc)
		goto out;
	rc = mkdir(target, 0700);
	if (rc && errno != EEXIST)
		goto out;
	resolved = realpath(target, NULL);
	if (!resolved)
		goto out;
	if (view_path_contains(spec.project, resolved) || view_path_contains(resolved, spec.project) ||
	    view_path_contains(spec.storage, resolved) || view_path_contains(resolved, spec.storage) ||
	    view_path_contains(call->ctx->cfg->arena_dir, resolved) ||
	    view_path_contains(resolved, call->ctx->cfg->arena_dir)) {
		errno = EINVAL;
		goto out;
	}
	directory = opendir(resolved);
	if (!directory)
		goto out;
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			if (errno)
				goto out;
			break;
		}
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
			errno = ENOTEMPTY;
			goto out;
		}
	}
	closedir(directory);
	directory = NULL;
	rc = snprintf(lockpath, sizeof(lockpath), "%s/lock", spec.runtime);
	if (rc < 0 || rc >= (int)sizeof(lockpath)) {
		errno = ENAMETOOLONG;
		goto out;
	}
	lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0)
		goto out;
	rc = flock(lock, LOCK_EX | LOCK_NB);
	if (rc)
		goto out;
	rc = fyai_fsview_mount(&spec, resolved, &identity);
	if (rc)
		goto out;
	mounted = true;
	updated = fy_assoc(call->ctx->gb, view, "mount", view_mount_record(call->ctx->gb, resolved, &identity));
	updated = fy_assoc(call->ctx->gb, updated, "state", "mounted");
	rc = view_save(call->ctx, name, updated);
	if (rc)
		goto out;
	*result = view_summary(call->gb, name, updated);
	complete = true;
out:
	saved = errno;
	if (mounted && !complete)
		fyai_fsview_unmount(&spec, resolved, &identity);
	if (directory)
		closedir(directory);
	if (lock >= 0)
		close(lock);
	if (!complete) {
		if (saved == EPERM || saved == EACCES)
			fyai_error(call->ctx, "view '%s': mount at '%s' requires mount privileges in the current namespace; for rootless inspection start unshare -Urnm sh: %s", name, target, strerror(saved));
		else
			fyai_error(call->ctx, "view '%s': cannot mount at '%s': %s", name, target, strerror(saved));
	}
	free(resolved);
	return complete ? 0 : -1;
}

int fyai_cmd_view_unmount(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	struct fyai_fsview spec = { 0 };
	struct fyai_fsview_mount identity;
	fy_generic view, mount, updated, path;
	char lockpath[PATH_MAX];
	int lock = -1, rc = -1, saved;
	bool complete = false;

	if (view_writable(call))
		return -1;
	view = view_find(call, name);
	mount = fy_get(view, "mount", fy_invalid);
	path = fy_get(mount, "path", fy_invalid);
	if (!fy_is_mapping(view) || !fy_is_mapping(mount) || !fy_is_string(path)) {
		fyai_error(call->ctx, "view '%s' has no recorded mount", name);
		return -1;
	}
	rc = view_mount_paths(view, &spec);
	if (rc)
		goto out;
	identity = (struct fyai_fsview_mount){
		.namespace_device = fy_get(mount, "namespace_device", 0LL),
		.namespace_inode = fy_get(mount, "namespace_inode", 0LL),
		.mount_id = fy_get(mount, "mount_id", 0LL),
		.cover_id = fy_get(mount, "cover_id", 0LL),
		.root_inode = fy_get(mount, "root_inode", 0LL),
	};
	if (!identity.mount_id || !identity.namespace_inode || !identity.root_inode) {
		errno = EINVAL;
		goto out;
	}
	rc = snprintf(lockpath, sizeof(lockpath), "%s/lock", spec.runtime);
	if (rc < 0 || rc >= (int)sizeof(lockpath)) {
		errno = ENAMETOOLONG;
		goto out;
	}
	lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0)
		goto out;
	rc = flock(lock, LOCK_EX | LOCK_NB);
	if (rc)
		goto out;
	rc = fyai_fsview_unmount(&spec, fy_castp(&path, ""), &identity);
	if (rc) {
		saved = errno;
		if (identity.cover_id != (uint64_t)fy_get(mount, "cover_id", 0LL)) {
			updated = fy_assoc(call->ctx->gb, view, "mount", view_mount_record(call->ctx->gb, fy_castp(&path, ""), &identity));
			view_save(call->ctx, name, updated);
		}
		errno = saved;
		goto out;
	}
	updated = fy_disassoc(call->ctx->gb, view, "mount");
	updated = fy_assoc(call->ctx->gb, updated, "state", "ready");
	rc = view_save(call->ctx, name, updated);
	if (rc)
		goto out;
	*result = view_summary(call->gb, name, updated);
	complete = true;
out:
	saved = errno;
	if (lock >= 0)
		close(lock);
	if (!complete)
		fyai_error(call->ctx, "view '%s': cannot unmount the recorded inspection mount: %s", name, strerror(saved));
	return complete ? 0 : -1;
}

#else
int fyai_cmd_view_create(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_show(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_list(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_update(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_enter(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_mount(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_unmount(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

#endif
