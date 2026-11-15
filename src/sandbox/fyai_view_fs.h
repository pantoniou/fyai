/* SPDX-License-Identifier: MIT */
#ifndef FYAI_VIEW_FS_H
#define FYAI_VIEW_FS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * File operations that the commands `view cp` and `view rm` run on a directory
 * tree: the root of a project, or the project as a view shows it. Every path is a
 * relative path of plain names that is walked from the root descriptor with no
 * symlink followed, so a path cannot leave the root. The names .git and .fyai are
 * never read, written or removed.
 *
 * A failure returns -1 with errno set and, when an error buffer is given, the path
 * at which the operation stopped.
 */

/*
 * Normalize a path that a user typed into out: leading "./" and repeated and
 * trailing slashes are removed. Return false when the result is not a path of the
 * kind above, or is empty: ".", "..", an absolute path and a reserved name are not.
 */
bool fyai_view_fs_path(const char *in, char *out, size_t size);

/* A function that takes the bytes of a stream. Return 0, or -1 with errno set. */
typedef int (*fyai_view_fs_emit_fn)(void *arg, const void *data, size_t length);

/*
 * Write the paths and what they hold to a stream: directories, regular files and
 * symbolic links, with their permission bits and modification times. Another type
 * is an error (ENOTSUP). The stream names path i as names[i], so a reader puts it
 * there; a NULL names keeps the paths. With a NULL emit nothing is written and the
 * same walk checks that every path exists and has a type that the stream holds, so
 * a caller can fail before it changes anything.
 */
int fyai_view_fs_pack(int root, const char *const *paths, const char *const *names, size_t count,
		      fyai_view_fs_emit_fn emit, void *arg, char *error, size_t error_size);

/* A function that fills the buffer from a stream; the count read, 0 at its end, or -1. */
typedef long (*fyai_view_fs_fill_fn)(void *arg, void *data, size_t length);

/*
 * Write what a stream holds below root. A file is written beside its place and
 * renamed to it, so a reader never sees half a file. A path that exists as another
 * type is replaced. A parent directory is made when it is absent, and a directory
 * that exists keeps what it holds that the stream does not name.
 */
int fyai_view_fs_unpack(int root, fyai_view_fs_fill_fn fill, void *arg, char *error,
			size_t error_size);

/* Remove the paths and what they hold. A path that is absent is an error, unless force. */
int fyai_view_fs_remove(int root, const char *const *paths, size_t count, bool force, char *error,
			size_t error_size);

#endif
