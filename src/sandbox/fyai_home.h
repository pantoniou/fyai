/* SPDX-License-Identifier: MIT */
#ifndef FYAI_HOME_H
#define FYAI_HOME_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "fyai_sandbox.h"

struct fyai_ctx;

/*
 * The mounts that a sandbox profile gives to the tools of a run, and the private
 * directory that holds what they need.
 */
struct fyai_home {
	char *dir;				/* the private directory */
	bool owned;				/* fyai made it, and removes it */
	struct fyai_sandbox_mount *mount;	/* in the order that they are made */
	size_t mount_n;
	char **strings;				/* what the mounts point at */
	size_t strings_n;
	bool userns;				/* `expose: userns`: the tool may write /proc */
};

/*
 * Make the mounts of the sandbox profile when it has any: `mounts`, the `userns`
 * exposure (a read-only mask of the paths of /proc that change the kernel), and the
 * private home (`home: {private: true}`), and set ctx->scope->home. The private home is
 * a tree that is mounted over the real home, so the tools see the same paths and
 * the real files are hidden. It holds the private versions that `copy` and `env`
 * name, each at its own place: a directory is an overlay of the original, so
 * nothing is copied and the original never changes, or, with `method: copy`, a
 * copy that shares the blocks of the original where the file system can, as the
 * content store does. A file is a copy. Each path that the sandbox grants under
 * the home, the project among them, is mounted into the tree first, so it stays
 * visible. A source that a `deny` entry covers is left out. The mounts are made in
 * the namespace of each tool, which needs a cover. Call it in the process that
 * forks the tools, before the fork; it does nothing in a tool child and when the
 * mounts exist. Returns 0, or -1 after it reported why.
 */
int fyai_home_ensure(struct fyai_ctx *ctx);

/* Remove the private directory that fyai made, and forget the mounts. */
void fyai_home_release(struct fyai_ctx *ctx);

/* Forget the mounts without removing the directory. */
void fyai_home_abandon(struct fyai_ctx *ctx);

#endif
