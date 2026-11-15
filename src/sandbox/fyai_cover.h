/* SPDX-License-Identifier: MIT */
#ifndef FYAI_COVER_H
#define FYAI_COVER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct fyai_sandbox_mount;

/*
 * How a tool hides the paths that the sandbox denies and the project arena.
 *
 * Landlock cannot deny a path under a directory that it grants whole, and the
 * carve-out that stands in for the denial leaves each directory above the
 * denied path only the right to list. A tool then cannot create an entry in the
 * project root. A cover hides the path in a mount namespace instead, and
 * Landlock gets no denial for it.
 *
 *  USERNS  the tool makes user, mount and PID namespaces itself.
 *  BWRAP   a holder owns the namespaces, and the tool joins them with setns().
 *          The holder is the scope of an exchange (doc/exchange-scope.md): a
 *          process that this process or an ancestor started, either an init
 *          that this program makes where it may make namespaces, or a
 *          bubblewrap process. Ubuntu lets bubblewrap make a user namespace
 *          when it does not let an unconfined program do so.
 *  NONE    no cover: Landlock denies the path, with the cost above.
 */
enum fyai_cover_method {
	FYAI_COVER_NONE,
	FYAI_COVER_USERNS,
	FYAI_COVER_BWRAP,
};

/*
 * Most holders that wait to be reaped. A holder ends after its grace time, so a
 * holder retires at most once for each exchange; a full list makes the next one
 * wait for the oldest.
 */
#define FYAI_COVER_RETIRED_MAX 8

/*
 * What an invocation knows about covers, owned by its context. A process that
 * started a holder owns it: it keeps the write end of the pipe that the holder
 * reads, and the holder ends when that process does. A process that was started
 * by one that owns a holder, by exec or by fork, only uses it.
 */
struct fyai_cover {
	pid_t holder;		/* the holder in use, 0 when none */
	bool owner;		/* this process started it and keeps @hold_fd */
	int hold_fd;		/* the write end of the pipe of the holder */
	char *paths;		/* the paths that it covers, one for each line */
	int userns;		/* a user namespace of the process: -1 unknown, 0 no, 1 yes */
	bool bwrap_failed;	/* a holder could not be made or joined: do not try again */
	bool userns_failed;	/* an init holder could not be made or joined */
	pid_t waiter;		/* the child that runs the holder: reaped after it ends */
	pid_t retired[FYAI_COVER_RETIRED_MAX];	/* holders that ended and are not reaped yet */
};

/* Who may start the holder of a scope when a tool chooses its cover. */
enum fyai_cover_start {
	FYAI_COVER_JOIN_ONLY,	/* a tool child: use a holder that exists */
	FYAI_COVER_START_SCOPE,	/* the invocation: start a holder for the exchange */
	FYAI_COVER_START_CALL,	/* the tool: its own holder, for one call */
};

/*
 * Set up @cover for a process that has just started. A holder that an ancestor
 * started is named by the environment, which a tool clears later: read it now.
 */
void fyai_cover_init(struct fyai_cover *cover);

/* A forked child does not own the holder of its parent: close its copy of the pipe. */
void fyai_cover_abandon(struct fyai_cover *cover);

/* End the holder that this process started, and forget the one that it uses. */
void fyai_cover_release(struct fyai_cover *cover);

/*
 * Wait until the holders that fyai_cover_release() ended are gone, and with them
 * the mounts of their namespaces. A holder gives its processes a grace time, so
 * this can take that long. A caller removes a mount point only after it.
 */
void fyai_cover_drain(struct fyai_cover *cover);

/*
 * Choose the cover for the @n existing @paths. @mode is `sandbox_cover`: auto
 * (USERNS, else BWRAP, else NONE), userns, bwrap or none. A holder that an
 * ancestor started is used before a new one is started. With BWRAP, *@holder is
 * the process id whose namespaces to join with fyai_cover_join().
 */
enum fyai_cover_method fyai_cover_select(struct fyai_cover *cover, const char *mode,
				  const char *const *paths, size_t n, pid_t *holder,
				  enum fyai_cover_start start);

/*
 * The processes that the holder of this process holds, by pid in the namespace
 * of this process: the ones with the PID namespace of the holder. Returns how
 * many it wrote to @pids, at most @max. The holder itself is not one.
 */
size_t fyai_cover_members(const struct fyai_cover *cover, pid_t *pids, size_t max);

/* True when the holder of this process covers @path. */
bool fyai_cover_covers(const struct fyai_cover *cover, const char *path);

/*
 * Join the namespaces of @holder, make the @mount_n mounts that it lacks,
 * and drop the capabilities of this process. The
 * call forks: the caller never returns, and ends with the status of the program
 * that returns. Call it from a process that has one thread, before it confines
 * itself. Returns 0 in the program, or -1 with errno set before the fork.
 */
int fyai_cover_join(pid_t holder, const struct fyai_sandbox_mount *mount,
		    size_t mount_n);

/*
 * Say whether a cover is possible on this host now, without keeping a holder:
 * @userns for a user namespace of the process, @bwrap for a holder. @bwrap is
 * asked only when @userns is false, and is false otherwise.
 */
void fyai_cover_probe(struct fyai_cover *cover, bool *userns, bool *bwrap);

#endif
