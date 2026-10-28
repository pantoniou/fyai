/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/xattr.h>
#endif
#include <unistd.h>
#include "fyai_project_capture.h"
#include "fyai_fsview.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

static int native_capture_remove(const char *path, const struct stat *st, int kind,
				 struct FTW *walk)
{
	(void)st;
	(void)walk;
	return kind == FTW_DP ? rmdir(path) : unlink(path);
}

int project_native_capture(void)
{
	char path[] = "/tmp/fyai-native-capture-XXXXXX", error[4096];
	struct fyai_project_capture_opts opts = {
		.baseline_fd = -1, .upper_fd = -1, .workers = 2, .verify = true,
	};
	struct fyai_manifest first = { 0 }, same = { 0 }, changed = { 0 }, materialized = { 0 };
	struct fyai_project_capture_stats stats;
	struct fy_generic_builder_cfg builder_cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *builder;
	struct fyai_manifest opened = { 0 };
	fy_generic reference;
	char manifest_name[FYAI_MANIFEST_NAME_SIZE];
	char bytes[7];
	int root, fd, rc;
	int captured;
#ifdef __APPLE__
	struct statfs filesystem;
#endif

	FYAI_TCHECK(mkdtemp(path));
	root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(root >= 0);
	rc = mkdirat(root, "source", 0700);
	FYAI_TCHECK(!rc);
	rc = mkdirat(root, "objects", 0700);
	FYAI_TCHECK(!rc);
	opts.source_fd = openat(root, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	opts.objects_fd = openat(root, "objects", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(opts.source_fd >= 0 && opts.objects_fd >= 0);
	fd = openat(opts.source_fd, "file", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	FYAI_TCHECK(fd >= 0);
	rc = write(fd, "initial", 7);
	FYAI_TCHECK(rc == 7);
	rc = symlinkat("/outside/project", opts.source_fd, "link");
	FYAI_TCHECK(!rc);
	rc = fyai_project_capture_manifest(&opts, &first, error, sizeof(error));
	if (rc)
		fprintf(stderr, "capture %s: %s\n", error, strerror(errno));
	FYAI_TCHECK(!rc);
	builder = fy_generic_builder_create(&builder_cfg);
	FYAI_TCHECK(builder);
	rc = fyai_fsview_manifest_publish(path, &first, true, manifest_name);
	FYAI_TCHECK(!rc);
	reference = fyai_fsview_reference(builder, first.root, first.object_count, manifest_name);
	rc = fyai_fsview_manifest_open(path, reference, &opened);
	FYAI_TCHECK(!rc && fyai_manifest_equal(&first, &opened));
	fyai_manifest_close(&opened);
	reference = fy_assoc(builder, reference, "root", "wrong");
	rc = fyai_fsview_manifest_open(path, reference, &opened);
	FYAI_TCHECK(rc == -1 && errno == EBADMSG);
	fy_generic_builder_destroy(builder);
	rc = fyai_project_capture_manifest(&opts, &same, error, sizeof(error));
	FYAI_TCHECK(!rc && fyai_manifest_equal(&first, &same));
	rc = pwrite(fd, "changed", 7, 0);
	FYAI_TCHECK(rc == 7);
	rc = fyai_project_capture_manifest(&opts, &changed, error, sizeof(error));
	FYAI_TCHECK(!rc && !fyai_manifest_equal(&first, &changed));
	rc = mkdirat(root, "baseline", 0700);
	FYAI_TCHECK(!rc);
	opts.baseline_fd = openat(root, "baseline", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(opts.baseline_fd >= 0);
	opts.stats = &stats;
	rc = fyai_project_capture_manifest(&opts, &materialized, error, sizeof(error));
	if (rc)
		fprintf(stderr, "materialize %s: %s\n", error, strerror(errno));
	FYAI_TCHECK(!rc && fyai_manifest_equal(&changed, &materialized));
#ifdef __APPLE__
	rc = fstatfs(root, &filesystem);
	FYAI_TCHECK(!rc);
	if (!strcmp(filesystem.f_fstypename, "apfs"))
		FYAI_TCHECK(stats.reflinks == 1);
#endif
	rc = pwrite(fd, "another", 7, 0);
	FYAI_TCHECK(rc == 7);
	captured = openat(opts.baseline_fd, "file", O_RDONLY | O_CLOEXEC);
	FYAI_TCHECK(captured >= 0);
	rc = read(captured, bytes, sizeof(bytes));
	FYAI_TCHECK(rc == 7 && !memcmp(bytes, "changed", 7));
	close(captured);
	close(opts.baseline_fd);
	fyai_manifest_close(&materialized);
#ifdef __APPLE__
	opts.baseline_fd = -1;
	rc = fsetxattr(fd, "user.fyai.test", "x", 1, 0, 0);
	FYAI_TCHECK(!rc);
	rc = fyai_project_capture_manifest(&opts, &materialized, error, sizeof(error));
	FYAI_TCHECK(rc == -1 && errno == ENOTSUP);
	rc = fremovexattr(fd, "user.fyai.test", 0);
	FYAI_TCHECK(!rc);
	opts.incremental = true;
	rc = fyai_project_capture_manifest(&opts, &materialized, error, sizeof(error));
	FYAI_TCHECK(rc == -1 && errno == ENOTSUP);
#endif
	fyai_manifest_close(&changed);
	fyai_manifest_close(&same);
	fyai_manifest_close(&first);
	close(fd);
	close(opts.source_fd);
	close(opts.objects_fd);
	close(root);
	rc = nftw(path, native_capture_remove, 16, FTW_DEPTH | FTW_PHYS);
	FYAI_TCHECK(!rc);
	return 0;
}

FYAI_TEST_ENTRY(project, native_capture, project_native_capture)
