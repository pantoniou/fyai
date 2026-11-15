/* SPDX-License-Identifier: MIT */
#ifndef FYAI_ORPHAN_H
#define FYAI_ORPHAN_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fy_generic_builder;

/*
 * Processes that a tool leaves running after it ends, for a host that has no
 * namespace to hold them (doc/exchange-scope.md).
 *
 * A tool child is a subreaper: a process that a program of the tool detaches
 * becomes its child. When the call ends, the child names them to the
 * invocation, which keeps a pidfd for each. When the scope ends, the invocation
 * sends SIGTERM to each of them and to the processes below them, and SIGKILL
 * after a grace time.
 */

/* Make this process the parent of the processes that its descendants orphan. */
void fyai_orphan_subreaper(void);

/*
 * The live children of this process, as a sequence of {pid, start}. Give it to
 * the invocation with the result of the call. Empty when this process is in a
 * PID namespace of a scope: the pids would name other processes outside it.
 */
fy_generic fyai_orphan_children(struct fy_generic_builder *gb);

/*
 * A line for the model that names the live children of this process, or
 * fy_invalid when there are none: a call that leaves a process running says
 * so. @until says when the scope ends them.
 */
fy_generic fyai_orphan_note(struct fy_generic_builder *gb, const char *until);

/* Say that a process of a scope namespace has no pid that the invocation can use. */
void fyai_orphan_namespaced(void);

/* Keep @detached, the sequence that fyai_orphan_children() made, for the scope. */
void fyai_orphan_track(struct fyai_ctx *ctx, fy_generic detached);

/* A row {pid, name, kept: "tracked"} for each process that the scope keeps. */
fy_generic fyai_orphan_rows(const struct fyai_ctx *ctx, struct fy_generic_builder *gb);

/* The name that the kernel keeps for @pid, into @name of @size bytes. */
void fyai_orphan_name(pid_t pid, char *name, size_t size);

/*
 * The label that the program carries in FYAI_DAEMON, which a `background` call with a
 * name sets, into @label of @size bytes; empty when there is none.
 */
void fyai_orphan_label(pid_t pid, char *label, size_t size);

/*
 * Stop the daemons that @which names, each by its label or its pid: SIGTERM at once,
 * and SIGKILL after `sandbox_grace_ms` for those that are still there. It looks at what
 * the invocation tracks and at what the holder of the scope holds. Returns how many
 * processes it signalled.
 */
size_t fyai_scope_stop(struct fyai_ctx *ctx, const char *which);

/* How many processes the scope keeps. */
size_t fyai_orphan_count(const struct fyai_ctx *ctx);

/* The scope ends: stop what it keeps, and end what does not stop. */
void fyai_orphan_sweep(struct fyai_ctx *ctx);

/* The invocation ends: end every process now. */
void fyai_orphan_release(struct fyai_ctx *ctx);

/* A forked child does not own the processes of its parent: forget them. */
void fyai_orphan_abandon(struct fyai_ctx *ctx);

#endif
