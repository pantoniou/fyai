/*
 * fyai_cmd_fsview.c - commands of the filesystem views
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <errno.h>

#include "fyai_view.h"

#ifdef __linux__
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
#include <time.h>

#include "fyai_branch.h"
#include "fyai_config.h"
#include "fyai_fsview.h"
#include "fyai_view_apply.h"
#include "fyai_diff.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_project_capture.h"
#include "fyai_storage.h"
#include "fyai_sink.h"
#include "fyai_tools.h"

static fy_generic view_store(struct fyai_ctx *ctx)
{
	struct fyai_branch branch;

	fyai_branch_decode(ctx->branch_prev, &branch);
	return fy_is_mapping(branch.store) ? branch.store : fy_map_empty;
}

static fy_generic view_find(struct fyai_ctx *ctx, const char *name)
{
	fy_generic views;

	views = fy_get(view_store(ctx), "views", fy_map_empty);
	return fy_get(views, name, fy_invalid);
}

static int view_save(struct fyai_ctx *ctx, const char *name, fy_generic view)
{
	fy_generic store, views;
	char *unstored;
	int rc;

	store = view_store(ctx);
	views = fy_get(store, "views", fy_map_empty);
	views = fy_is_valid(view) ? fy_assoc(ctx->gb, views, fy_value(ctx->gb, name), view) :
				    fy_disassoc(ctx->gb, views, fy_value(ctx->gb, name));
	ctx->branch_store = fy_assoc(ctx->gb, store, "views", views);
	if (!fy_is_valid(ctx->branch_store)) {
		errno = ENOMEM;
		return -1;
	}
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMMAND, NULL);
	/* View references must be published before a terminal child opens the
	 * branch. */
	unstored = ctx->session_unstored;
	ctx->session_unstored = NULL;
	rc = fyai_publish_root(ctx, fy_invalid, fy_invalid, fy_invalid);
	if (rc)
		ctx->session_unstored = unstored;
	else
		free(unstored);
	return rc;
}

static fy_generic view_summary(struct fy_generic_builder *gb, const char *name, fy_generic view)
{
	fy_generic baseline, result, summary;

	baseline = fy_get(view, "baseline", fy_invalid);
	result = fy_get(view, "result", baseline);

	summary = fy_mapping(
		gb, "name", fy_value(gb, name), "project", fy_get(view, "project", ""), "baseline",
		fy_get(baseline, "root", ""), "root", fy_get(result, "root", ""), "durability",
		fy_get(view, "durability", "durable"), "synchronized",
		fy_get(view, "synchronized", true), "materialization",
		fy_get(view, "materialization", "copy"), "state", fy_get(view, "state", "ready"));
	if (fy_is_mapping(fy_get(view, "capture", fy_invalid)))
		summary = fy_assoc(gb, summary, "capture", fy_get(view, "capture", fy_invalid));
	if (fy_is_valid(fy_get(view, "mount", fy_invalid)))
		summary = fy_assoc(gb, summary, "mount", fy_get(view, "mount", fy_invalid));
	return summary;
}

static int view_writable(struct fyai_ctx *ctx)
{
	if (ctx->cfg->root_spec || ctx->gb != ctx->durable_gb) {
		fyai_error(ctx, "view: a writable durable arena is required; --root "
				      "and --transient are read-only for views");
		return -1;
	}
	return 0;
}

static char *view_scratch(struct fyai_ctx *ctx)
{
	fy_generic section = fy_get(ctx->cfg->config_doc, "view", fy_invalid);

	return fyai_fsview_scratch(fy_get(section, "scratch_dir", ""));
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
	       strncmp(storage + length + 1, ".fyai/", 6) && strcmp(storage + length + 1, ".fyai");
}

static int view_boot_id(char id[37])
{
	char text[37];
	ssize_t count;
	int fd, saved;

	fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		count = read(fd, text, sizeof(text));
	} while (count < 0 && errno == EINTR);
	saved = errno;
	close(fd);
	errno = saved;
	if (count != 37 || text[36] != '\n') {
		if (count >= 0)
			errno = EIO;
		return -1;
	}
	memcpy(id, text, 36);
	id[36] = '\0';
	return 0;
}

static int view_sync_backing(const char *runtime)
{
	int fd, rc, saved;

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

static int view_recover(struct fyai_ctx *ctx, fy_generic *view, const struct fyai_fsview *spec,
			char *error, size_t error_size)
{
	fy_generic expected;
	char boot[37];
	int rc;

	rc = view_boot_id(boot);
	if (rc)
		return -1;
	if (fy_equal(fy_get(*view, "validated_boot", fy_invalid), boot))
		return 0;
	expected = fy_get(*view, "result", fy_get(*view, "baseline", fy_invalid));
	rc = fyai_fsview_recover(ctx->gb, spec, expected, error, error_size);
	if (rc) {
		if (error && error_size)
			snprintf(error, error_size,
				 "reboot recovery validation failed; view may "
				 "be lost or corrupted; update or recreate it");
		return -1;
	}
	*view = fy_assoc(ctx->gb, *view, "validated_boot", fy_value(ctx->gb, boot));
	if (!fy_is_mapping(*view)) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

static int view_initialize_upper(int runtime, int baseline)
{
	struct stat st;
	struct timespec times[2];
	int fd, rc, saved;

	rc = fstat(baseline, &st);
	if (rc)
		return -1;
	fd = openat(runtime, "upper", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	times[0] = (struct timespec){ .tv_nsec = UTIME_OMIT };
	times[1] = st.st_mtim;
	rc = fchown(fd, st.st_uid, st.st_gid);
	if (!rc)
		rc = fchmod(fd, st.st_mode & 07777);
	if (!rc)
		rc = futimens(fd, times);
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

static const char *view_copy_backend(enum fyai_cas_method backend)
{
	switch (backend) {
	case FYAI_CAS_METHOD_REFLINK:
		return "reflink";
	case FYAI_CAS_METHOD_COPY_RANGE:
		return "copy_file_range";
	case FYAI_CAS_METHOD_SENDFILE:
		return "sendfile";
	default:
		return "none";
	}
}

static fy_generic view_capture_statistics(struct fy_generic_builder *gb,
					  const struct fyai_project_capture_stats *stats)
{
	uint64_t total = stats->copies + stats->reflinks + stats->hardlinks + stats->metacopies;
	double unit = total ? 100.0 / total : 0.0;

	return fy_mapping(gb, "elapsed_ms", (long long)stats->elapsed_ms, "files",
			  (long long)stats->files, "directories", (long long)stats->directories,
			  "symlinks", (long long)stats->symlinks, "logical_bytes",
			  (long long)stats->logical_bytes, "copied_bytes",
			  (long long)stats->copied_bytes, "reflinked_bytes",
			  (long long)stats->reflinked_bytes, "borrowed_bytes",
			  (long long)stats->borrowed_bytes, "borrowed_files",
			  (long long)stats->borrowed_files, "workers", (long long)stats->workers,
			  "copy_backend", view_copy_backend(stats->copy_backend), "storage",
			  fy_mapping(gb, "copy",
				     fy_mapping(gb, "files", (long long)stats->copies, "percent",
						stats->copies * unit),
				     "reflink",
				     fy_mapping(gb, "files", (long long)stats->reflinks, "percent",
						stats->reflinks * unit),
				     "hardlink",
				     fy_mapping(gb, "files", (long long)stats->hardlinks, "percent",
						stats->hardlinks * unit),
				     "metacopy",
				     fy_mapping(gb, "files", (long long)stats->metacopies,
						"percent", stats->metacopies * unit)));
}

struct view_capture_progress {
	struct fyai_ctx *ctx;
	const char *name;
	struct fyai_sink_band *band;
	uint64_t last_ms;
	enum fyai_project_capture_phase phase;
	bool shown;
};

static void view_capture_progress(void *arg, const struct fyai_project_capture_stats *stats)
{
	static const char *const phases[] = { "scanning",  "capturing",		 "materializing",
					      "verifying", "building manifests", "complete" };
	struct view_capture_progress *progress = arg;
	char title[160], body[320];
	uint64_t total = stats->copies + stats->reflinks + stats->hardlinks + stats->metacopies;
	double unit = total ? 100.0 / total : 0.0;
	int length;

	if (progress->shown && progress->phase == stats->phase &&
	    stats->elapsed_ms - progress->last_ms < 500)
		return;
	progress->shown = true;
	progress->phase = stats->phase;
	progress->last_ms = stats->elapsed_ms;
	snprintf(title, sizeof(title), "view %s: %s", progress->name, phases[stats->phase]);
	if (stats->phase == FYAI_PROJECT_SCAN || stats->phase == FYAI_PROJECT_MANIFEST)
		length = snprintf(body, sizeof(body), "%llu files discovered; %.2f s",
				  (unsigned long long)stats->files, stats->elapsed_ms / 1000.0);
	else
		length = snprintf(
			body, sizeof(body),
			"%llu/%llu files, %.1f%%; %.2f s; materialized: copy "
			"%.1f%%, reflink %.1f%%, hardlink %.1f%%, metacopy "
			"%.1f%%; borrowed Git %llu files; %s",
			(unsigned long long)stats->completed, (unsigned long long)stats->files,
			stats->files ? 100.0 * stats->completed / stats->files : 0.0,
			stats->elapsed_ms / 1000.0, stats->copies * unit, stats->reflinks * unit,
			stats->hardlinks * unit, stats->metacopies * unit,
			(unsigned long long)stats->borrowed_files,
			view_copy_backend(stats->copy_backend));
	if (progress->band)
		fyai_sink_band_paint(progress->band, title, NULL, body, length, NULL);
	else
		fyai_report(progress->ctx, "%s: %s\n", title, body);
}

/*
 * Remove only private runtime entries; never change shared blob file metadata.
 */
static int view_runtime_clear(int fd, unsigned int depth)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int child, rc = 0, saved;

	if (depth > 128) {
		errno = ELOOP;
		return -1;
	}
	rc = fchmod(fd, 0700);
	if (rc)
		return -1;
	child = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (child < 0)
		return -1;
	directory = fdopendir(child);
	if (!directory) {
		saved = errno;
		close(child);
		errno = saved;
		return -1;
	}
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			rc = errno ? -1 : 0;
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		rc = fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW);
		if (rc)
			break;
		if (S_ISDIR(st.st_mode)) {
			child = openat(fd, entry->d_name,
				       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (child < 0) {
				rc = -1;
				break;
			}
			rc = view_runtime_clear(child, depth + 1);
			saved = errno;
			close(child);
			errno = saved;
			if (rc)
				break;
		}
		rc = unlinkat(fd, entry->d_name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0);
		if (rc)
			break;
	}
	saved = errno;
	closedir(directory);
	errno = saved;
	return rc;
}

int fyai_view_capture(const struct fyai_view_request *request, fy_generic *record)
{
	struct fyai_ctx *ctx = request->ctx;
	const char *name = request->name;
	const char *project = request->project;
	const char *durability = request->durability;
	bool replace = request->replace;
	struct fyai_project_capture_stats statistics = { 0 };
	struct view_capture_progress progress = { .ctx = ctx, .name = name };
	struct fyai_project_capture_opts opts = { .stats = &statistics,
						  .progress = view_capture_progress,
						  .progress_arg = &progress,
						  .source_fd = -1,
						  .objects_fd = -1,
						  .baseline_fd = -1,
						  .data_fd = -1,
						  .metacopy = true,
						  .defer_sync = true };
	struct fyai_manifest manifest = { 0 }, previous_manifest = { 0 };
	fy_generic snapshot, previous, stored_project, stored_runtime;
	char manifest_name[FYAI_MANIFEST_NAME_SIZE];
	char *resolved = NULL, *storage = NULL, *objects = NULL, *views = NULL, *runtime = NULL;
	char *scratch = NULL;
	char error[PATH_MAX] = "", boot[37];
	const char *lockpath;
	const char *const directories[] = {
		"baseline", "data", "upper", "work", "cover", "merged"
	};
	size_t i;
	int rc = -1, root = -1, lock = -1, gc_lock = -1, saved;
	unsigned int attempt = 0;
	bool complete = false, unstable = false;

	if (view_writable(ctx))
		return -1;
	if (!request->progress)
		opts.progress = NULL;
	previous = view_find(ctx, name);
	if (!durability)
		durability = replace ?
				     fy_get(previous, "durability", "durable") :
				     fy_get(fy_get(ctx->cfg->config_doc, "view", fy_invalid),
					    "durability", "lazy");
	rc = view_boot_id(boot);
	if (rc)
		goto out;
	if (!replace && fy_is_valid(previous)) {
		fyai_error(ctx, "view '%s' already exists", name);
		return -1;
	}
	if (replace && fy_is_valid(fy_get(previous, "mount", fy_invalid))) {
		fyai_error(ctx, "view '%s' is mounted; unmount it before updating", name);
		return -1;
	}
	if (replace) {
		if (!fy_is_mapping(previous)) {
			fyai_error(ctx, "view '%s' does not exist", name);
			return -1;
		}
		stored_project = fy_get(previous, "project", fy_invalid);
		stored_runtime = fy_get(previous, "runtime", fy_invalid);
		if (!fy_is_string(stored_project) || !fy_is_string(stored_runtime)) {
			fyai_error(ctx, "view '%s': invalid stored paths", name);
			return -1;
		}
		project = fy_castp(&stored_project, "");
		lockpath = fy_sprintfa("%s/lock", fy_castp(&stored_runtime, ""));
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
	scratch = view_scratch(ctx);
	if (!scratch)
		goto out;
	if (!fyai_fsview_project_usable(resolved, scratch)) {
		errno = EINVAL;
		goto out;
	}
	rc = view_storage(resolved, &storage, &objects, &views);
	if (rc)
		goto out;
	if (view_storage_beneath(resolved, storage) ||
	    view_storage_beneath(resolved, ctx->cfg->arena_dir)) {
		errno = EINVAL;
		goto out;
	}
	/* The capture and its record in the arena are not collected while it runs. */
	gc_lock = fyai_view_storage_lock_shared(storage);
	if (gc_lock < 0) {
		rc = -1;
		goto out;
	}
retry:
	attempt++;
	unstable = false;
	progress.shown = false;
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
	rc = fyai_fsview_metacopy_check(runtime);
	if (rc && errno != ENOTSUP)
		goto out;
	opts.metacopy = !rc;
	opts.data_fd = openat(root, "data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (opts.data_fd < 0)
		goto out;
	opts.verify = request->verify;
	opts.borrow_git = !request->copy_git_objects;
	opts.source_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	opts.objects_fd = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	opts.baseline_fd =
		openat(root, "baseline", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (opts.source_fd < 0 || opts.objects_fd < 0 || opts.baseline_fd < 0)
		goto out;
	if (replace && fy_equal(fy_get(previous, "validated_boot", fy_invalid), boot) &&
	    !opts.reuse_baseline && !opts.metacopy &&
	    fy_equal(fy_get(previous, "materialization", fy_invalid), "copy") &&
	    fy_equal(fy_get(fy_get(previous, "baseline", fy_invalid), "version", fy_invalid),
		     (long long)FYAI_FSVIEW_SNAPSHOT_VERSION)) {
		lockpath = fy_sprintfa("%s/baseline",       fy_castp(&stored_runtime, ""));
		opts.previous_baseline_fd =
			open(lockpath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (opts.previous_baseline_fd < 0)
			goto out;
		opts.reuse_baseline = true;
		rc = fyai_fsview_manifest_open(fy_get(previous, "storage", ""),
					       fy_get(previous, "baseline", fy_invalid),
					       &previous_manifest);
		if (rc)
			goto out;
		opts.baseline_manifest = &previous_manifest;
	}
	if (opts.progress && !progress.band && fyai_sink_bands_available(ctx->sink))
		progress.band =
			fyai_sink_band_open(ctx->sink, false, "Capturing project view", NULL);
	rc = fyai_project_capture_manifest(&opts, &manifest, error, sizeof(error));
	if (rc) {
		unstable = errno == EAGAIN;
		if (errno != EAGAIN || attempt >= 3)
			goto out;
		if (opts.progress) {
			if (progress.band)
				fyai_sink_band_paint(progress.band,
						     "Project changed during "
						     "capture; retrying",
						     NULL, error, strlen(error), NULL);
			else
				fyai_report(ctx,
					    "view %s: project changed during "
					    "capture at %s; retrying (%u/3)\n",
					    name, *error ? error : ".", attempt + 1);
		}
		close(opts.source_fd);
		close(opts.objects_fd);
		close(opts.baseline_fd);
		close(opts.data_fd);
		opts.source_fd = opts.objects_fd = opts.baseline_fd = opts.data_fd = -1;
		rc = view_runtime_clear(root, 0);
		if (rc)
			goto out;
		close(root);
		root = -1;
		rc = rmdir(runtime);
		if (rc)
			goto out;
		free(runtime);
		runtime = NULL;
		goto retry;
	}
	rc = fyai_fsview_manifest_publish(storage, &manifest, !strcmp(durability, "durable"),
					  manifest_name);
	if (rc)
		goto out;
	snapshot = fyai_fsview_reference(ctx->gb, manifest.root, manifest.object_count,
					 manifest_name);
	fyai_manifest_close(&manifest);
	rc = view_initialize_upper(root, opts.baseline_fd);
	if (!rc && !strcmp(durability, "durable"))
		rc = view_sync_backing(runtime);
	if (rc)
		goto out;
	*record = fy_mapping(ctx->gb,
		"version", 1LL,
		"name", fy_value(ctx->gb, name),
		"project", fy_value(ctx->gb, resolved),
		"storage", fy_value(ctx->gb, storage),
		"runtime", fy_value(ctx->gb, runtime),
		"baseline", snapshot,
		"durability", fy_value(ctx->gb, durability),
		"synchronized", (bool)!strcmp(durability, "durable"),
		"validated_boot", fy_value(ctx->gb, boot),
		"state", "ready",
		"materialization", opts.metacopy ? "metacopy" : "copy",
		"capture", fy_assoc(ctx->gb, view_capture_statistics(ctx->gb, &statistics),
				    "attempts", (long long)attempt));
	rc = view_save(ctx, name, *record);
	if (rc)
		goto out;
	complete = true;
out:
	saved = errno;
	if (progress.band) {
		if (!complete)
			fyai_sink_band_paint(progress.band, "Project view capture failed", NULL,
					     strerror(saved), strlen(strerror(saved)), NULL);
		fyai_sink_band_commit(progress.band);
		fyai_sink_band_destroy(progress.band);
	}

	if (!complete && root >= 0) {
		rc = view_runtime_clear(root, 0);
		if (!rc)
			rc = rmdir(runtime);
		if (rc)
			fyai_warning(ctx,
				     "view '%s': cannot remove failed runtime "
				     "'%s': %s",
				     name, runtime, strerror(errno));
	}

	if (opts.reuse_baseline)
		close(opts.previous_baseline_fd);
	fyai_manifest_close(&manifest);
	fyai_manifest_close(&previous_manifest);
	if (opts.source_fd >= 0)
		close(opts.source_fd);
	if (opts.objects_fd >= 0)
		close(opts.objects_fd);
	if (opts.baseline_fd >= 0)
		close(opts.baseline_fd);
	if (opts.data_fd >= 0)
		close(opts.data_fd);
	if (root >= 0)
		close(root);
	if (lock >= 0)
		close(lock);
	if (gc_lock >= 0)
		close(gc_lock);
	if (!complete && unstable && saved == EAGAIN)
		fyai_error(ctx,
			   "view '%s': project changed during capture at %s after "
			   "%u attempts; retry when project activity stops",
			   name, *error ? error : ".", attempt);
	else if (!complete)
		fyai_error(ctx, "view '%s': cannot capture '%s'%s%s: %s", name, project,
			   *error ? " at " : "", error, strerror(saved));
	free(resolved);
	free(storage);
	free(objects);
	free(views);
	free(runtime);
	free(scratch);
	return complete ? 0 : -1;
}

/* A file larger than this is compared by its digest and never read. */
#define VIEW_DIFF_READ_LIMIT ((uint64_t)64 << 20)

struct view_diff_side {
	bool present;
	bool opaque;
	unsigned int mode;
	unsigned char *data;
	size_t length;
	const struct fyai_mobject *object;
};

static void view_diff_side_free(struct view_diff_side *side)
{
	free(side->data);
	memset(side, 0, sizeof(*side));
}

/* Load the bytes of a file or the target of a symlink; a directory is absent. */
static int view_diff_side_load(struct view_diff_side *side, int objects,
			       const struct fyai_mobject *object)
{
	struct stat st;
	size_t done = 0;
	ssize_t n;
	int source;

	memset(side, 0, sizeof(*side));
	side->object = object;
	if (object->kind == FYAI_PROJECT_SYMLINK) {
		side->present = true;
		side->mode = 0120000;
		side->length = object->target_length;
		side->data = malloc(side->length ? side->length : 1);
		if (!side->data)
			return -1;
		memcpy(side->data, object->target, side->length);
		return 0;
	}
	if (object->kind != FYAI_PROJECT_FILE)
		return 0;
	side->present = true;
	side->mode = object->meta.mode & 0111 ? 0100755 : 0100644;
	if (object->blob.size > VIEW_DIFF_READ_LIMIT) {
		side->opaque = true;
		return 0;
	}
	source = fyai_cas_open(objects, &object->blob);
	if (source < 0)
		return -1;
	if (fstat(source, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != object->blob.size) {
		close(source);
		errno = EIO;
		return -1;
	}
	side->length = (size_t)object->blob.size;
	side->data = malloc(side->length ? side->length : 1);
	if (!side->data) {
		close(source);
		return -1;
	}
	while (done < side->length) {
		n = read(source, side->data + done, side->length - done);
		if (n <= 0) {
			close(source);
			errno = n < 0 ? errno : EIO;
			return -1;
		}
		done += (size_t)n;
	}
	close(source);
	return 0;
}

static bool view_diff_side_same(const struct view_diff_side *a, const struct view_diff_side *b)
{
	if (a->opaque || b->opaque)
		return a->opaque && b->opaque &&
		       !memcmp(a->object->blob.digest, b->object->blob.digest, FYAI_CAS_HASH_SIZE);
	return a->length == b->length && (!a->length || !memcmp(a->data, b->data, a->length));
}

static bool view_diff_side_binary(const struct view_diff_side *side)
{
	size_t probe = side->length < 8000 ? side->length : 8000;

	return side->opaque || (probe && memchr(side->data, '\0', probe));
}

/* Append the diff of one path in the extended git format; a side may be absent. */
static int view_diff_part(struct response_buffer *out, const char *path,
			  const struct view_diff_side *a, const struct view_diff_side *b)
{
	char line[PATH_MAX * 2 + 64], *text = NULL;
	char aname[PATH_MAX + 8], bname[PATH_MAX + 8];
	int rc = -1;

	snprintf(aname, sizeof(aname), a->present ? "a/%s" : "/dev/null", path);
	snprintf(bname, sizeof(bname), b->present ? "b/%s" : "/dev/null", path);
	if (a->present && b->present && a->mode == b->mode && view_diff_side_same(a, b))
		return 0;
	snprintf(line, sizeof(line), "diff --git a/%s b/%s\n", path, path);
	if (response_buffer_append(out, line))
		return -1;
	if (!a->present)
		snprintf(line, sizeof(line), "new file mode %06o\n", b->mode);
	else if (!b->present)
		snprintf(line, sizeof(line), "deleted file mode %06o\n", a->mode);
	else if (a->mode != b->mode)
		snprintf(line, sizeof(line), "old mode %06o\nnew mode %06o\n", a->mode, b->mode);
	else
		line[0] = '\0';
	if (response_buffer_append(out, line))
		return -1;
	if (a->present && b->present && view_diff_side_same(a, b))
		return 0;
	if (view_diff_side_binary(a) || view_diff_side_binary(b)) {
		snprintf(line, sizeof(line), "Binary files %s and %s differ\n", aname, bname);
		return response_buffer_append(out, line);
	}
	if (fyai_diff_unified(a->data ? (const char *)a->data : "", a->length,
			      b->data ? (const char *)b->data : "", b->length, aname, bname, 3,
			      &text))
		return -1;
	rc = response_buffer_append(out, text);
	free(text);
	return rc;
}

static int view_diff_path(struct response_buffer *out, struct fyai_manifest manifests[2],
			  int objects[2], const char *path)
{
	struct view_diff_side side[2] = { { 0 }, { 0 } };
	struct view_diff_side none = { 0 };
	struct fyai_mobject object[2];
	size_t i, length = strlen(path);
	int rc = -1, saved;

	for (i = 0; i < 2; i++) {
		if (!fyai_manifest_lookup(&manifests[i], (const unsigned char *)path, length,
					  &object[i], NULL))
			continue;
		if (view_diff_side_load(&side[i], objects[i], &object[i]))
			goto out;
	}
	if (side[0].present && side[1].present &&
	    (side[0].mode & 0170000) != (side[1].mode & 0170000)) {
		/* A change of type is a removal and an addition, as git writes it. */
		if (view_diff_part(out, path, &side[0], &none) ||
		    view_diff_part(out, path, &none, &side[1]))
			goto out;
	} else if (side[0].present || side[1].present) {
		if (view_diff_part(out, path, &side[0], &side[1]))
			goto out;
	}
	rc = 0;
out:
	saved = errno;
	view_diff_side_free(&side[0]);
	view_diff_side_free(&side[1]);
	errno = saved;
	return rc;
}

static int view_diff_recover(struct fyai_ctx *ctx, const char *name, fy_generic *view)
{
	struct fyai_fsview spec = { 0 };
	char boot[37], error[PATH_MAX] = "";
	const char *lockpath;
	int lock = -1, rc, saved;

	if (view_boot_id(boot))
		return -1;
	if (fy_equal(fy_get(*view, "validated_boot", fy_invalid), boot))
		return 0;
	spec.project = fy_get(*view, "project", "");
	spec.runtime = fy_get(*view, "runtime", "");
	spec.storage = fy_get(*view, "storage", "");
	spec.baseline = fy_get(*view, "baseline", fy_invalid);
	spec.metacopy = fy_equal(fy_get(*view, "materialization", ""), "metacopy");
	lockpath = fy_sprintfa("%s/lock", spec.runtime);
	lock = open(lockpath, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		rc = -1;
		goto out;
	}
	rc = view_recover(ctx, view, &spec, error, sizeof(error));
	if (!rc && !ctx->cfg->root_spec && ctx->gb == ctx->durable_gb)
		rc = view_save(ctx, name, *view);
out:
	saved = errno;
	if (lock >= 0)
		close(lock);
	if (rc)
		fyai_error(ctx, "view '%s': cannot validate after reboot%s%s: %s", name,
			   *error ? ": " : "", error, strerror(saved));
	return rc ? -1 : 0;
}

#define view_state_error(quiet, ctx, ...) \
	do { \
		if (!(quiet)) \
			fyai_error(ctx, __VA_ARGS__); \
	} while (0)

/* A snapshot that a command compares: a view, or the project state of a reference. */
struct view_state {
	fy_generic reference;
	fy_generic baseline;
	char storage[PATH_MAX];
	/* The directory that the snapshot is of, and that an apply writes. */
	char project[PATH_MAX];
	bool is_view;
};

static int view_state_set(struct fyai_ctx *ctx, struct view_state *state, fy_generic reference,
			  const char *storage, const char *what, bool quiet)
{
	size_t length;

	if (!fy_is_mapping(reference) || fy_str_empty(storage) || strlen(storage) >= PATH_MAX) {
		view_state_error(quiet, ctx, "no project state is recorded at '%s'", what);
		return -1;
	}
	state->reference = reference;
	strcpy(state->storage, storage);
	/* The storage of a project is its .fyai directory. */
	if (!state->project[0]) {
		length = strlen(storage);
		if (length > 6 && !strcmp(storage + length - 6, "/.fyai"))
			snprintf(state->project, sizeof(state->project), "%.*s", (int)(length - 6),
				 storage);
	}
	return 0;
}

/* The oldest entry of the run that holds the head, so the state is that of the turn's end. */
static fy_generic view_entry_of_head(struct fyai_ctx *ctx, struct fyai_branch *b, fy_generic head)
{
	struct fyai_branch cur = *b, next;
	fy_generic found = fy_invalid;

	while (fy_is_valid(cur.entry)) {
		if (cur.head.v == head.v)
			found = cur.entry;
		else if (fy_is_valid(found))
			break;
		if (!fyai_branch_entry_contained(ctx->durable_allocator, cur.prev, 1) ||
		    !fyai_branch_decode(cur.prev, &next))
			break;
		cur = next;
	}
	return found;
}

/*
 * Resolve a name to a snapshot. A view gives its result, and its baseline for
 * the comparison with one name. A reference (BRANCH, BRANCH~N, BRANCH@{N}, HEAD)
 * gives the project state that its ref-log entry recorded, or the nearest earlier
 * entry that recorded one.
 */
static int view_state_find(struct fyai_ctx *ctx, const char *spec, struct view_state *state,
			   bool quiet)
{
	char parsed[256];
	struct fyai_branch b, e;
	fy_generic view, cur, entry;
	const char *name;
	long long n, i;
	int kind;

	memset(state, 0, sizeof(*state));
	view = view_find(ctx, spec);
	if (fy_is_mapping(view)) {
		if (view_diff_recover(ctx, spec, &view))
			return -1;
		state->is_view = true;
		state->baseline = fy_get(view, "baseline", fy_invalid);
		snprintf(state->project, sizeof(state->project), "%s", fy_get(view, "project", ""));
		return view_state_set(ctx, state, fy_get(view, "result", state->baseline),
				      fy_get(view, "storage", ""), spec, quiet);
	}
	kind = fyai_ref_parse(spec, parsed, sizeof(parsed), &n);
	if (kind < 0) {
		view_state_error(quiet, ctx, "'%s' is not a view or a reference; use a view name, <branch>, "
				"<branch>~N or <branch>@{N}", spec);
		return -1;
	}
	name = !strcmp(parsed, "HEAD") ? fyai_ctx_branch(ctx) : parsed;
	if (!fyai_branch_name_ref_valid(name) ||
	    !fyai_branch_lookup(ctx->arena_branches, name, &b)) {
		view_state_error(quiet, ctx, "'%s' is not a view, and there is no branch '%s'", spec, name);
		return -1;
	}
	entry = b.entry;
	if (kind == '@') {
		for (i = 0; i < n; i++) {
			if (!fyai_branch_entry_contained(ctx->durable_allocator, b.prev, 1) ||
			    !fyai_branch_decode(b.prev, &b)) {
				view_state_error(quiet, ctx, "%s: the ref log has no entry %lld", name, n);
				return -1;
			}
		}
		entry = b.entry;
	} else if (kind == '~') {
		cur = b.head;
		for (i = 0; i < n; i++) {
			if (!fy_is_valid(cur) || fy_is_null(cur)) {
				view_state_error(quiet, ctx, "%s: only %lld turns, cannot go back %lld", name, i, n);
				return -1;
			}
			cur = fy_get(cur, "previous");
		}
		entry = view_entry_of_head(ctx, &b, cur);
		if (!fy_is_valid(entry)) {
			view_state_error(quiet, ctx, "%s: no ref-log entry holds that turn", spec);
			return -1;
		}
	}
	/* An entry that recorded no state is at the state of the entry before it. */
	for (i = 0; fy_is_valid(entry) && i < 4096; i++) {
		if (!fyai_branch_decode(entry, &e))
			break;
		if (fy_is_mapping(e.project))
			return view_state_set(ctx, state, e.project,
					      fy_get(e.project, "storage", ""), spec, quiet);
		if (!fyai_branch_entry_contained(ctx->durable_allocator, e.prev, 1))
			break;
		entry = e.prev;
	}
	view_state_error(quiet, ctx, "no project state is recorded at '%s'; set view/track_project", spec);
	return -1;
}

static int view_state_resolve(struct fyai_ctx *ctx, const char *spec, struct view_state *state)
{
	return view_state_find(ctx, spec, state, false);
}

/*
 * The paths that the project ignores are kept out of what faces the user and the
 * project: a diff, a list of changes and an apply. A view holds the whole project,
 * build artifacts included, because the agent that works in it needs them; the
 * rules apply when its changes are shown or written back. They are the rules of
 * view/ignore, then the .gitignore files of the project on disk, as git reads them.
 * NULL when no rule applies or the project cannot be read.
 */
static struct fyai_ignore_tree *view_ignore_tree(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
						 const char *project, struct fyai_ignore_spec *spec)
{
	struct fyai_ignore_tree *tree;
	int root;

	if (fy_str_empty(project))
		return NULL;
	fyai_ignore_spec_load(spec, fyai_ignore_record(gb, ctx->cfg->config_doc));
	root = open(project, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root < 0)
		return NULL;
	tree = fyai_ignore_tree_open(spec, root);
	close(root);
	return tree;
}

static bool view_ignore_skip(void *arg, const char *path, bool is_dir)
{
	return fyai_ignore_tree_match(arg, path, is_dir);
}

/* The rows of a list of changes that the project does not ignore. */
static fy_generic view_changes_visible(struct fy_generic_builder *gb, struct fyai_ignore_tree *tree,
				       fy_generic changes)
{
	fy_generic kept, row;
	bool is_dir;

	if (!tree)
		return changes;
	kept = fy_sequence(gb);
	fy_foreach(row, changes) {
		is_dir = fy_equal(fy_get(fy_get(row, "before", fy_invalid), "kind", ""), "directory") ||
			 fy_equal(fy_get(fy_get(row, "after", fy_invalid), "kind", ""), "directory");
		if (!fyai_ignore_tree_match(tree, fy_get(row, "path", ""), is_dir))
			kept = fy_append(gb, kept, row);
	}
	return kept;
}

int fyai_cmd_view_diff(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *other = fyai_cmd_arg_str(call, "other");
	struct response_buffer listing = { 0 };
	struct fyai_manifest manifests[2] = { { 0 }, { 0 } };
	struct view_state left = { 0 }, right = { 0 };
	struct fyai_ignore_spec ignore;
	struct fyai_ignore_tree *tree;
	fy_generic snapshots[2], changes, row, patch, before, after, note;
	const char *objects;
	const char *stores[2], *path, *error_path = "";
	int cas[2] = { -1, -1 };
	int rc = -1, saved;
	size_t i;

	if (view_state_resolve(call->ctx, name, &left) ||
	    (other && view_state_resolve(call->ctx, other, &right)))
		return -1;
	if (!other && !left.is_view) {
		/* A reference alone is compared with the head of the branch. */
		if (view_state_resolve(call->ctx, "HEAD", &right))
			return -1;
	} else if (!other) {
		right = left;
		left.reference = left.baseline;
	}
	snapshots[0] = left.reference;
	snapshots[1] = right.reference;
	stores[0] = left.storage;
	stores[1] = right.storage;
	if (fyai_fsview_manifest_open(stores[0], snapshots[0], &manifests[0]) ||
	    fyai_fsview_manifest_open(stores[1], snapshots[1], &manifests[1]))
		goto out;
	changes = fyai_manifest_diff(call->gb, &manifests[0], &manifests[1]);
	if (!fy_is_sequence(changes))
		goto out;
	/* What the project ignores is not a change that the user is asked about. */
	tree = view_ignore_tree(call->ctx, call->gb, *right.project ? right.project : left.project,
				&ignore);
	changes = view_changes_visible(call->gb, tree, changes);
	fyai_ignore_tree_close(tree);
	*result = fy_mapping(call->gb, "changes", changes);
	if (!fy_len(changes)) {
		*result = call->format == FYAI_CMD_OUT_MARKDOWN ?
				  fy_invalid :
				  fy_assoc(call->gb, *result, "patch", "");
		rc = 0;
		goto out;
	}
	if (fyai_cmd_arg_bool(call, "stat")) {
		fy_foreach(row, changes) {
			if (response_buffer_append(&listing, fy_get(row, "status", "")) ||
			    response_buffer_append(&listing, " ") ||
			    response_buffer_append(&listing, fy_get(row, "path", "")) ||
			    response_buffer_append(&listing, "\n"))
				goto out;
		}
		*result = fy_assoc(call->gb, *result, "patch", fy_value(call->gb, listing.data));
		rc = 0;
		goto out;
	}
	for (i = 0; i < 2; i++) {
		objects = fy_sprintfa("%s/objects/blake3", stores[i]);
		cas[i] = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (cas[i] < 0)
			goto out;
	}
	fy_foreach(row, changes) {
		path = fy_get(row, "path", "");
		if (!strcmp(path, "."))
			continue;
		error_path = path;
		if (view_diff_path(&listing, manifests, cas, path))
			goto out;
	}
	error_path = "";
	fy_foreach(row, changes) {
		before = fy_get(row, "before", fy_invalid);
		after = fy_get(row, "after", fy_invalid);
		if (!fy_equal(fy_get(before, "kind", ""), "directory") &&
		    !fy_equal(fy_get(after, "kind", ""), "directory") &&
		    (!fy_is_mapping(before) || !fy_is_mapping(after) ||
		     !fy_equal(fy_get(before, "kind", ""), fy_get(after, "kind", "")) ||
		     !fy_equal(fy_get(before, "digest", fy_null),
			       fy_get(after, "digest", fy_null)) ||
		     !fy_equal(fy_get(before, "target_hex", fy_null),
			       fy_get(after, "target_hex", fy_null))))
			continue;
		error_path = "formatting metadata";
		note = fy_emit(call->gb, row,
			       FYOPEF_DISABLE_DIRECTORY | FYOPEF_NO_ENDING_NEWLINE |
				       FYOPEF_MODE_JSON | FYOPEF_STYLE_COMPACT | FYOPEF_WIDTH_INF,
			       NULL);
		if (!fy_is_string(note) || response_buffer_append(&listing, "# metadata ") ||
		    response_buffer_append(&listing, fy_castp(&note, "")) ||
		    response_buffer_append(&listing, "\n")) {
			rc = -1;
			goto out;
		}
	}
	error_path = "";
	patch = fy_value(call->gb, listing.data ? listing.data : "");
	*result = fy_assoc(call->gb, *result, "patch", patch);
	rc = 0;
	if (call->format == FYAI_CMD_OUT_MARKDOWN) {
		rc = fyai_present_diff(call->ctx, fy_castp(&patch, ""),
				       fyai_cmd_arg_bool(call, "unified"));
		*result = fy_invalid;
	}
out:
	saved = errno;
	for (i = 0; i < 2; i++) {
		if (cas[i] >= 0)
			close(cas[i]);
		fyai_manifest_close(&manifests[i]);
	}
	free(listing.data);
	if (rc)
		fyai_error(call->ctx, "view '%s': cannot diff%s%s: %s", name,
			   *error_path ? " at " : "", error_path, strerror(saved));
	return rc ? -1 : 0;
}

fy_generic fyai_view_list(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	(void)gb;
	return fy_get(view_store(ctx), "views", fy_map_empty);
}

/*
 * Record that a result was applied. The project now holds files that the next
 * entry of the ref log should record, so with track_project it takes the state
 * that the project has after the apply. A view keeps the result that was
 * applied, which `view list` shows; its baseline stays, because the baseline
 * tree is the lower layer of the mounts (view update takes a new one). A
 * failure is a warning: the files are written.
 */
static void view_applied_record(struct fyai_ctx *ctx, const char *name,
				const struct view_state *target,
				const struct fyai_apply_summary *summary)
{
	fy_generic view, captured, applied;

	if (ctx->cfg->root_spec || ctx->gb != ctx->durable_gb)
		return;
	if (fyai_project_state_enabled(ctx) && !fyai_project_state_capture(ctx, &captured) &&
	    fy_is_mapping(captured))
		ctx->project_state = captured;
	view = target->is_view ? view_find(ctx, name) : fy_invalid;
	if (fy_is_mapping(view)) {
		applied = fy_mapping(ctx->gb, "root", fy_get(target->reference, "root", ""), "at",
				     (long long)fyai_branch_timestamp(), "applied",
				     (long long)summary->applied, "conflicts",
				     (long long)summary->conflicts);
		view_save(ctx, name, fy_assoc(ctx->gb, view, "applied", applied));
	} else if (fy_is_mapping(ctx->project_state)) {
		fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMMAND, NULL);
		fyai_publish_state(ctx);
	}
	ctx->project_state = fy_invalid;
}

int fyai_view_pull(struct fyai_ctx *ctx, struct fy_generic_builder *gb, const char *name,
		   const char *base, const char *const *paths, size_t count,
		   enum fyai_view_pull_mode mode, fy_generic *result)
{
	struct view_state target = { 0 }, from = { 0 };
	struct fyai_apply_summary summary;
	struct fyai_manifest manifests[2] = { { 0 }, { 0 } };
	struct fyai_ignore_spec ignore;
	struct fyai_ignore_tree *tree = NULL;
	fy_generic rows;
	const char *objects;
	int root = -1, cas = -1, rc = -1, saved;

	if (view_state_find(ctx, name, &target, false))
		return -1;
	/*
	 * The change is from the base to the target. A view has its baseline for a
	 * base, and a reference has the head of the branch.
	 */
	if (base) {
		if (view_state_find(ctx, base, &from, false))
			return -1;
	} else if (target.is_view) {
		from = target;
		from.reference = target.baseline;
	} else if (view_state_find(ctx, "HEAD", &from, false)) {
		return -1;
	}
	if (fyai_fsview_manifest_open(from.storage, from.reference, &manifests[0]) ||
	    fyai_fsview_manifest_open(target.storage, target.reference, &manifests[1]))
		goto out;
	tree = view_ignore_tree(ctx, gb, target.project, &ignore);
	if (mode == FYAI_VIEW_PULL_CHANGES) {
		rows = fyai_manifest_diff(gb, &manifests[0], &manifests[1]);
		if (!fy_is_sequence(rows))
			goto out;
		*result = fy_mapping(gb, "changes", view_changes_visible(gb, tree, rows));
		rc = 0;
		goto out;
	}
	if (!target.project[0]) {
		errno = EINVAL;
		goto out;
	}
	root = open(target.project, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	objects = fy_sprintfa("%s/objects/blake3", target.storage);
	cas = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root < 0 || cas < 0)
		goto out;
	rc = fyai_view_apply_filtered(gb, root, cas, &manifests[0], &manifests[1], paths, count,
				      mode == FYAI_VIEW_PULL_DRY_RUN, tree ? view_ignore_skip : NULL, tree,
				      &rows, &summary);
	if (rc)
		goto out;
	*result = fy_mapping(gb, "applied", (long long)summary.applied, "satisfied",
			     (long long)summary.satisfied, "conflicts", (long long)summary.conflicts,
			     "skipped", (long long)summary.skipped, "changes", rows);
	if (summary.conflicts)
		ctx->cfg->exit_status = 1;
	if (mode == FYAI_VIEW_PULL_APPLY && summary.applied)
		view_applied_record(ctx, name, &target, &summary);
out:
	saved = errno;
	fyai_ignore_tree_close(tree);
	if (root >= 0)
		close(root);
	if (cas >= 0)
		close(cas);
	fyai_manifest_close(&manifests[0]);
	fyai_manifest_close(&manifests[1]);
	if (rc)
		fyai_error(ctx, "view '%s': cannot %s '%s': %s", name,
			   mode == FYAI_VIEW_PULL_CHANGES ? "list the changes of" : "apply to",
			   target.project, strerror(saved));
	errno = saved;
	return rc ? -1 : 0;
}

int fyai_cmd_view_apply(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic paths = fy_get(call->args, "paths", fy_seq_empty);
	const char *path, **selected;
	size_t count = fy_len(paths), index = 0;
	int rc;

	selected = calloc(count + 1, sizeof(*selected));
	if (!selected) {
		fyai_error(call->ctx, "could not allocate the path list");
		return -1;
	}
	fy_foreach(path, paths)
		selected[index++] = path;
	rc = fyai_view_pull(call->ctx, call->gb, fyai_cmd_arg_str(call, "name"),
			    fyai_cmd_arg_str(call, "base"), selected, count,
			    fyai_cmd_arg_bool(call, "dry_run") ? FYAI_VIEW_PULL_DRY_RUN :
								 FYAI_VIEW_PULL_APPLY,
			    result);
	free(selected);
	return rc;
}

int fyai_view_pull_agent(struct fyai_ctx *ctx, struct fy_generic_builder *gb, const char *child,
			 const char *const *paths, size_t count, enum fyai_view_pull_mode mode,
			 fy_generic *result)
{
	char name[FYAI_BRANCH_NAME_MAX + sizeof(FYAI_VIEW_AGENT_PREFIX)];

	/*
	 * A child is one name: a separator, a marker or a reference character
	 * would address another branch, a user view or a reference.
	 */
	if (fy_str_empty(child) || strlen(child) > FYAI_BRANCH_NAME_MAX ||
	    !fyai_branch_name_valid(child) || strchr(child, '/')) {
		fyai_error(ctx, "'%s' is not the name of a sub-agent", child ? child : "");
		return -1;
	}
	snprintf(name, sizeof(name), FYAI_VIEW_AGENT_PREFIX "%s", child);
	/* The view is looked up first, so a name that is not one never reaches the references. */
	if (!fy_is_mapping(view_find(ctx, name))) {
		fyai_error(ctx, "no sub-agent '%s' of this agent left a view", child);
		return -1;
	}
	return fyai_view_pull(ctx, gb, name, NULL, paths, count, mode, result);
}

static int view_capture(struct fyai_cmd_call *call, fy_generic *result, bool replace)
{
	struct fyai_view_request request = {
		.ctx = call->ctx,
		.name = fyai_cmd_arg_str(call, "name"),
		.project = fyai_cmd_arg_str(call, "project"),
		.durability = fyai_cmd_arg_str(call, "durability"),
		.replace = replace,
		.verify = fyai_cmd_arg_bool(call, "verify"),
		.copy_git_objects = fyai_cmd_arg_bool(call, "copy_git_objects"),
		.progress = fyai_cmd_arg_bool(call, "debug") && !fyai_cmd_arg_bool(call, "quiet"),
	};
	fy_generic record;
	int rc;

	/* The views of sub-agents are made by the agent tool, never by a user. */
	if (!replace && fyai_view_name_is_agent(request.name)) {
		fyai_error(call->ctx, "view '%s': the name is reserved for sub-agent views",
			   request.name);
		return -1;
	}
	rc = fyai_view_capture(&request, &record);
	if (rc)
		return -1;
	*result = call->format != FYAI_CMD_OUT_MARKDOWN || request.progress ?
			  view_summary(call->gb, request.name, record) :
			  fy_invalid;
	return 0;
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

	view = view_find(call->ctx, name);
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
	fy_generic view, views, row, yaml;

	views = fy_get(view_store(call->ctx), "views", fy_map_empty);
	*result = fy_sequence(call->gb);
	fy_foreach_key_value(name, view, views) {
		row = fyai_cmd_arg_bool(call, "full") ?
			      fy_assoc(call->gb, view, "name", fy_value(call->gb, name)) :
			      fy_mapping(call->gb, "name", name, "project",
					 fy_get(view, "project", ""), "state",
					 fy_get(view, "state", "ready"));
		*result = fy_append(call->gb, *result, row);
	}
	if (fyai_cmd_arg_bool(call, "full") && call->format == FYAI_CMD_OUT_MARKDOWN) {
		yaml = fy_emit(call->gb, *result,
			       FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_YAML_1_2 |
				       FYOPEF_STYLE_PRETTY,
			       NULL);
		*result = fy_is_string(yaml) ?
				  fy_stringf(call->gb, "```yaml\n%s\n```\n", fy_castp(&yaml, "")) :
				  fy_invalid;
	}
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
		     bool self, struct shell_command_result *result)
{
	struct fyai_child_spec spec = { .in_fd = -1,
					.out_fd = -1,
					.err_fd = -1,
					.ctty_fd = -1,
					.status_fd = -1,
					.inherit_env = view->agent,
					.view = view };
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
		/* What an agent runtime starts is in the view, and shares it. */
		if (view->agent && setenv("FYAI_VIEW", "1", 1))
			_exit(FYAI_SHELL_EXIT_EXEC);
		rc = fyai_child_exec_prepare(ctx, &spec);
		if (rc)
			_exit(rc);
		if (self) {
			fyai_exec_self((const char *const *)argv);
			fyai_child_status_report(3, FYAI_CHILD_STAGE_EXEC, errno);
		} else if (argv && argv[0]) {
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
		result->exit_code = cancelled	      ? 128 + cancelled :
				    WIFEXITED(status) ? WEXITSTATUS(status) :
							128 + result->signal;
	}
	errno = saved;
	return rc;
}

static int view_terminal_quote(struct response_buffer *line, const char *text)
{
	int rc = response_buffer_append(line, "'");

	for (; !rc && *text; text++)
		rc = *text == '\'' ? response_buffer_append(line, "'\\''") :
				     response_buffer_append_data(line, text, 1);
	return rc ? rc : response_buffer_append(line, "'");
}

static void view_terminal_exited(void *userdata, int exit_code, int signal)
{
	struct fyai_ctx *ctx = userdata;

	if (fyai_branches_refresh(ctx))
		fyai_error(ctx, "view: cannot refresh references after the "
				"terminal exited");
	if (exit_code || signal)
		fyai_error(ctx, "view command failed; its output is in its "
				"terminal tile");
}

static int view_enter_terminal(struct fyai_cmd_call *call)
{
	struct response_buffer line = { 0 };
	struct fyai_shell_session *session;
	const char *argument;
	fy_generic arena;
	char executable[PATH_MAX];
	ssize_t length;
	int rc;

	if (view_writable(call->ctx))
		return -1;
	length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
	if (length <= 0 || length == sizeof(executable) - 1) {
		fyai_error(call->ctx, "view: cannot locate the executable for "
				      "the terminal child");
		return -1;
	}
	executable[length] = '\0';
	arena = fy_emit(call->gb, fy_value(call->gb, call->ctx->cfg->arena_dir),
			FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_JSON | FYOPEF_NO_ENDING_NEWLINE,
			NULL);
	if (!fy_is_string(arena)) {
		fyai_error(call->ctx, "view: cannot encode the terminal child's arena path");
		return -1;
	}
	rc = view_terminal_quote(&line, executable) || response_buffer_append(&line, " --set ") ||
	     view_terminal_quote(&line, fy_sprintfa("arena_dir=%s", fy_castp(&arena, ""))) ||
	     response_buffer_append(&line, " --branch ") ||
	     view_terminal_quote(&line, fyai_ctx_branch(call->ctx)) ||
	     response_buffer_append(&line, " view enter ");
	if (!rc && fyai_cmd_arg_bool(call, "verify"))
		rc = response_buffer_append(&line, "--verify ");
	if (!rc)
		rc = view_terminal_quote(&line, fyai_cmd_arg_str(call, "name")) ||
		     response_buffer_append(&line, " --");
	fy_foreach(argument, fy_get(call->args, "command", fy_seq_empty))
		if (!rc)
			rc = response_buffer_append(&line, " ") ||
			     view_terminal_quote(&line, argument);
	if (rc) {
		fyai_error(call->ctx, "view: cannot allocate the terminal command");
		free(line.data);
		return -1;
	}
	session = fyai_tools_config_program(call->ctx, line.data, "view", fy_seq_empty,
					    view_terminal_exited, call->ctx);
	free(line.data);
	return session ? 0 : -1;
}

#define VIEW_RUN_LISTED 20

struct fyai_view_run {
	struct fyai_fsview spec;
	fy_generic view;
	char *name;
	/* The strings that spec names. */
	char *project, *runtime, *storage, *scratch;
	int lock;
	bool started;
};

static void view_run_release(struct fyai_view_run *run)
{
	if (run->lock >= 0)
		close(run->lock);
	free(run->name);
	free(run->project);
	free(run->runtime);
	free(run->storage);
	free(run->scratch);
}

/*
 * Open the stored view for execution: check its paths and baseline, take the
 * lock of its runtime, validate it after a reboot and mark it running. The
 * strings of spec belong to the view and to scratch; the lock is held until
 * the caller closes it. Return 0, or -1 with the cause reported or, for a
 * failure with errno set, in error and errno.
 */
static int view_open_run(struct fyai_ctx *ctx, const char *name, struct fyai_view_run *run,
			 char *error, size_t error_size)
{
	struct fyai_fsview *spec = &run->spec;
	fy_generic *view = &run->view, project, runtime, storage;
	const char *lockpath;
	char *resolved;
	int rc;

	*view = view_find(ctx, name);
	if (!fy_is_mapping(*view)) {
		fyai_error(ctx, "view '%s' does not exist", name);
		return -1;
	}
	if (fy_is_valid(fy_get(*view, "mount", fy_invalid))) {
		fyai_error(ctx, "view '%s' is mounted; unmount it before entering", name);
		return -1;
	}
	project = fy_get(*view, "project", fy_invalid);
	runtime = fy_get(*view, "runtime", fy_invalid);
	storage = fy_get(*view, "storage", fy_invalid);
	if (!fy_is_string(project) || !fy_is_string(runtime) || !fy_is_string(storage)) {
		fyai_error(ctx, "view '%s': invalid stored paths", name);
		return -1;
	}
	run->project = strdup(fy_castp(&project, ""));
	run->runtime = strdup(fy_castp(&runtime, ""));
	run->storage = strdup(fy_castp(&storage, ""));
	if (!run->project || !run->runtime || !run->storage) {
		errno = ENOMEM;
		return -2;
	}
	spec->project = run->project;
	spec->runtime = run->runtime;
	spec->storage = run->storage;
	spec->arena = ctx->cfg->arena_dir;
	spec->lazy = fy_equal(fy_get(*view, "durability", "durable"), "lazy");
	spec->baseline = fy_get(*view, "baseline", fy_invalid);
	if (!fy_equal(fy_get(spec->baseline, "version", fy_invalid),
		      (long long)FYAI_FSVIEW_SNAPSHOT_VERSION)) {
		fyai_error(ctx, "view '%s': unsupported snapshot version; recreate the view", name);
		return -1;
	}
	spec->metacopy = fy_equal(fy_get(*view, "materialization", fy_invalid), "metacopy");
	run->scratch = view_scratch(ctx);
	if (!run->scratch)
		return -2;
	spec->scratch = run->scratch;
	resolved = realpath(spec->runtime, NULL);
	if (!resolved)
		return -2;
	rc = strcmp(resolved, spec->runtime);
	free(resolved);
	if (rc || strncmp(spec->runtime, spec->storage, strlen(spec->storage)) ||
	    strncmp(spec->runtime + strlen(spec->storage), "/views/view-", 12)) {
		errno = EINVAL;
		return -2;
	}
	lockpath = fy_sprintfa("%s/lock", spec->runtime);
	run->lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (run->lock < 0)
		return -2;
	rc = flock(run->lock, LOCK_EX | LOCK_NB);
	if (rc)
		return -2;
	rc = view_recover(ctx, view, spec, error, error_size);
	if (rc)
		return -2;
	*view = fy_assoc(ctx->gb, *view, "state", "running");
	rc = view_save(ctx, name, *view);
	return rc ? -2 : 0;
}

int fyai_cmd_view_enter(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	fy_generic arguments = fy_get(call->args, "command", fy_seq_empty);
	const char *argument;
	char **argv = NULL;
	size_t argc = fy_len(arguments), index = 0;
	struct fyai_view_run run = { .lock = -1 };
	struct shell_command_result output = { 0 };
	fy_generic snapshot, updated;
	char error[PATH_MAX] = "", startup[FYAI_CHILD_START_TEXT_MAX];
	const char *why;
	int rc = -1, saved;
	bool complete = false;

	if (call->surface == FYAI_CMD_SESSION)
		return view_enter_terminal(call);
	if (view_writable(call->ctx))
		return -1;
	run.spec.verify = fyai_cmd_arg_bool(call, "verify");
	rc = view_open_run(call->ctx, name, &run, error, sizeof(error));
	if (rc == -1) {
		view_run_release(&run);
		return -1;
	}
	run.started = rc == 0;
	if (rc)
		goto out;
	argv = calloc(argc + 1, sizeof(*argv));
	if (!argv)
		goto out;
	fy_foreach(argument, arguments)
		argv[index++] = (char *)argument;
	rc = view_exec(call->ctx, &run.spec, argv, false, &output);
	if (rc)
		goto out;
	why = fyai_child_start_text(&output.start, NULL, run.spec.project, startup, sizeof(startup));
	if (why) {
		rc = fyai_fsview_verify(&run.spec, error, sizeof(error));
		if (!rc || !*error)
			fyai_error(call->ctx, "view '%s': %s", name, why);
		rc = -1;
		goto out;
	}
	snapshot = fyai_fsview_snapshot(call->ctx->gb, &run.spec, error, sizeof(error));
	if (!fy_is_valid(snapshot)) {
		rc = -1;
		goto out;
	}
	updated = fy_assoc(call->ctx->gb, run.view, "result", snapshot);
	updated = fy_assoc(call->ctx->gb, updated, "state", "ready");
	updated = fy_assoc(call->ctx->gb, updated, "synchronized", (bool)!run.spec.lazy);
	rc = view_save(call->ctx, name, updated);
	if (rc)
		goto out;
	call->ctx->cfg->exit_status = output.exit_code;
	*result = fy_invalid;
	complete = true;
out:
	saved = errno;
	if (run.started && !complete) {
		updated = fy_assoc(call->ctx->gb, run.view, "state", "incomplete");
		view_save(call->ctx, name, updated);
	}
	free(argv);
	view_run_release(&run);
	shell_command_result_cleanup(&output);
	if (!complete)
		fyai_error(call->ctx, "view '%s': cannot complete execution%s%s: %s", name,
			   *error ? " at " : "", error, strerror(saved));
	return complete ? 0 : -1;
}

static fy_generic view_mount_record(struct fy_generic_builder *gb, const char *path,
				    const struct fyai_fsview_mount *identity)
{
	return fy_mapping(gb, "path", fy_value(gb, path), "readonly", true, "namespace_device",
			  (long long)identity->namespace_device, "namespace_inode",
			  (long long)identity->namespace_inode, "mount_id",
			  (long long)identity->mount_id, "cover_id", (long long)identity->cover_id,
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
	spec->baseline = fy_get(view, "baseline", fy_invalid);
	if (!fy_equal(fy_get(spec->baseline, "version", fy_invalid),
		      (long long)FYAI_FSVIEW_SNAPSHOT_VERSION)) {
		errno = ENOTSUP;
		return -1;
	}
	spec->lazy = fy_equal(fy_get(view, "durability", "durable"), "lazy");
	spec->metacopy = fy_equal(fy_get(view, "materialization", fy_invalid), "metacopy");
	return 0;
}

int fyai_cmd_view_remove(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	struct fyai_fsview spec = { 0 };
	fy_generic view;
	const char *lockpath;
	int lock = -1, rc, saved;

	if (view_writable(call->ctx))
		return -1;
	view = view_find(call->ctx, name);
	if (!fy_is_mapping(view)) {
		fyai_error(call->ctx, "view '%s' does not exist", name);
		return -1;
	}
	if (fy_is_valid(fy_get(view, "mount", fy_invalid))) {
		fyai_error(call->ctx, "view '%s' is mounted; unmount it first", name);
		return -1;
	}
	rc = view_mount_paths(view, &spec);
	if (rc)
		goto out;
	lockpath = fy_sprintfa("%s/lock", spec.runtime);
	lock = open(lockpath, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (lock < 0 && errno != ENOENT) {
		rc = -1;
		goto out;
	}
	if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB)) {
		rc = -1;
		goto out;
	}
	/* Retained branch history can still name this runtime and its CAS
	 * objects. */
	rc = view_save(call->ctx, name, fy_invalid);
	if (!rc)
		*result = fy_mapping(call->gb, "name", fy_value(call->gb, name), "removed", true);
out:
	saved = errno;
	if (lock >= 0)
		close(lock);
	if (rc)
		fyai_error(call->ctx, "view '%s': cannot remove: %s", name, strerror(saved));
	return rc ? -1 : 0;
}

int fyai_cmd_view_sync(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	struct fyai_fsview spec = { 0 };
	fy_generic view;
	char error[PATH_MAX] = "";
	const char *lockpath;
	int lock = -1, rc, saved;

	if (view_writable(call->ctx))
		return -1;
	view = view_find(call->ctx, name);
	if (!fy_is_mapping(view)) {
		fyai_error(call->ctx, "view '%s' does not exist", name);
		return -1;
	}
	rc = view_mount_paths(view, &spec);
	if (rc)
		goto out;
	lockpath = fy_sprintfa("%s/lock", spec.runtime);
	lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0) {
		rc = -1;
		goto out;
	}
	rc = flock(lock, LOCK_EX | LOCK_NB);
	if (rc)
		goto out;
	rc = view_recover(call->ctx, &view, &spec, error, sizeof(error));
	if (!rc)
		rc = view_sync_backing(spec.runtime);
	if (rc)
		goto out;
	view = fy_assoc(call->ctx->gb, view, "synchronized", true);
	rc = view_save(call->ctx, name, view);
	if (!rc)
		*result = view_summary(call->gb, name, view);
out:
	saved = errno;
	if (lock >= 0)
		close(lock);
	if (rc)
		fyai_error(call->ctx, "view '%s': cannot synchronize%s%s: %s", name,
			   *error ? " at " : "", error, strerror(saved));
	return rc ? -1 : 0;
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
	char *resolved = NULL, error[PATH_MAX] = "";
	const char *lockpath;
	struct dirent *entry;
	DIR *directory = NULL;
	int lock = -1, rc = -1, saved;
	bool complete = false, mounted = false;

	if (view_writable(call->ctx))
		return -1;
	view = view_find(call->ctx, name);
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
	if (view_path_contains(spec.project, resolved) ||
	    view_path_contains(resolved, spec.project) ||
	    view_path_contains(spec.storage, resolved) ||
	    view_path_contains(resolved, spec.storage) ||
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
	lockpath = fy_sprintfa("%s/lock", spec.runtime);
	lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0)
		goto out;
	rc = flock(lock, LOCK_EX | LOCK_NB);
	if (rc)
		goto out;
	rc = view_recover(call->ctx, &view, &spec, error, sizeof(error));
	if (rc)
		goto out;
	rc = fyai_fsview_mount(&spec, resolved, &identity);
	if (rc)
		goto out;
	mounted = true;
	updated = fy_assoc(call->ctx->gb, view, "mount",
			   view_mount_record(call->ctx->gb, resolved, &identity));
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
			fyai_error(call->ctx,
				   "view '%s': mount at '%s' requires mount "
				   "privileges in the current namespace; for "
				   "rootless inspection start unshare -Urnm "
				   "sh: %s",
				   name, target, strerror(saved));
		else
			fyai_error(call->ctx, "view '%s': cannot mount at '%s'%s%s: %s", name,
				   target, *error ? " at " : "", error, strerror(saved));
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
	const char *lockpath;
	int lock = -1, rc = -1, saved;
	bool complete = false;

	if (view_writable(call->ctx))
		return -1;
	view = view_find(call->ctx, name);
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
	lockpath = fy_sprintfa("%s/lock", spec.runtime);
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
			updated = fy_assoc(
				call->ctx->gb, view, "mount",
				view_mount_record(call->ctx->gb, fy_castp(&path, ""), &identity));
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
		fyai_error(call->ctx,
			   "view '%s': cannot unmount the recorded inspection "
			   "mount: %s",
			   name, strerror(saved));
	return complete ? 0 : -1;
}

/* A short account of the change, for the parent of an isolated agent. */
static char *view_run_summary(struct fyai_ctx *ctx, const struct fyai_view_run *run, fy_generic result)
{
	struct fy_generic_builder_cfg builder_cfg = { 0 };
	struct fy_generic_builder *gb = NULL;
	struct fyai_manifest manifests[2] = { 0 };
	struct fyai_ignore_spec ignore;
	struct fyai_ignore_tree *tree;
	fy_generic changes, row;
	FILE *text;
	const char *path;
	char *out = NULL;
	size_t count = 0, listed = 0, size = 0;

	gb = fy_generic_builder_create(&builder_cfg);
	fyai_error_check(ctx, gb, out, "could not make a builder for the change summary");
	if (fyai_fsview_manifest_open(run->spec.storage, run->spec.baseline, &manifests[0]) ||
	    fyai_fsview_manifest_open(run->spec.storage, result, &manifests[1]))
		goto out;
	changes = fyai_manifest_diff(gb, &manifests[0], &manifests[1]);
	if (!fy_is_sequence(changes))
		goto out;
	/* The summary faces the user and the agent that started the run, as the diff does. */
	tree = view_ignore_tree(ctx, gb, run->spec.project, &ignore);
	changes = view_changes_visible(gb, tree, changes);
	fyai_ignore_tree_close(tree);
	fy_foreach(row, changes)
		count += strcmp(fy_get(row, "path", ""), ".") != 0;
	if (!count) {
		out = strdup("no files changed");
		goto out;
	}
	text = open_memstream(&out, &size);
	if (!text)
		goto out;
	fprintf(text, "%zu path%s changed:", count, count == 1 ? "" : "s");
	fy_foreach(row, changes) {
		path = fy_get(row, "path", "");
		if (!strcmp(path, "."))
			continue;
		if (listed == VIEW_RUN_LISTED)
			break;
		fprintf(text, "\n  %s %s", fy_get(row, "status", ""), path);
		listed++;
	}
	if (count > listed)
		fprintf(text, "\n  ... and %zu more", count - listed);
	if (fclose(text)) {
		free(out);
		out = NULL;
	}
out:
	if (gb)
		fy_generic_builder_destroy(gb);
	fyai_manifest_close(&manifests[0]);
	fyai_manifest_close(&manifests[1]);
	return out;
}

int fyai_view_run_begin(struct fyai_ctx *ctx, const char *name, struct fyai_view_run **out)
{
	struct fyai_view_request request = { .ctx = ctx, .name = name, .progress = false };
	struct fyai_view_run *run;
	fy_generic record;
	char error[PATH_MAX] = "", *root = NULL;
	int rc, saved;

	request.replace = fy_is_mapping(view_find(ctx, name));
	if (!request.replace) {
		root = fyai_discover_project_root();
		request.project = root ? root : ".";
	}
	rc = fyai_view_capture(&request, &record);
	free(root);
	if (rc)
		return -1;
	run = calloc(1, sizeof(*run));
	if (!run) {
		fyai_error_check(ctx, run, err, "could not allocate the view run of '%s'", name);
err:
		return -1;
	}
	run->lock = -1;
	run->name = strdup(name);
	rc = view_open_run(ctx, name, run, error, sizeof(error));
	if (rc) {
		saved = errno;
		if (rc == -2)
			fyai_error(ctx, "view '%s': cannot prepare execution%s%s: %s", name,
				   *error ? " at " : "", error, strerror(saved));
		goto fail;
	}
	run->started = true;
	run->spec.agent = true;
	rc = fyai_fsview_agent_prepare(&run->spec);
	if (rc) {
		saved = errno;
		fyai_error(ctx, "view '%s': cannot prepare the agent mounts: %s", name,
			   strerror(saved));
		goto fail;
	}
	*out = run;
	return 0;
fail:
	if (run->started)
		view_save(ctx, name, fy_assoc(ctx->gb, run->view, "state", "incomplete"));
	view_run_release(run);
	free(run);
	return -1;
}

const struct fyai_fsview *fyai_view_run_spec(const struct fyai_view_run *run)
{
	return &run->spec;
}

const char *fyai_view_run_name(const struct fyai_view_run *run)
{
	return run->name;
}

int fyai_view_run_finish(struct fyai_ctx *ctx, struct fyai_view_run *run, char **summary)
{
	fy_generic snapshot, updated;
	char error[PATH_MAX] = "";
	int saved;

	*summary = NULL;
	snapshot = fyai_fsview_snapshot(ctx->gb, &run->spec, error, sizeof(error));
	if (!fy_is_valid(snapshot)) {
		saved = errno;
		view_save(ctx, run->name, fy_assoc(ctx->gb, run->view, "state", "incomplete"));
		fyai_error(ctx, "view '%s': cannot capture the result%s%s: %s", run->name,
			   *error ? " at " : "", error, strerror(saved));
		return -1;
	}
	updated = fy_assoc(ctx->gb, run->view, "result", snapshot);
	updated = fy_assoc(ctx->gb, updated, "state", "ready");
	updated = fy_assoc(ctx->gb, updated, "synchronized", (bool)!run->spec.lazy);
	if (view_save(ctx, run->name, updated))
		return -1;
	*summary = view_run_summary(ctx, run, snapshot);
	return 0;
}

void fyai_view_run_free(struct fyai_view_run *run)
{
	if (!run)
		return;
	view_run_release(run);
	free(run);
}

int fyai_view_session_bootstrap(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic section = fy_get(cfg->config_doc, "view", fy_invalid);
	struct shell_command_result output = { 0 };
	struct fyai_view_run *run = NULL;
	char **argv = NULL, startup[FYAI_CHILD_START_TEXT_MAX];
	char *summary = NULL;
	const char *why;
	size_t argc = 0, i;
	int rc = -1, saved;

	if (!fy_get(section, "isolate_session", false) || getenv("FYAI_VIEW") ||
	    cfg->tool_child || !cfg->argv || fyai_cfg_no_requests(cfg))
		return 0;
	if (ctx->tclient) {
		fyai_error(ctx, "view: a session in a view cannot use credential isolation");
		return -1;
	}
	if (!fyai_exec_self_available()) {
		fyai_error(ctx, "view: this system cannot run the program again");
		return -1;
	}
	while (cfg->argv[argc])
		argc++;
	argv = calloc(argc + 1, sizeof(*argv));
	fyai_error_check(ctx, argv, out, "could not allocate the command line of the view");
	for (i = 0; i < argc; i++)
		argv[i] = cfg->argv[i];
	if (fyai_view_run_begin(ctx, "session", &run))
		goto out;
	/*
	 * The record of the view is on this branch. The session continues on it,
	 * so the view and the conversation stay together, and a later command
	 * finds the view on the branch that it names.
	 */
	if (setenv("FYAI_BRANCH", fyai_ctx_branch(ctx), 1)) {
		fyai_error(ctx, "view: cannot pass the branch to the session: %s", strerror(errno));
		goto out;
	}
	rc = view_exec(ctx, &run->spec, argv, true, &output);
	if (rc) {
		saved = errno;
		view_save(ctx, run->name, fy_assoc(ctx->gb, run->view, "state", "incomplete"));
		fyai_error(ctx, "view 'session': cannot run the session: %s", strerror(saved));
		goto out;
	}
	why = fyai_child_start_text(&output.start, NULL, run->spec.project, startup,
				    sizeof(startup));
	if (why) {
		view_save(ctx, run->name, fy_assoc(ctx->gb, run->view, "state", "incomplete"));
		fyai_error(ctx, "view 'session': %s", why);
		rc = -1;
		goto out;
	}
	rc = fyai_view_run_finish(ctx, run, &summary);
	if (rc)
		goto out;
	cfg->exit_status = output.exit_code;
	fyai_notice(ctx, "The session ran in the view 'session' on the branch %s. Its changes "
			 "are not in the project; %s\nReview them with `fyai -b %s view diff "
			 "session` and apply them with `fyai -b %s view apply session`.\n",
		    fyai_ctx_branch(ctx), summary ? summary : "the change could not be listed",
		    fyai_ctx_branch(ctx), fyai_ctx_branch(ctx));
	rc = 1;
out:
	free(summary);
	free(argv);
	shell_command_result_cleanup(&output);
	fyai_view_run_free(run);
	return rc;
}

/*
 * The project that the arena belongs to: the directory that holds the .fyai
 * directory of the arena. A run whose arena is elsewhere has no project state
 * to record, so it never captures a directory by chance.
 */
static char *project_state_root(struct fyai_ctx *ctx)
{
	const char *arena = ctx->cfg->arena_dir, *marker;
	char *root, *real;

	if (fy_str_empty(arena))
		return NULL;
	real = realpath(arena, NULL);
	if (!real)
		return NULL;
	marker = strstr(real, "/.fyai/");
	if (!marker && strlen(real) > 6 && !strcmp(real + strlen(real) - 6, "/.fyai"))
		marker = real + strlen(real) - 6;
	if (!marker || marker == real) {
		free(real);
		return NULL;
	}
	root = strndup(real, (size_t)(marker - real));
	free(real);
	return root;
}

char *fyai_view_project_root(struct fyai_ctx *ctx)
{
	return project_state_root(ctx);
}

int fyai_view_runtime_remove(const char *runtime)
{
	int fd, rc, saved;

	fd = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = view_runtime_clear(fd, 0);
	saved = errno;
	close(fd);
	errno = saved;
	if (rc)
		return -1;
	return rmdir(runtime);
}

bool fyai_view_isolation_available(struct fyai_ctx *ctx)
{
	char *root;
	bool available;

	if (ctx->cfg->transient || ctx->cfg->root_spec || ctx->gb != ctx->durable_gb ||
	    getenv("FYAI_VIEW") || ctx->tclient)
		return false;
	root = project_state_root(ctx);
	available = root != NULL;
	free(root);
	return available;
}

bool fyai_project_state_enabled(struct fyai_ctx *ctx)
{
	fy_generic section = fy_get(ctx->cfg->config_doc, "view", fy_invalid);

	return fy_get(section, "track_project", false) && !ctx->cfg->transient &&
	       !ctx->cfg->root_spec && !ctx->cfg->tool_child && !getenv("FYAI_VIEW");
}

int fyai_project_state_capture(struct fyai_ctx *ctx, fy_generic *ref)
{
	struct fyai_project_capture_stats statistics = { 0 };
	struct fyai_project_capture_opts opts = { .stats = &statistics,
						  .source_fd = -1,
						  .objects_fd = -1,
						  .baseline_fd = -1,
						  .data_fd = -1,
						  .borrow_git = true,
						  .defer_sync = true };
	struct fyai_ignore_spec ignore;
	struct fyai_manifest manifest = { 0 };
	char manifest_name[FYAI_MANIFEST_NAME_SIZE], error[PATH_MAX] = "";
	char *root, *storage = NULL, *objects = NULL, *views = NULL;
	fy_generic section = fy_get(ctx->cfg->config_doc, "view", fy_invalid);
	bool durable = fy_equal(fy_get(section, "durability", "lazy"), "durable");
	unsigned int attempt;
	int rc = -1, saved, gc_lock = -1;

	*ref = fy_invalid;
	fyai_ignore_spec_load(&ignore, fyai_ignore_record(ctx->gb, ctx->cfg->config_doc));
	opts.ignore = &ignore;
	root = project_state_root(ctx);
	if (!root)
		return 0;
	rc = view_storage(root, &storage, &objects, &views);
	if (rc)
		goto out;
	gc_lock = fyai_view_storage_lock_shared(storage);
	if (gc_lock < 0) {
		rc = -1;
		goto out;
	}
	opts.source_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	opts.objects_fd = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (opts.source_fd < 0 || opts.objects_fd < 0) {
		rc = -1;
		goto out;
	}
	for (attempt = 0; attempt < 3; attempt++) {
		rc = fyai_project_capture_manifest(&opts, &manifest, error, sizeof(error));
		if (!rc || errno != EAGAIN)
			break;
	}
	if (rc)
		goto out;
	rc = fyai_fsview_manifest_publish(storage, &manifest, durable, manifest_name);
	if (rc)
		goto out;
	*ref = fy_assoc(ctx->gb, fyai_fsview_reference(ctx->gb, manifest.root, manifest.object_count,
						       manifest_name),
			"storage", fy_value(ctx->gb, storage));
	if (!fy_is_mapping(*ref)) {
		*ref = fy_invalid;
		errno = ENOMEM;
		rc = -1;
	}
out:
	saved = errno;
	if (opts.source_fd >= 0)
		close(opts.source_fd);
	if (opts.objects_fd >= 0)
		close(opts.objects_fd);
	if (gc_lock >= 0)
		close(gc_lock);
	fyai_manifest_close(&manifest);
	free(root);
	free(storage);
	free(objects);
	free(views);
	if (rc)
		fyai_warning(ctx, "project state: cannot capture the project%s%s: %s",
			     *error ? " at " : "", error, strerror(saved));
	errno = saved;
	return rc;
}

int fyai_project_state_restore(struct fyai_ctx *ctx, const char *spec, bool force,
			       struct fy_generic_builder *gb, fy_generic *report)
{
	struct view_state target = { 0 }, current = { 0 };
	struct fyai_manifest manifests[2] = { { 0 }, { 0 } };
	struct fyai_apply_summary summary;
	fy_generic rows, row, forced = fy_invalid;
	const char *path, *action;
	char *root = NULL, *objects = NULL, list[512] = "";
	size_t shown = 0;
	int project = -1, cas = -1, rc = -1, saved;
	bool have_current;

	*report = fy_invalid;
	if (!fyai_project_state_enabled(ctx))
		return 0;
	root = project_state_root(ctx);
	if (!root)
		return 0;
	if (view_state_find(ctx, spec, &target, true)) {
		fyai_notice(ctx, "reset: no project state is recorded at '%s'; the files are "
				 "not changed", spec);
		free(root);
		return 0;
	}
	have_current = !view_state_find(ctx, "HEAD", &current, true);
	if (!have_current && !force) {
		fyai_error(ctx, "reset: no project state is recorded for the current head, so "
				"edits of the project cannot be told from the state of '%s'; use "
				"--force to take that state over them", spec);
		goto out;
	}
	if (force) {
		/* What the project holds now is the base, and is recorded first. */
		if (fyai_project_state_capture(ctx, &forced) || !fy_is_mapping(forced))
			goto out;
		current.reference = forced;
		snprintf(current.storage, sizeof(current.storage), "%s",
			 fy_get(forced, "storage", ""));
	}
	rc = fyai_fsview_manifest_open(current.storage, current.reference, &manifests[0]);
	if (!rc)
		rc = fyai_fsview_manifest_open(target.storage, target.reference, &manifests[1]);
	if (rc) {
		fyai_error(ctx, "reset: cannot open a recorded project state: %s", strerror(errno));
		goto out;
	}
	project = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (asprintf(&objects, "%s/objects/blake3", target.storage) < 0) {
		objects = NULL;
		rc = -1;
		goto out;
	}
	cas = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (project < 0 || cas < 0) {
		rc = -1;
		goto out;
	}
	/* Nothing is written while a path differs from what was recorded. */
	rc = fyai_view_apply(gb, project, cas, &manifests[0], &manifests[1], NULL, 0, true, &rows,
			     &summary);
	if (rc)
		goto out;
	if (summary.conflicts || summary.skipped) {
		fy_foreach(row, rows) {
			action = fy_get(row, "action", "");
			if (!strcmp(action, "applied") || !strcmp(action, "satisfied") || shown >= 5)
				continue;
			path = fy_get(row, "path", "");
			snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s",
				 shown ? ", " : "", path);
			shown++;
		}
		fyai_error(ctx, "reset: the project changed since its state was recorded (%s); "
				"nothing was changed. Use --force to take the state of '%s' over "
				"the changes", list, spec);
		rc = -1;
		goto out;
	}
	if (fy_is_mapping(forced)) {
		/* An entry for the state that is about to go, so that the reset can be undone. */
		ctx->project_state = forced;
		fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMMAND, NULL);
		rc = fyai_publish_state(ctx);
		if (rc)
			goto out;
	}
	rc = fyai_view_apply(gb, project, cas, &manifests[0], &manifests[1], NULL, 0, false, &rows,
			     &summary);
	if (rc)
		goto out;
	/* The project holds the state now, so the entry of the reset records it. */
	ctx->project_state = target.reference;
	*report = fy_stringf(gb, "%zu paths restored", summary.applied);
out:
	saved = errno;
	if (project >= 0)
		close(project);
	if (cas >= 0)
		close(cas);
	fyai_manifest_close(&manifests[0]);
	fyai_manifest_close(&manifests[1]);
	free(root);
	free(objects);
	errno = saved;
	return rc ? -1 : 0;
}

#else
int fyai_cmd_view_apply(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_diff(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_remove(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}

int fyai_cmd_view_sync(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_error(call->ctx, "view: filesystem views require Linux");
	return -1;
}
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


int fyai_view_run_begin(struct fyai_ctx *ctx, const char *name, struct fyai_view_run **out)
{
	(void)name;
	(void)out;
	fyai_error(ctx, "view: filesystem views require Linux");
	return -1;
}

const struct fyai_fsview *fyai_view_run_spec(const struct fyai_view_run *run)
{
	(void)run;
	return NULL;
}

const char *fyai_view_run_name(const struct fyai_view_run *run)
{
	(void)run;
	return NULL;
}

int fyai_view_run_finish(struct fyai_ctx *ctx, struct fyai_view_run *run, char **summary)
{
	(void)ctx;
	(void)run;
	*summary = NULL;
	return -1;
}

void fyai_view_run_free(struct fyai_view_run *run)
{
	(void)run;
}

int fyai_view_session_bootstrap(struct fyai_ctx *ctx)
{
	(void)ctx;
	return 0;
}

int fyai_view_pull_agent(struct fyai_ctx *ctx, struct fy_generic_builder *gb, const char *child,
			 const char *const *paths, size_t count, enum fyai_view_pull_mode mode,
			 fy_generic *result)
{
	return fyai_view_pull(ctx, gb, child, NULL, paths, count, mode, result);
}

int fyai_view_pull(struct fyai_ctx *ctx, struct fy_generic_builder *gb, const char *name,
		   const char *base, const char *const *paths, size_t count,
		   enum fyai_view_pull_mode mode, fy_generic *result)
{
	(void)gb;
	(void)base;
	(void)name;
	(void)paths;
	(void)count;
	(void)mode;
	(void)result;
	fyai_error(ctx, "view: filesystem views require Linux");
	return -1;
}

fy_generic fyai_view_list(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	(void)ctx;
	(void)gb;
	return fy_map_empty;
}

bool fyai_project_state_enabled(struct fyai_ctx *ctx)
{
	(void)ctx;
	return false;
}

int fyai_project_state_capture(struct fyai_ctx *ctx, fy_generic *ref)
{
	(void)ctx;
	*ref = fy_invalid;
	return 0;
}

bool fyai_view_isolation_available(struct fyai_ctx *ctx)
{
	(void)ctx;
	return false;
}

int fyai_project_state_restore(struct fyai_ctx *ctx, const char *spec, bool force,
			       struct fy_generic_builder *gb, fy_generic *report)
{
	(void)ctx;
	(void)spec;
	(void)force;
	(void)gb;
	*report = fy_invalid;
	return 0;
}

char *fyai_view_project_root(struct fyai_ctx *ctx)
{
	(void)ctx;
	return NULL;
}

int fyai_view_runtime_remove(const char *runtime)
{
	(void)runtime;
	errno = ENOTSUP;
	return -1;
}
#endif
