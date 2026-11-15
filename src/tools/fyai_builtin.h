/*
 * fyai_builtin.h - checks of the data that the binary embeds
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_BUILTIN_H
#define FYAI_BUILTIN_H

struct fyai_ctx;

/*
 * Check every embedded document against its schema, and against the code
 * where a schema is not enough. A build fixes this data, so the build runs
 * the check one time (`fyai __check-builtins`) and a start does not. Report
 * the first problem and return -1, or return 0.
 */
int fyai_builtin_check(struct fyai_ctx *ctx);

#endif
