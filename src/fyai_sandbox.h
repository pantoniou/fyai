/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SANDBOX_H
#define FYAI_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Native confinement for tool sub-executions: Landlock on Linux and Seatbelt
 * on macOS. The spec is applied in the forked child, after the stdio dup2 and
 * immediately before exec, so the restriction is inherited by the tool and
 * every process it spawns. Landlock restrictions can be tightened in a child.
 * Seatbelt does not support nested application; launch each confined execution
 * from an unconfined supervisor.
 *
 * fyai keeps the admission decision (config policy / prompt) elsewhere; this
 * module is only the enforcement floor: given that a command is allowed to
 * run, bound what it may touch.
 *
 * Landlock has no "deny" rule - deny is the default for every handled access
 * type, and a path is forbidden precisely by *not* granting it. A deny-list
 * under an otherwise-granted directory is therefore honored by a carve-out
 * walk (grant every sibling on the way down except the branch leading to the
 * denied path), not by a subtractive rule. The config's deny list (plus the
 * always-present .fyai arena) feeds that walk.
 */

/*
 * Access mode for a grant. Landlock rights are finer-grained than read/write;
 * these name the useful compositions (resolved to right bitmasks, and masked to
 * the running ABI and to file-vs-directory applicability, inside the module):
 *
 *   RW      read + execute + write + create/delete/rename (the default)
 *   RO      read + execute only
 *   EDIT    read + execute + modify existing files (write, truncate), but no
 *           creating, deleting, or renaming directory entries
 *   APPEND  read + execute + write to existing files without truncating them,
 *           and no create/delete/rename
 */
enum fyai_sandbox_mode {
	FYAI_SB_RW = 0,
	FYAI_SB_RO,
	FYAI_SB_EDIT,
	FYAI_SB_APPEND,
};

/* How a mount shows its source. */
enum fyai_sandbox_mount_kind {
	/* The target is the source: the same files, writes included. */
	FYAI_MOUNT_BIND = 0,
	/*
	 * The target shows the source, and keeps what the tool writes in @upper, which
	 * @work supports. The source never changes. Both are directories on one file
	 * system that holds extended attributes.
	 */
	FYAI_MOUNT_OVERLAY,
};

/*
 * A mount made in the mount namespace of the tool, before the tool is
 * confined. @source and @target exist, and a file is a bind only. The caller
 * owns the strings.
 */
struct fyai_sandbox_mount {
	enum fyai_sandbox_mount_kind kind;
	const char *source;
	const char *target;
	const char *upper;
	const char *work;
	bool readonly;
	/* Make it after the procfs of the namespace is mounted, which hides what is below /proc. */
	bool late;
};

/* One filesystem grant. */
struct fyai_sandbox_path {
	const char *path;
	enum fyai_sandbox_mode mode;
};

/* Map a config mode name to an enum; reject unknown names. */
int fyai_sandbox_mode_parse(const char *name, enum fyai_sandbox_mode *modep);

/*
 * Drop every environment variable except a small, non-secret set (PATH,
 * HOME, shell/locale/terminal basics), so credentials in the parent's
 * environment (<PROVIDER>_API_KEY and friends) never reach a tool or the
 * processes it spawns. Portable; call in the child before exec (or at the top
 * of the `fyai tool` one-shot).
 *
 * @keep is a NULL-terminated list of further names to keep, or NULL. Only
 * fyai names them, from its configuration; never a model.
 *
 * Returns 0 when the environment holds nothing but the kept set, -1 when a
 * variable could not be removed. Fail closed on -1: a partial sanitize still
 * carries credentials.
 */
int fyai_env_sanitize(const char *const *keep);

/* Longest environment variable name fyai_env_sanitize() can remove. */
#define FYAI_ENV_NAME_MAX 256

struct fyai_sandbox_spec {
	/* Grant read and execute beneath /, except globally denied paths. */
	bool read_all;

	/*
	 * Let the tool open and list every directory: the entries, never the
	 * files. A program that changes to / and opens "." needs it.
	 */
	bool list_root;

	/*
	 * Mounts of the tool, made in order in its mount namespace before it is
	 * confined. One that exists is kept. A mount that fails fails the tool.
	 */
	const struct fyai_sandbox_mount *mount;
	size_t mount_n;

	/*
	 * Project root, granted read/write recursively EXCEPT any path in
	 * @deny that falls beneath it (the arena ".fyai" is always denied).
	 * NULL skips the project grant.
	 */
	const char *project_root;
	/* Private scratch directory of a filesystem view, with global denies applied. */
	const char *scratch_root;

	/* Extra explicit grants (config sandbox.allow), applied after the
	 * project. Each is granted whole; deny carve-outs are not applied to
	 * these (an explicit allow wins). */
	const struct fyai_sandbox_path *allow;
	size_t allow_n;

	/*
	 * Absolute denied paths. The first @deny_global_n apply to every grant;
	 * the remaining entries apply only beneath @project_root.
	 */
	const char *const *deny;
	size_t deny_n;
	size_t deny_global_n;

	/*
	 * Each selected protocol is restricted to its listed destination ports.
	 * TCP binds are restricted only when @restrict_tcp_bind is set. UDP binds
	 * are restricted with UDP sends; destination grants permit ephemeral source
	 * binds. TCP requires ABI 4; UDP requires ABI 10.
	 */
	bool restrict_tcp;
	const uint16_t *tcp_ports;
	size_t tcp_ports_n;
	bool restrict_tcp_bind;
	const uint16_t *tcp_bind_ports;
	size_t tcp_bind_ports_n;
	bool restrict_udp;
	const uint16_t *udp_ports;
	size_t udp_ports_n;
	const uint16_t *udp_bind_ports;
	size_t udp_bind_ports_n;
	/*
	 * Deny every UDP socket with a seccomp filter, for a kernel without
	 * Landlock ABI 10. The filter cannot test a port: use it only when no UDP
	 * port is permitted. Exclusive with @restrict_udp.
	 */
	bool deny_udp;

	/*
	 * Existing paths to hide: each is covered by an empty read-only mount in a
	 * private mount namespace before confinement. A hidden path needs no entry
	 * in @deny, and the directories above it keep their write rights, which a
	 * denied path takes away. The caller checks fyai_fsview_hide_usable().
	 */
	const char *const *hide;
	size_t hide_n;
	/*
	 * The holder whose namespaces cover @hide, when a holder owns the cover
	 * (fyai_cover.h): the process joins them in place of making its own. 0 when
	 * the process makes its own.
	 */
	pid_t cover_holder;

	/*
	 * Remove from PATH every entry that the grants do not reach: a relative
	 * or empty entry, one under a denied path, and one outside the system,
	 * scratch, project and allow trees. A tool then cannot find a program
	 * that it could not run. Applied with the ruleset, in the child.
	 */
	bool filter_path;

	/*
	 * When Landlock is unavailable or too old for the requested policy:
	 * strict fails the exec (fail-closed); non-strict proceeds unconfined
	 * (the config policy / prompt remains the portable floor).
	 */
	bool strict;
};

/*
 * True when @dir is an absolute path without a ".." segment that @spec lets a
 * tool execute from: it is under a system, scratch, project, scratch_root or
 * allow tree and under no denied path.
 */
bool fyai_sandbox_path_visible(const struct fyai_sandbox_spec *spec,
			       const char *dir);

/* Highest supported Landlock ABI (>=1), 0 if unsupported, <0 on error. */
int fyai_sandbox_abi(void);

/* True when at least filesystem confinement is available on this kernel. */
bool fyai_sandbox_available(void);

/* True if this build and kernel can restrict TCP connect; -1 probes ABI. */
bool fyai_sandbox_net_restrictable(int abi);

/* True if this build and kernel can restrict UDP bind and send; -1 probes ABI. */
bool fyai_sandbox_udp_restrictable(int abi);

/*
 * True if a seccomp filter can deny all UDP sockets: a supported architecture
 * and a kernel with seccomp. It stands in for ABI 10 when no port is permitted.
 */
bool fyai_sandbox_udp_deniable(void);

/*
 * Apply @spec to the current process irreversibly. Returns 0 on success (the
 * caller proceeds to exec), -1 on a failure the caller should treat per
 * spec->strict. Must be called from the child, post-dup2, pre-exec.
 */
int fyai_sandbox_apply(const struct fyai_sandbox_spec *spec);

#endif
