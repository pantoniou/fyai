/*
 * fyai_secret.c - dependency-free machine-local secret storage
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

#include "fyai_sink.h"
#include "fyai_secret.h"
#include "fyai_secret_store.h"
#include "fyai.h"

void fyai_secret_clear(void *value, size_t len)
{
	volatile unsigned char *p = value;

	while (len--)
		*p++ = 0;
}

void fyai_secret_clear_and_free(char **value, size_t *len)
{
	if (value && *value) {
		fyai_secret_clear(*value, *len);
		free(*value);
	}

	if (value)
		*value = NULL;
	if (len)
		*len = 0;
}

#define SECRET_SERVICE "org.fyai.Secret"

int fyai_secret_kernel_get(const char *name, char **value, size_t *len)
{
	return fyai_secret_store_system()->get(SECRET_SERVICE, name, value, len);
}

int fyai_secret_kernel_set(const char *name, const void *value, size_t len)
{
	return fyai_secret_store_system()->set(SECRET_SERVICE, NULL, name, value, len);
}

int fyai_secret_kernel_delete(const char *name)
{
	return fyai_secret_store_system()->del(SECRET_SERVICE, name);
}

#define FYAI_SECRET_BACKEND_NAME (fyai_secret_store_system()->name)

fy_generic fyai_secret_status_data(struct fyai_ctx *ctx,
				   struct fy_generic_builder *gb,
				   const char *name)
{
	char *probe = NULL;
	size_t probe_len = 0;
	int rc;

	(void)ctx;
	if (!name || !*name) {
		rc = fyai_secret_kernel_get("fyai:__probe__", &probe,
					    &probe_len);
		fyai_secret_clear_and_free(&probe, &probe_len);
		return fy_mapping(gb, "backend",
				  rc == FYAI_SECRET_UNSUPPORTED ?
				  "unavailable" : FYAI_SECRET_BACKEND_NAME);
	}
	rc = fyai_secret_kernel_get(fy_sprintfa("fyai:%s", name), &probe,
				    &probe_len);
	fyai_secret_clear_and_free(&probe, &probe_len);
	return fy_mapping(gb, "name", name, "state",
			  rc == FYAI_SECRET_OK ? "present" :
			  rc == FYAI_SECRET_NOT_FOUND ? "absent" :
			  "unavailable");
}

int fyai_secret_action(struct fyai_ctx *ctx,
		       enum fyai_secret_command command, const char *name,
		       bool stdin_value)
{
	const char *stored;
	char *value = NULL, *probe = NULL;
	size_t len = 0, cap = 0, probe_len = 0;
	FILE *fp = NULL;
	struct termios old, noecho;
	bool restore = false;
	int fd = -1, rc = -1;

	if (command == FYAI_SECRET_STATUS && (!name || !*name)) {
		rc = fyai_secret_kernel_get("fyai:__probe__", &probe, &probe_len);
		fyai_secret_clear_and_free(&probe, &probe_len);
		fyai_result(ctx, "secret backend: %s\n",
		       rc == FYAI_SECRET_UNSUPPORTED ? "unavailable" :
		       FYAI_SECRET_BACKEND_NAME);
		rc = rc == FYAI_SECRET_UNSUPPORTED ? -1 : 0;
		goto out;
	}
	stored = fy_sprintfa("fyai:%s", name);
	if (command == FYAI_SECRET_STATUS) {
		rc = fyai_secret_kernel_get(stored, &probe, &probe_len);
		fyai_secret_clear_and_free(&probe, &probe_len);
		fyai_result(ctx, "secret %s: %s\n", name,
		       rc == FYAI_SECRET_OK ? "present" :
		       rc == FYAI_SECRET_NOT_FOUND ? "absent" : "unavailable");
		rc = rc == FYAI_SECRET_UNSUPPORTED || rc < 0 ? -1 : 0;
		goto out;
	}
	if (command == FYAI_SECRET_DELETE) {
		rc = fyai_secret_kernel_delete(stored);
		if (rc == FYAI_SECRET_OK || rc == FYAI_SECRET_NOT_FOUND) {
			fyai_result(ctx, "secret: removed %s\n", name);
			rc = 0;
			goto out;
		}
		fyai_report(ctx, "secret: %s backend is unavailable\n",
			FYAI_SECRET_BACKEND_NAME);
		rc = -1;
		goto out;
	}
	fp = stdin_value ? stdin : fopen("/dev/tty", "r+");
	if (!fp) {
		fyai_report(ctx, "secret: cannot open /dev/tty; use --stdin\n");
		goto out;
	}
	fd = fileno(fp);
	if (!stdin_value && !tcgetattr(fd, &old)) {
		noecho = old;
		noecho.c_lflag &= ~ECHO;
		if (!tcsetattr(fd, TCSAFLUSH, &noecho))
			restore = true;
		fprintf(fp, "Secret %s: ", name);
		fflush(fp);
	}
	if (getline(&value, &cap, fp) >= 0)
		len = strcspn(value, "\r\n");
	if (restore) {
		(void)tcsetattr(fd, TCSAFLUSH, &old);
		restore = false;
		fprintf(fp, "\n");
	}
	if (!stdin_value) {
		fclose(fp);
		fp = NULL;
	}
	if (!len) {
		fyai_report(ctx, "secret: empty value\n");
		rc = -1;
	} else {
		rc = fyai_secret_kernel_set(stored, value, len);
		if (rc == FYAI_SECRET_OK) {
			fyai_result(ctx, "secret: stored %s\n", name);
			rc = 0;
		} else {
			fyai_report(ctx, "secret: %s backend is unavailable\n",
			FYAI_SECRET_BACKEND_NAME);
			rc = -1;
		}
	}
out:
	if (restore && fp)
		(void)tcsetattr(fd, TCSAFLUSH, &old);
	if (fp && !stdin_value)
		fclose(fp);
	fyai_secret_clear_and_free(&probe, &probe_len);
	fyai_secret_clear_and_free(&value, &cap);
	return rc;
}

int fyai_secret_execute(struct fyai_ctx *ctx)
{
	struct fyai_secret_args *a = &ctx->cfg->cmd.args.secret;

	return fyai_secret_action(ctx, a->command, a->name, a->stdin_value);
}
