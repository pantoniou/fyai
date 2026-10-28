/*
 * fyai_view_native_test.c - standalone view commands of the native macOS
 *                           backend
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_yaml_flags.h"

#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "fyai_test_scratch.h"

FYAI_TEST_ENTRY(view, native_commands, view_native_commands)

#ifdef __APPLE__

#define NATIVE_ENV_MAX	16
#define NATIVE_ARGS_MAX	16

/*
 * The commands run the fyai binary that $FYAI_BIN names, in @root, with an
 * environment of their own. @out and @err hold the output of the last
 * command; the run owns them and each string of @envp.
 */
struct native_run {
	const char *binary;
	const char *root;
	char *envp[NATIVE_ENV_MAX];
	size_t env_n;
	char *out;
	char *err;
	int status;
};

static void native_env_add(struct native_run *run, const char *fmt, ...)
{
	va_list ap;
	int rc;

	FYAI_TCHECK(run->env_n + 1 < NATIVE_ENV_MAX);
	va_start(ap, fmt);
	rc = vasprintf(&run->envp[run->env_n], fmt, ap);
	va_end(ap);
	FYAI_TCHECK(rc >= 0);
	run->env_n++;
}

static void native_env_keep(struct native_run *run, const char *name)
{
	const char *value;

	value = getenv(name);
	if (value)
		native_env_add(run, "%s=%s", name, value);
}

static char *native_read(const char *path)
{
	struct stat st;
	char *text;
	ssize_t got;
	int fd, rc;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	FYAI_TCHECK(fd >= 0);
	rc = fstat(fd, &st);
	FYAI_TCHECK(!rc);
	text = malloc(st.st_size + 1);
	FYAI_TCHECK(text);
	got = read(fd, text, st.st_size);
	FYAI_TCHECK(got == st.st_size);
	text[got] = '\0';
	close(fd);
	return text;
}

static void native_write(const char *path, const char *text)
{
	ssize_t wrote;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	FYAI_TCHECK(fd >= 0);
	wrote = write(fd, text, strlen(text));
	FYAI_TCHECK(wrote == (ssize_t)strlen(text));
	close(fd);
}

/* Run @argv in the root and keep its status and its output. */
static void native_exec(struct native_run *run, char *const argv[])
{
	char out[PATH_MAX], err[PATH_MAX];
	pid_t pid, waited;
	int status, fd;

	snprintf(out, sizeof(out), "%s/stdout", run->root);
	snprintf(err, sizeof(err), "%s/stderr", run->root);
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		if (chdir(run->root))
			_exit(126);
		fd = open("/dev/null", O_RDONLY);
		if (fd < 0 || dup2(fd, 0) < 0)
			_exit(126);
		fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0 || dup2(fd, 1) < 0)
			_exit(126);
		fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0 || dup2(fd, 2) < 0)
			_exit(126);
		execve(argv[0], argv, run->envp);
		_exit(127);
	}
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	FYAI_TCHECK(waited == pid);
	free(run->out);
	free(run->err);
	run->out = native_read(out);
	run->err = native_read(err);
	run->status = WIFEXITED(status) ? WEXITSTATUS(status) :
		      128 + WTERMSIG(status);
}

/* Run fyai with the arguments in @ap up to NULL. */
static void native_fyai_v(struct native_run *run, char **argv, va_list ap)
{
	size_t argc = 0;

	argv[argc++] = (char *)run->binary;
	do {
		FYAI_TCHECK(argc < NATIVE_ARGS_MAX);
		argv[argc] = va_arg(ap, char *);
	} while (argv[argc++]);
	native_exec(run, argv);
}

/* Run fyai; a failure ends the test with the output of the command. */
static void native_ok(struct native_run *run, ...)
{
	char *argv[NATIVE_ARGS_MAX];
	va_list ap;
	size_t i;

	va_start(ap, run);
	native_fyai_v(run, argv, ap);
	va_end(ap);
	if (run->status) {
		fprintf(stderr, "fyai");
		for (i = 1; argv[i]; i++)
			fprintf(stderr, " '%s'", argv[i]);
		fprintf(stderr, ": status %d\n%s%s", run->status, run->out, run->err);
	}
	FYAI_TCHECK(!run->status);
}

/* Run fyai, which must fail. */
static void native_fails(struct native_run *run, ...)
{
	char *argv[NATIVE_ARGS_MAX];
	va_list ap;

	va_start(ap, run);
	native_fyai_v(run, argv, ap);
	va_end(ap);
	FYAI_TCHECK(run->status);
}

/* Copy the last line of @text, without its newline, into @line. */
static void native_last_line(const char *text, char *line, size_t size)
{
	const char *start, *end;

	end = text + strlen(text);
	while (end > text && end[-1] == '\n')
		end--;
	start = end;
	while (start > text && start[-1] != '\n')
		start--;
	FYAI_TCHECK(end > start && (size_t)(end - start) < size);
	memcpy(line, start, end - start);
	line[end - start] = '\0';
}

static int native_remove(const char *path, const struct stat *st, int kind,
			 struct FTW *walk)
{
	(void)st;
	(void)walk;
	return kind == FTW_DP ? rmdir(path) : unlink(path);
}

static void native_check_diff(const char *json)
{
	struct fy_generic_builder_cfg builder_cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic doc, changes, item;
	bool found = false;

	gb = fy_generic_builder_create(&builder_cfg);
	FYAI_TCHECK(gb);
	doc = fy_parse(gb, json, FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	changes = fy_get(doc, "changes", fy_invalid);
	FYAI_TCHECK(fy_is_sequence(changes));
	fy_foreach(item, changes)
		if (fy_equal(fy_get(item, "path", fy_invalid), "file"))
			found = true;
	FYAI_TCHECK(found);
	fy_generic_builder_destroy(gb);
}

int view_native_commands(void)
{
	char root[] = "/private/tmp/fyai-native-commands-XXXXXX";
	char project[PATH_MAX], home[PATH_MAX], scratch[PATH_MAX];
	char path[PATH_MAX], file[PATH_MAX], alias[PATH_MAX];
	char physical[PATH_MAX], again[PATH_MAX];
	char *git[] = { "/usr/bin/git", "init", "-q", project, NULL };
	struct native_run run = { 0 };
	struct stat st;
	char *text, *slash;
	size_t i;
	int rc;

	run.binary = getenv("FYAI_BIN");
	if (!run.binary)
		fprintf(stderr, "FYAI_BIN names no fyai binary\n");
	FYAI_TCHECK(run.binary);

	/* /tmp is a link to /private/tmp, which gives the project an alias. */
	FYAI_TCHECK(mkdtemp(root));
	rc = fyai_test_scratch_add(root);
	FYAI_TCHECK(!rc);
	run.root = root;
	snprintf(project, sizeof(project), "%s/project with spaces", root);
	snprintf(home, sizeof(home), "%s/home", root);
	snprintf(scratch, sizeof(scratch), "%s/scratch with spaces", root);
	rc = mkdir(project, 0700);
	FYAI_TCHECK(!rc);
	rc = mkdir(home, 0700);
	FYAI_TCHECK(!rc);
	rc = mkdir(scratch, 0700);
	FYAI_TCHECK(!rc);
	snprintf(path, sizeof(path), "%s/include", project);
	rc = mkdir(path, 0700);
	FYAI_TCHECK(!rc);
	snprintf(path, sizeof(path), "%s/include/project.h", project);
	native_write(path, "#define MESSAGE \"native view passed\"\n");
	snprintf(path, sizeof(path), "%s/main.c", project);
	native_write(path, "#include <stdio.h>\n#include <project.h>\n"
		     "int main(void) { puts(MESSAGE); return 0; }\n");
	snprintf(file, sizeof(file), "%s/file", project);
	native_write(file, "baseline\n");
	snprintf(path, sizeof(path), "%s/restricted", project);
	native_write(path, "hidden\n");

	native_env_add(&run, "PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin");
	native_env_keep(&run, "USER");
	native_env_keep(&run, "LOGNAME");
	native_env_keep(&run, "LANG");
	native_env_keep(&run, "ASAN_OPTIONS");
	native_env_add(&run, "HOME=%s", home);
	native_env_add(&run, "TMPDIR=%s", scratch);
	native_env_add(&run, "XDG_CONFIG_HOME=%s/.config", home);
	native_env_add(&run, "XDG_CACHE_HOME=%s/.cache", home);
	native_env_add(&run, "XDG_DATA_HOME=%s/.local/share", home);
	native_env_add(&run, "CPATH=%s/include:/usr/include", project);
	native_env_add(&run, "CMAKE_PREFIX_PATH=%s/install:/opt/homebrew", project);

	native_exec(&run, git);
	FYAI_TCHECK(!run.status);

	native_ok(&run, "init", NULL);
	native_ok(&run, "config", "set", "sandbox_profiles/dev/deny",
		    "[restricted]", NULL);
	native_ok(&run, "view", "create", "native", project, "--quiet", NULL);
	native_ok(&run, "view", "list", NULL);
	native_ok(&run, "view", "show", "native", NULL);

	/* The program sees the clone, and the project path maps to it. */
	native_ok(&run, "view", "enter", "native", "--", "/bin/sh", "-c",
		    "test \"$CPATH\" = \"$PWD/include:/usr/include\" && "
		    "test \"$CMAKE_PREFIX_PATH\" = \"$PWD/install:/opt/homebrew\" && "
		    "git status --porcelain >/dev/null && "
		    "cc main.c -o program && ./program && printf changed > file && pwd",
		    NULL);
	FYAI_TCHECK(strstr(run.out, "native view passed"));
	native_last_line(run.out, physical, sizeof(physical));
	FYAI_TCHECK(strcmp(physical, project));
	text = native_read(file);
	FYAI_TCHECK(!strcmp(text, "baseline\n"));
	free(text);
	snprintf(path, sizeof(path), "%s/program", project);
	rc = stat(path, &st);
	FYAI_TCHECK(rc && errno == ENOENT);

	/* A second entry finds the same clone with its changes. */
	native_ok(&run, "view", "enter", "native", "--", "/bin/sh", "-c",
		    "test \"$(cat file)\" = changed && ./program && pwd", NULL);
	native_last_line(run.out, again, sizeof(again));
	FYAI_TCHECK(!strcmp(again, physical));

	/* The host project is denied by its path, by its alias, and by the policy. */
	native_fails(&run, "view", "enter", "native", "--", "/bin/cat", file, NULL);
	FYAI_TCHECK(!strncmp(file, "/private/tmp/", 13));
	snprintf(alias, sizeof(alias), "/tmp/%s", file + 13);
	native_fails(&run, "view", "enter", "native", "--", "/bin/cat", alias, NULL);
	native_fails(&run, "view", "enter", "native", "--", "/bin/cat", "restricted",
		    NULL);

	/* No profile does not lift the confinement of the view. */
	native_ok(&run, "config", "set", "sandbox_profile", "none", NULL);
	native_fails(&run, "view", "enter", "native", "--", "/bin/cat", file, NULL);

	native_ok(&run, "view", "sync", "native", NULL);
	native_ok(&run, "view", "diff", "native", "--stat", "--output", "json", NULL);
	native_check_diff(run.out);

	native_fails(&run, "view", "enter", "--profile", "dev", "native", "--",
		    "/usr/bin/true", NULL);
	FYAI_TCHECK(strstr(run.err, "standalone"));

	/* A lost clone ends entry, and its recorded result still applies. */
	slash = strrchr(physical, '/');
	FYAI_TCHECK(slash);
	*slash = '\0';
	rc = nftw(physical, native_remove, 16, FTW_DEPTH | FTW_PHYS);
	FYAI_TCHECK(!rc);
	native_fails(&run, "view", "enter", "native", "--", "/usr/bin/true", NULL);
	native_ok(&run, "view", "apply", "native", NULL);
	text = native_read(file);
	FYAI_TCHECK(!strcmp(text, "changed"));
	free(text);
	native_ok(&run, "view", "remove", "native", NULL);

	free(run.out);
	free(run.err);
	for (i = 0; i < run.env_n; i++)
		free(run.envp[i]);
	return 0;
}

#else
/* The cloned project directory is the view of the macOS backend. */
int view_native_commands(void)
{
	return 0;
}
#endif
