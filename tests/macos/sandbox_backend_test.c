/* SPDX-License-Identifier: MIT */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "fyai_sandbox.h"

int main(void)
{
	char root[] = "/private/tmp/fyai-seatbelt-XXXXXX";
	char outside[] = "/private/tmp/fyai-outside-XXXXXX";
	char arena[1024], denied[1024], allowed[1024], source[1024], binary[1024];
	char compiler[4096], sdk[4096];
	const char *deny[1];
	struct fyai_sandbox_spec spec = { .strict = true };
	struct fyai_sandbox_path grant = { "/tmp", FYAI_SB_EDIT };
	pid_t child;
	int fd, status;
	FILE *stream;

	assert(fyai_sandbox_abi() == 0);
	assert(fyai_sandbox_available());
	stream = popen("/usr/bin/xcrun --find clang", "r");
	assert(stream);
	assert(fgets(compiler, sizeof(compiler), stream));
	assert(!pclose(stream));
	compiler[strcspn(compiler, "\r\n")] = '\0';
	assert(compiler[0] == '/');
	stream = popen("/usr/bin/xcrun --show-sdk-path", "r");
	assert(stream);
	assert(fgets(sdk, sizeof(sdk), stream));
	assert(!pclose(stream));
	sdk[strcspn(sdk, "\r\n")] = '\0';
	assert(sdk[0] == '/');
	assert(mkdtemp(root));
	fd = mkstemp(outside);
	assert(fd >= 0);
	close(fd);
	snprintf(arena, sizeof(arena), "%s/.fyai", root);
	snprintf(denied, sizeof(denied), "%s/secret", root);
	snprintf(allowed, sizeof(allowed), "%s/allowed", root);
	snprintf(source, sizeof(source), "%s/main.c", root);
	snprintf(binary, sizeof(binary), "%s/program", root);
	stream = fopen(source, "w");
	assert(stream);
	fputs("#include <stdio.h>\nint main(void) { puts(\"native compiler passed\"); return 0; }\n", stream);
	assert(!fclose(stream));
	assert(!mkdir(arena, 0700));
	fd = open(denied, O_CREAT | O_WRONLY, 0600);
	assert(fd >= 0);
	close(fd);
	deny[0] = denied;
	spec.project_root = root;
	spec.deny = deny;
	spec.deny_n = spec.deny_global_n = 1;
	assert(fyai_sandbox_path_visible(&spec, root));
	assert(!fyai_sandbox_path_visible(&spec, denied));
	assert(!fyai_sandbox_path_visible(&spec, arena));
	spec.allow = &grant;
	spec.allow_n = 1;
	assert(fyai_sandbox_apply(&spec) == -1 && errno == ENOTSUP);
	spec.allow = NULL;
	spec.allow_n = 0;
	spec.scratch_root = "/tmp/invalid\nprofile";
	assert(fyai_sandbox_apply(&spec) == -1 && errno == EINVAL);
	spec.scratch_root = NULL;
	spec.restrict_tcp = true;
	assert(fyai_sandbox_apply(&spec) == -1 && errno == ENOTSUP);
	spec.restrict_tcp = false;
	child = fork();
	assert(child >= 0);
	if (!child) {
		assert(!setenv("PATH", "/usr/bin:relative:/ungranted", 1));
		assert(!setenv("TMPDIR", root, 1));
		spec.filter_path = true;
		assert(!fyai_sandbox_apply(&spec));
		assert(!strcmp(getenv("PATH"), "/usr/bin"));
		fd = open(allowed, O_CREAT | O_WRONLY, 0600);
		assert(fd >= 0);
		close(fd);
		assert(open(denied, O_RDONLY) == -1);
		assert(open(denied, O_WRONLY) == -1);
		assert(open(arena, O_RDONLY) == -1);
		assert(open(outside, O_RDONLY) == -1);
		execl(compiler, "clang", "-isysroot", sdk, source, "-o", binary, NULL);
		_exit(127);
	}
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && !WEXITSTATUS(status));
	assert(!unlink(allowed));
	assert(!unlink(denied));
	assert(!unlink(source));
	assert(!unlink(binary));
	assert(!rmdir(arena));
	assert(!rmdir(root));
	assert(!unlink(outside));
	puts("macOS sandbox backend passed");
	return 0;
}
