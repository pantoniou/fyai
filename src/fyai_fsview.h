/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FSVIEW_H
#define FYAI_FSVIEW_H

#include "fyai_manifest.h"
#include "fyai_sandbox.h"
#include "fyai_project.h"

struct fyai_cfg;

/* Mount point of the backing runtime tree below the scratch directory. */
#define FYAI_FSVIEW_BACKING_NAME ".fyai-view-runtime"

/*
 * Make @fd, a directory of the arena, the arena of an agent runtime in a view:
 * close-on-exec, named /proc/self/fd/@fd, and in a process that is not
 * dumpable, so another process of the view cannot open it through /proc. An
 * arena directory that named the previous descriptor names the new one.
 */
int fyai_fsview_arena_adopt(struct fyai_cfg *cfg, int fd);
/* Close the arena descriptor in a process that runs no agent runtime. */
void fyai_fsview_arena_drop(struct fyai_cfg *cfg);
/* Keep the arena descriptor across the next execution of this program. */
int fyai_fsview_arena_pass(const struct fyai_cfg *cfg);

struct fyai_fsview {
	const char *project;
	/* Absolute directory that the child replaces with a private tmpfs. */
	const char *scratch;
	const char *runtime;
	const char *storage;
	const char *arena;
	/* Reference to the borrowed immutable lower manifest, for result capture. */
	fy_generic baseline;
	bool metacopy;
	bool terminal;
	bool verify;
	bool lazy;
	/*
	 * An agent runtime keeps the arena as a detached mount behind a
	 * descriptor. No projection mounts the arena at a path.
	 */
	bool agent;
};

struct fyai_fsview_mount {
	uint64_t namespace_device;
	uint64_t namespace_inode;
	uint64_t mount_id;
	uint64_t cover_id;
	uint64_t root_inode;
};

/*
 * Resolve the scratch directory: the configured path, else $TMPDIR, else
 * /tmp. The result is absolute and canonical; the caller frees it.
 */
char *fyai_fsview_scratch(const char *configured);

/*
 * A view keeps its baseline and its result as manifest files in the manifests
 * directory of the project storage, and holds a reference to each: a mapping
 * with the version, the root, the name of the file and the number of objects.
 */
#define FYAI_FSVIEW_SNAPSHOT_VERSION 3

/* Build a reference in gb. */
fy_generic fyai_fsview_reference(struct fy_generic_builder *gb,
				 const unsigned char root[FYAI_CAS_HASH_SIZE], uint64_t objects,
				 const char *name);

/*
 * Open the manifest that a reference names and check that it holds the root
 * of the reference. Return 0, or -1 with errno set.
 */
int fyai_fsview_manifest_open(const char *storage, fy_generic reference,
			      struct fyai_manifest *manifest);

/* Publish a manifest and return the name of its file. */
int fyai_fsview_manifest_publish(const char *storage, const struct fyai_manifest *manifest,
				 bool durable, char name[FYAI_MANIFEST_NAME_SIZE]);

/*
 * Reject a project that the scratch tmpfs or the backing tree would hide, and a
 * scratch directory that the project mount would hide.
 */
bool fyai_fsview_project_usable(const char *project, const char *scratch);

/* Probe rootless CAS reads, metadata, and isolated write copy-up. */
int fyai_fsview_metacopy_check(const char *runtime);
/* Whether this process may enter the user, mount and PID namespaces of a view. */
bool fyai_fsview_namespace_usable(void);

/*
 * Whether this process may hide paths with fyai_fsview_hide(): it can make a
 * user, mount and PID namespace. A probe child makes them.
 */
bool fyai_fsview_hide_usable(void);

/*
 * Run the rest of this process in private user, mount and PID namespaces, with
 * each of the @n paths covered by an empty read-only mount (a directory by an
 * empty one, a file by /dev/null) and a procfs that lists only the processes of
 * the namespace. The capabilities are dropped, so nothing that runs here sees
 * or changes what the paths hold, undoes the cover, or reaches the processes
 * that started it. A path that does not exist is skipped. The
 * @mount_n mounts are made before the capabilities go.
 *
 * The call forks twice. The process that calls it never returns: it waits for
 * the init of the namespace and ends with the status of the program, so the
 * parent sees one child, as before. The init reaps what the program leaves and
 * ends with it, and the kernel then ends the rest of the namespace. Only the
 * program returns. Call it from a process that has one thread, before it
 * confines itself. Returns 0 in the program, or -1 with errno set before any
 * fork.
 */
int fyai_fsview_hide(const char *const *paths, size_t n,
		     const struct fyai_sandbox_mount *mount, size_t mount_n);

/*
 * Make each of the @n mounts whose `late` is @late, in order, in the mount
 * namespace of this process, which must hold the capabilities to mount. A
 * mount that exists is left as it is, so the calls of a scope that share a
 * namespace make it once. Returns 0, or -1 with errno set at the first mount
 * that fails.
 */
int fyai_fsview_mounts(const struct fyai_sandbox_mount *mount, size_t n, bool late);

/*
 * Start the holder of a scope: user, mount and PID namespaces with each of the
 * @n paths covered as fyai_fsview_hide() covers them, and an init that stays
 * until @hold, the read end of a pipe, reads end of file. @owner is the write
 * end, which the holder closes: only this process may keep it. The init then
 * sends SIGTERM to every process of the namespace, waits a grace time, and ends
 * them with SIGKILL. The holder has a group of its own and ends if this process
 * does. On success *@waiter is the child that this process must reap, and
 * *@init is the process whose namespaces fyai_cover_join() joins. Returns 0, or
 * -1 with errno set.
 */
/*
 * Register @fn, to run with @arg in this process after the program of
 * fyai_fsview_hide() or fyai_cover_join() has ended, and before this process
 * ends. Without it, this process joins the namespaces and cannot see the
 * files that they cover or the mounts that they hide. With it, those two calls
 * start with fyai_fsview_outer(): this process waits outside the namespaces
 * and runs @fn there.
 */
void fyai_fsview_on_parent_exit(void (*fn)(void *arg), void *arg);

/*
 * Make the process that registered a function with fyai_fsview_on_parent_exit()
 * wait outside the namespaces: it forks, and the child goes on. The parent runs
 * the function when the child ends, and ends with its status. Does nothing
 * for any other process. Returns 0, or -1 with errno set when the fork failed.
 */
int fyai_fsview_outer(void);

/* Set the time that the init of a holder gives its processes after SIGTERM. */
void fyai_fsview_set_grace(long ms);

int fyai_fsview_holder(const char *const *paths, size_t n, int hold,
		       int owner, pid_t *waiter, pid_t *init);

/* Drop every capability of this process and lock the securebits, as a view does. */
int fyai_fsview_drop_capabilities(void);

/* Read-only inspection mounts persist in the caller's mount namespace. */
int fyai_fsview_mount(const struct fyai_fsview *view, const char *target,
		      struct fyai_fsview_mount *identity);
/* On a busy unmount, identity records any cover already removed. */
int fyai_fsview_unmount(const struct fyai_fsview *view, const char *target,
			struct fyai_fsview_mount *identity);

/* Verify borrowed CAS provenance and bytes; error is a caller-owned buffer. */
int fyai_fsview_verify(const struct fyai_fsview *view, char *error, size_t error_size);

/*
 * Validate the retained lower, CAS bytes, and latest merged snapshot after
 * reboot.
 */
int fyai_fsview_recover(struct fy_generic_builder *gb, const struct fyai_fsview *view,
			fy_generic expected, char *error, size_t error_size);

/*
 * Called only in a prepared tool child. Parent waits for the PID-namespace
 * init. A non-negative @announce_fd is a sequenced-packet socket. The process
 * that runs tool code sends one byte with a pidfd of itself, then waits for the
 * release of the supervisor and returns. The pidfd names the process in every
 * namespace, so a supervisor in an ancestor namespace, and the credential
 * transport, take its PID there. The release can carry one environment variable.
 * Every process closes the descriptor.
 */
int fyai_fsview_enter(struct fyai_cfg *cfg, const struct fyai_fsview *view,
		      int status_fd, int announce_fd);

/*
 * Supervisor side of @announce_fd. Receive the announcement and return the pidfd
 * of the process in @pidfd, which the caller closes.
 */
int fyai_fsview_init_pidfd(int fd, int *pidfd);

/*
 * Release the announced process, with one environment variable for it when @name
 * and @value are given. Not releasing it, and closing @fd, makes it fail.
 */
int fyai_fsview_init_release(int fd, const char *name, const char *value);

/* Ingest a frozen upper through a trusted private overlay mount. Result belongs
 * to gb. */
fy_generic fyai_fsview_snapshot(struct fy_generic_builder *gb, const struct fyai_fsview *view,
				char *error, size_t error_size);

/*
 * Remove the contents of the runtime tree @fd, a directory of this process, and
 * leave the directory. Only private entries: it never changes the metadata of a
 * shared blob. Returns 0, or -1 with errno set.
 */
int fyai_fsview_runtime_clear(int fd);

/*
 * Whether a view can run what the Linux views run: terminal sessions, agents,
 * the transport, a confinement profile and the program itself again. A platform
 * without it runs standalone commands only.
 */
bool fyai_fsview_sessions_supported(void);

/* Make a pipe whose ends close on exec. */
int fyai_fsview_pipe(int fd[2]);

/* Make a connected pair of sequenced-packet sockets that close on exec. */
int fyai_fsview_seqpacket_pair(int fd[2]);

/*
 * Write the absolute path of the running program to @path, which holds
 * @capacity bytes, with no terminating NUL guarantee. Returns the length, or -1
 * with errno set.
 */
ssize_t fyai_fsview_executable(char *path, size_t capacity);

/* Write the identity of this boot of the machine, 36 characters, and a NUL. */
int fyai_fsview_boot_id(char id[37]);

/*
 * Make the data of a view that the runtime and the storage directories hold
 * durable. Returns 0, or -1 with errno set.
 */
int fyai_fsview_sync_backing(const char *runtime, const char *storage);

/*
 * Make a stored view ready to run. A backend that runs the view in a physical
 * directory checks the baseline and makes that directory, or finds it again,
 * and sets spec->execution; *@execution is then the string that spec names, which
 * the caller frees, and *@view records it. A platform that needs nothing does
 * nothing. Returns 0, or -1 with errno set or the cause in @error.
 */
struct fyai_ctx;
int fyai_fsview_prepare(struct fyai_ctx *ctx, struct fyai_fsview *spec, fy_generic *view,
			char **execution, char *error, size_t error_size);

/* Confine the calling child to the physical directory of the view, if the platform needs it. */
int fyai_fsview_confine(struct fyai_ctx *ctx, const struct fyai_fsview *view);

#endif
