/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SANDBOX_H
#define FYAI_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Landlock-based confinement for tool sub-executions (the built-in shell
 * tool). The spec is applied in the forked child, after the stdio dup2 and
 * immediately before exec, so the restriction is inherited by the tool and
 * every process it spawns. Landlock restrictions are additive and one-way:
 * once applied they cannot be relaxed, which is exactly the per-tool
 * tightening model the approval policy asks for.
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

/* One filesystem grant. */
struct fyai_sandbox_path {
	const char *path;
	enum fyai_sandbox_mode mode;
};

/* Map a config mode name to an enum; unknown names fall back to RW. */
enum fyai_sandbox_mode fyai_sandbox_mode_parse(const char *name);

/*
 * Drop every environment variable except a small, non-secret set (PATH,
 * HOME, shell/locale/terminal basics), so credentials in the parent's
 * environment (<PROVIDER>_API_KEY and friends) never reach a tool or the
 * processes it spawns. Portable; call in the child before exec (or at the top
 * of the `fyai tool` one-shot).
 */
void fyai_env_sanitize(void);

struct fyai_sandbox_spec {
	/*
	 * Project root, granted read/write recursively EXCEPT any path in
	 * @deny that falls beneath it (the arena ".fyai" is always denied).
	 * NULL skips the project grant.
	 */
	const char *project_root;

	/* Extra explicit grants (config sandbox.allow), applied after the
	 * project. Each is granted whole; deny carve-outs are not applied to
	 * these (an explicit allow wins). */
	const struct fyai_sandbox_path *allow;
	size_t allow_n;

	/* Absolute paths hidden from the tool (config sandbox.deny, plus the
	 * arena). Honored as carve-outs within @project_root. */
	const char *const *deny;
	size_t deny_n;

	/*
	 * Network egress. When @restrict_net is false the network access type
	 * is left unhandled and egress is unaffected. When true, TCP connect is
	 * denied except to the ports in @ports (an empty list denies all
	 * egress). Requires Landlock ABI >= 4; on older kernels a requested net
	 * restriction is treated per @strict.
	 */
	bool restrict_net;
	const uint16_t *ports;
	size_t ports_n;

	/*
	 * When Landlock is unavailable or too old for the requested policy:
	 * strict fails the exec (fail-closed); non-strict proceeds unconfined
	 * (the config policy / prompt remains the portable floor).
	 */
	bool strict;
};

/* Highest supported Landlock ABI (>=1), 0 if unsupported, <0 on error. */
int fyai_sandbox_abi(void);

/* True when at least filesystem confinement is available on this kernel. */
bool fyai_sandbox_available(void);

/*
 * Apply @spec to the current process irreversibly. Returns 0 on success (the
 * caller proceeds to exec), -1 on a failure the caller should treat per
 * spec->strict. Must be called from the child, post-dup2, pre-exec.
 */
int fyai_sandbox_apply(const struct fyai_sandbox_spec *spec);

#endif
