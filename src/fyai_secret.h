/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SECRET_H
#define FYAI_SECRET_H

#include <stddef.h>
#include <stdbool.h>

struct fyai_ctx;

enum fyai_secret_command {
	FYAI_SECRET_STATUS,
	FYAI_SECRET_SET,
	FYAI_SECRET_DELETE,
};

struct fyai_secret_args {
	enum fyai_secret_command command;
	const char *name;
	bool stdin_value;
};

enum fyai_secret_result {
	FYAI_SECRET_OK = 0,
	FYAI_SECRET_NOT_FOUND = 1,
	FYAI_SECRET_UNSUPPORTED = 2,
};

int fyai_secret_kernel_get(const char *name, char **value, size_t *len);
int fyai_secret_kernel_set(const char *name, const void *value, size_t len);
int fyai_secret_kernel_delete(const char *name);
void fyai_secret_clear(void *value, size_t len);
int fyai_secret_execute(struct fyai_ctx *ctx);
int fyai_secret_action(enum fyai_secret_command command, const char *name,
		       bool stdin_value);

#endif
