/* SPDX-License-Identifier: MIT */
/* Native macOS tool confinement: the Seatbelt backend (fyai_sandbox_backend.h). */
#include "fyai_sandbox_backend.h"

#ifdef FYAI_SANDBOX_BACKEND_MACOS
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sandbox.h>


static const char *const system_paths[] = {
	"/System", "/usr", "/bin", "/sbin", "/Library", "/Applications",
	"/opt", "/private/etc", "/dev", NULL,
};

/* Profile strings admit no control bytes or executable profile syntax. */
static int quoted(FILE *out, const char *path)
{
	const unsigned char *p;

	if (!path || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	fputc('"', out);
	for (p = (const unsigned char *)path; *p; p++) {
		if (*p < 32 || *p == 127) {
			errno = EINVAL;
			return -1;
		}
		if (*p == '"' || *p == '\\')
			fputc('\\', out);
		fputc(*p, out);
	}
	fputc('"', out);
	return 0;
}

/*
 * Seatbelt matches the resolved path, so a rule names it: /var is /private/var
 * on macOS. A path that does not exist is named as it is given.
 */
static int path_rule(FILE *out, const char *action, const char *rights, const char *path)
{
	char *resolved;
	int rc;

	resolved = realpath(path, NULL);
	fprintf(out, "(%s %s (subpath ", action, rights);
	rc = quoted(out, resolved ? resolved : path);
	free(resolved);
	if (rc)
		return -1;
	fputs("))\n", out);
	return ferror(out) ? -1 : 0;
}

static int apply_profile(const char *profile)
{
	char *error = NULL;
	int rc;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	rc = sandbox_init(profile, 0, &error);
	if (error)
		sandbox_free_error(error);
#pragma clang diagnostic pop
	if (rc)
		errno = EPERM;
	return rc;
}

static int mac_abi(void)
{
	return 0;
}

static bool mac_available(void)
{
	pid_t child, waited;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child)
		_exit(apply_profile("(version 1)(allow default)") ? 1 : 0);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	return waited == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

static bool mac_restrictable(int abi)
{
	(void)abi;
	return false;
}

static bool mac_udp_deniable(void)
{
	return false;
}

static bool mac_path_visible(const struct fyai_sandbox_spec *spec, const char *dir)
{
	const char *const *p;
	const char *relative;
	size_t i;
	bool visible = spec->read_all;

	if (!dir || dir[0] != '/' || strstr(dir, "/../") ||
	    (strlen(dir) >= 3 && !strcmp(dir + strlen(dir) - 3, "/..")))
		return false;
	for (i = 0; i < spec->deny_n; i++)
		if (fyai_sandbox_path_within(dir, spec->deny[i]))
			return false;
	for (p = system_paths; *p; p++)
		visible |= fyai_sandbox_path_within(dir, *p);
	if (spec->project_root)
		visible |= fyai_sandbox_path_within(dir, spec->project_root);
	if (spec->project_root && fyai_sandbox_path_within(dir, spec->project_root)) {
		relative = dir + strlen(spec->project_root);
		if (!strncmp(relative, "/.fyai", 6) && (!relative[6] || relative[6] == '/'))
			return false;
	}
	if (spec->scratch_root)
		visible |= fyai_sandbox_path_within(dir, spec->scratch_root);
	for (i = 0; i < spec->allow_n; i++)
		visible |= fyai_sandbox_path_within(dir, spec->allow[i].path);
	return visible;
}

static int mac_apply(const struct fyai_sandbox_spec *spec)
{
	const char *const *p;
	const char *scratch = getenv("TMPDIR");
	char temporary[4096];
	char *profile = NULL, *arena = NULL;
	size_t length = 0, i;
	FILE *out;
	int rc = -1, saved;
	size_t temporary_size;

	/* Unsupported policy is rejected before any restriction is installed. */
	if (spec->mount_n || spec->hide_n || spec->cover_holder ||
	    spec->restrict_tcp || spec->restrict_tcp_bind || spec->restrict_udp || spec->deny_udp) {
		errno = ENOTSUP;
		return -1;
	}
	for (i = 0; i < spec->allow_n; i++)
		if (spec->allow[i].mode != FYAI_SB_RW && spec->allow[i].mode != FYAI_SB_RO) {
			errno = ENOTSUP;
			return -1;
		}
	if (!scratch && !spec->scratch_root) {
		temporary_size = confstr(_CS_DARWIN_USER_TEMP_DIR, temporary, sizeof(temporary));
		if (!temporary_size || temporary_size > sizeof(temporary)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		scratch = temporary;
	}
	out = open_memstream(&profile, &length);
	if (!out)
		return -1;
	fputs("(version 1)\n(deny default)\n(allow process*)\n(allow file-read-metadata)\n"
	      "(allow file-read* (literal \"/\"))\n"
	      "(allow sysctl-read)\n(allow mach-lookup)\n(allow network*)\n", out);
	if (spec->read_all)
		fputs("(allow file-read*)\n", out);
	if (spec->list_root)
		fputs("(allow file-read-metadata)\n", out);
	for (p = system_paths; *p; p++)
		if (path_rule(out, "allow", "file-read*", *p))
			goto finish;
	/* The ioctl on a terminal lets a shell with job control own it. */
	if (path_rule(out, "allow", "file-read* file-write* file-ioctl", "/dev"))
		goto finish;
	if (scratch && path_rule(out, "allow", "file-read* file-write*", scratch))
		goto finish;
	if (spec->scratch_root && path_rule(out, "allow", "file-read* file-write*", spec->scratch_root))
		goto finish;
	if (spec->project_root) {
		if (path_rule(out, "allow", "file-read* file-write*", spec->project_root))
			goto finish;
		if (asprintf(&arena, "%s/.fyai", spec->project_root) < 0)
			goto finish;
	}
	for (i = 0; i < spec->allow_n; i++)
		if (path_rule(out, "allow", spec->allow[i].mode == FYAI_SB_RO ?
			      "file-read*" : "file-read* file-write*", spec->allow[i].path))
			goto finish;
	for (i = 0; i < spec->deny_n; i++)
		if (path_rule(out, "deny", "file-read* file-write*", spec->deny[i]))
			goto finish;
	if (arena && path_rule(out, "deny", "file-read* file-write*", arena))
		goto finish;
	rc = ferror(out) ? -1 : 0;
finish:
	saved = errno;
	if (fclose(out)) {
		rc = -1;
		saved = errno;
	}
	if (!rc && spec->filter_path)
		fyai_sandbox_filter_path(spec);
	if (!rc)
		rc = apply_profile(profile);
	else
		errno = saved;
	free(arena);
	free(profile);
	return rc;
}
const struct fyai_sandbox_backend fyai_sandbox_backend_macos = {
	.name = "macos",
	.abi = mac_abi,
	.available = mac_available,
	.net_restrictable = mac_restrictable,
	.udp_restrictable = mac_restrictable,
	.udp_deniable = mac_udp_deniable,
	.path_visible = mac_path_visible,
	.apply = mac_apply,
};

#endif /* FYAI_SANDBOX_BACKEND_MACOS */
