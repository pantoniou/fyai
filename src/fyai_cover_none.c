/*
 * fyai_cover_none.c - hide paths from a tool in a mount namespace (no support)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The holder is a bubblewrap process that makes user, mount and PID namespaces,
 * covers the paths in the mount namespace, and runs `cat` on a pipe until the
 * process that started it ends. A tool joins the namespaces with setns(): a
 * process of the user that made a user namespace holds every capability in it,
 * so nothing here needs privilege. The holder is not a daemon: it belongs to one
 * invocation and ends with it, and so does every tool in its PID namespace.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "fyai_host_backend.h"
#include "fyai_cover.h"
#include "fyai_fsview.h"

#ifndef FYAI_HOST_BACKEND_LINUX

void fyai_cover_init(struct fyai_cover *cover)
{
	memset(cover, 0, sizeof(*cover));
	cover->hold_fd = -1;
	cover->userns = -1;
}

void fyai_cover_abandon(struct fyai_cover *cover)
{
	(void)cover;
}

void fyai_cover_release(struct fyai_cover *cover)
{
	(void)cover;
}

void fyai_cover_drain(struct fyai_cover *cover)
{
	(void)cover;
}

enum fyai_cover_method fyai_cover_select(struct fyai_cover *cover, const char *mode,
				  const char *const *paths, size_t n, pid_t *holder,
				  enum fyai_cover_start start)
{
	(void)cover;
	(void)mode;
	(void)paths;
	(void)n;
	(void)start;
	*holder = 0;
	return FYAI_COVER_NONE;
}

size_t fyai_cover_members(const struct fyai_cover *cover, pid_t *pids, size_t max)
{
	(void)cover;
	(void)pids;
	(void)max;
	return 0;
}

bool fyai_cover_covers(const struct fyai_cover *cover, const char *path)
{
	(void)cover;
	(void)path;
	return false;
}

int fyai_cover_join(pid_t holder, const struct fyai_sandbox_mount *mount,
		    size_t mount_n)
{
	(void)holder;
	(void)mount;
	(void)mount_n;
	errno = ENOTSUP;
	return -1;
}

void fyai_cover_probe(struct fyai_cover *cover, bool *userns, bool *bwrap)
{
	(void)cover;
	*userns = false;
	*bwrap = false;
}

#endif /* !FYAI_HOST_BACKEND_LINUX */
