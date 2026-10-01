/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_cmd.h"
#include "fyai_fsview.h"
#include "fyai_project_capture.h"
#include "fyai_storage.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

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
	fy_generic baseline, result;

	baseline = fy_get(view, "baseline", fy_invalid);
	result = fy_get(view, "result", baseline);
	return fy_mapping(gb, "name", fy_value(gb, name),
		"project", fy_get(view, "project", ""),
		"baseline", fy_get(baseline, "root", ""),
		"root", fy_get(result, "root", ""),
		"materialization", "copy", "state", fy_get(view, "state", "ready"));
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

int fyai_cmd_view_create(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *project = fyai_cmd_arg_str(call, "project");
	struct fyai_project_capture_opts opts = { .source_fd = -1, .objects_fd = -1, .baseline_fd = -1 };
	fy_generic snapshot, record;
	char *resolved = NULL, *storage = NULL, *objects = NULL, *views = NULL, *runtime = NULL;
	char error[PATH_MAX] = "";
	const char *const directories[] = { "baseline", "upper", "work", "cover", "merged" };
	size_t i;
	int rc = -1, root = -1, saved;
	bool complete = false;

	if (view_writable(call))
		return -1;
	if (fy_is_valid(view_find(call, name))) {
		fyai_error(call->ctx, "view '%s' already exists", name);
		return -1;
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

int fyai_cmd_view_enter(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *command = fyai_cmd_arg_str(call, "command");
	struct fyai_fsview spec = { 0 };
	struct shell_command_opts opts = { 0 };
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
	opts.view = &spec;
	opts.workdir = spec.project;
	opts.timeout_ms = fy_get(call->args, "timeout_ms", 60000LL);
	rc = run_shell_command_capture_cb(call->ctx, command, &output, NULL, NULL, NULL, &opts);
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
	*result = fy_mapping(call->gb, "name", fy_value(call->gb, name),
		"root", fy_get(snapshot, "root", ""), "exit_code", (long long)output.exit_code,
		"timed_out", output.timed_out,
		"stdout", fyai_bytes_to_generic(call->gb, output.stdout_data ? output.stdout_data : "", output.stdout_len),
		"stderr", fyai_bytes_to_generic(call->gb, output.stderr_data ? output.stderr_data : "", output.stderr_len));
	complete = true;
out:
	saved = errno;
	if (started && !complete) {
		updated = fy_assoc(call->ctx->gb, view, "state", "incomplete");
		view_save(call->ctx, name, updated);
	}
	if (lock >= 0)
		close(lock);
	shell_command_result_cleanup(&output);
	if (!complete)
		fyai_error(call->ctx, "view '%s': cannot complete execution%s%s: %s", name,
			   *error ? " at " : "", error, strerror(saved));
	return complete ? 0 : -1;
}
