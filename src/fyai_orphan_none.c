/*
 * fyai_orphan_none.c - processes that a tool leaves running (no support)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "fyai_host_backend.h"
#include "fyai.h"
#include "fyai_cover.h"
#include "fyai_event.h"
#include "fyai_orphan.h"

#define FYAI_MODULE FYAIEM_TOOLS

#ifndef FYAI_HOST_BACKEND_LINUX

void fyai_orphan_subreaper(void)
{
}

void fyai_orphan_namespaced(void)
{
}

fy_generic fyai_orphan_children(struct fy_generic_builder *gb)
{
	return fy_sequence(gb);
}

fy_generic fyai_orphan_note(struct fy_generic_builder *gb, const char *until)
{
	(void)gb;
	(void)until;
	return fy_invalid;
}

void fyai_orphan_track(struct fyai_ctx *ctx, fy_generic detached)
{
	(void)ctx;
	(void)detached;
}

fy_generic fyai_orphan_rows(const struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	(void)ctx;
	return fy_sequence(gb);
}

void fyai_orphan_name(pid_t pid, char *name, size_t size)
{
	(void)pid;
	if (size)
		name[0] = '\0';
}

void fyai_orphan_label(pid_t pid, char *label, size_t size)
{
	(void)pid;
	if (size)
		label[0] = '\0';
}

size_t fyai_scope_stop(struct fyai_ctx *ctx, const char *which)
{
	(void)ctx;
	(void)which;
	return 0;
}

size_t fyai_orphan_count(const struct fyai_ctx *ctx)
{
	(void)ctx;
	return 0;
}

void fyai_orphan_sweep(struct fyai_ctx *ctx)
{
	(void)ctx;
}

void fyai_orphan_release(struct fyai_ctx *ctx)
{
	(void)ctx;
}

void fyai_orphan_abandon(struct fyai_ctx *ctx)
{
	(void)ctx;
}

#endif /* !FYAI_HOST_BACKEND_LINUX */
