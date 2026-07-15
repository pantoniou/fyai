/*
 * fyai_diag.h - collected diagnostics
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_DIAG_H
#define FYAI_DIAG_H

#include <stdbool.h>
#include <stdio.h>

#include <libfyaml/libfyaml-atomics.h>
#include <libfyaml/libfyaml-generic.h>
#include <libfyaml/libfyaml-util.h>

struct fyai_ctx;
struct fyai_cfg;

enum fyai_error_type {
	FYAIET_DEBUG,
	FYAIET_INFO,
	FYAIET_NOTICE,
	FYAIET_WARNING,
	FYAIET_ERROR,
	FYAIET_COUNT,
};

/*
 * The subsystem a diagnostic belongs to; drained as the message prefix, so
 * FYAIEM_CONFIG prints "config: ...". Formalizes the prefix each subsystem
 * used to hand-write into every format string.
 */
enum fyai_error_module {
	FYAIEM_UNKNOWN,
	FYAIEM_CONFIG,
	FYAIEM_AUTH,
	FYAIEM_CATALOG,
	FYAIEM_STORAGE,
	FYAIEM_SESSION,
	FYAIEM_TOOLS,
	FYAIEM_STREAM,
	FYAIEM_DISPLAY,
	FYAIEM_SECRET,
	FYAIEM_INIT,
	FYAIEM_LOG,
	FYAIEM_COUNT,
};

/*
 * A diagnostic sink. Owns its builder outright: nothing else builds into @gb,
 * so a drain can reclaim every message at once with
 * fy_generic_builder_reset().
 *
 * It deliberately does not reuse an existing builder. ctx->transient_gb is
 * destroyed (not rewound) at the end of every turn and does not exist at all
 * before fyai_run() declares the context - which is where a third of the
 * diagnostics in the tree are raised. ctx->gb/durable_gb is the
 * content-addressed arena and must never see a diagnostic.
 *
 * @list is the raw generic word rather than an fy_generic so it can be updated
 * atomically: the builder is thread safe, but publishing an appended sequence
 * is a read-modify-write that would drop diagnostics if two threads raced. See
 * fyai_diagf() for the lock-free append and fyai_diag_drain() for the one
 * ordering rule the CAS does not buy.
 */
struct fyai_diag {
	struct fy_generic_builder *gb;
	FY_ATOMIC(fy_generic_value) list; /* sequence of diagnostic mappings */
	FILE *fp;			/* drain target, stderr by default */
	unsigned int mask;		/* 1u << type, per enabled severity */
	bool source;			/* drain the file/line/func origin */
	bool collect;			/* false: drain each diagnostic at once */
};

int fyai_diag_setup(struct fyai_diag *diag);
void fyai_diag_cleanup(struct fyai_diag *diag);

/*
 * Print every collected diagnostic to @diag->fp and reset the sink. Call at a
 * point where stderr output cannot tear an in-flight render - the end of a
 * turn, of a slash command, or of a verb - never in the middle of one.
 *
 * The reset invalidates every generic built into @diag->gb, which is sound
 * only because a diagnostic holds no reference to any other builder: see
 * fyai_diagf().
 *
 * Raising is safe from any thread, but draining is not concurrent with it: the
 * list is claimed atomically, yet the reset that follows invalidates @gb for
 * everyone, including a raiser that has just built an item and not yet
 * published it. Drain only where the raisers are quiescent - which is what the
 * turn and verb boundaries are.
 */
void fyai_diag_drain(struct fyai_diag *diag);

/* True once an error has been raised and not yet drained or reset. */
bool fyai_diag_got_error(struct fyai_diag *diag);

/*
 * Discard the collected diagnostics without reporting them: the caller
 * recovered, so its complaints are moot - and dropping the error with them lets
 * the next failure be reported as the cause again. Use it where a failure is
 * tried and then worked around, not to silence one.
 */
void fyai_diag_reset(struct fyai_diag *diag);

/*
 * Raise one diagnostic. @diag may be NULL, in which case the message goes
 * straight to stderr - which is what keeps the sink-less callers (anything
 * running before the configuration is set up, and the leaf helpers that take
 * neither a context nor a configuration) working unchanged.
 *
 * The format is expanded into a plain buffer before it is interned, so the
 * result never points into another builder's storage. Do not hand a foreign
 * fy_generic to a diagnostic; format it first, or fyai_diag_drain()'s reset
 * would leave a dangling reference behind.
 *
 * An error raised while an error is already collected is demoted to debug: the
 * first one is the cause, and the ones behind it are only each caller noticing
 * that its callee failed. That is what lets a cleanup path keep its "could not
 * do X" without burying the reason underneath it. Draining reports the cause
 * and clears the state, so the next failure is a cause again.
 */
void fyai_diagf(struct fyai_diag *diag, enum fyai_error_type type,
		enum fyai_error_module module, const char *file, int line,
		const char *func, const char *fmt, ...)
	FY_FORMAT(printf, 7, 8);

/* NULL-safe sink accessors; keep this header free of the context layout. */
struct fyai_diag *fyai_ctx_diag(struct fyai_ctx *ctx);
struct fyai_diag *fyai_cfg_diag(struct fyai_cfg *cfg);

/*
 * Each source file using the macros below names its subsystem first:
 *
 *	#define FYAI_MODULE FYAIEM_CONFIG
 *
 * FYAI_MODULE is read where a macro is expanded, not where this header is
 * included, so the define only has to precede the first use. There is no
 * default on purpose: a file that raises diagnostics without naming its module
 * fails to compile rather than reporting them as coming from nowhere.
 */

#define fyai_diag_type(_diag, _type, _fmt, ...) \
	fyai_diagf((_diag), (_type), FYAI_MODULE, __FILE__, __LINE__, \
		   __func__, (_fmt) , ## __VA_ARGS__)

/* Context-scoped: the common case. */
#define fyai_error(_ctx, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag(_ctx), FYAIET_ERROR, (_fmt) , ## __VA_ARGS__)
#define fyai_warning(_ctx, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag(_ctx), FYAIET_WARNING, (_fmt) , ## __VA_ARGS__)
#define fyai_notice(_ctx, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag(_ctx), FYAIET_NOTICE, (_fmt) , ## __VA_ARGS__)
#define fyai_info(_ctx, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag(_ctx), FYAIET_INFO, (_fmt) , ## __VA_ARGS__)
#define fyai_debug(_ctx, _fmt, ...) \
	fyai_diag_type(fyai_ctx_diag(_ctx), FYAIET_DEBUG, (_fmt) , ## __VA_ARGS__)

/*
 * Report and jump to a cleanup label when @_cond does not hold. Only the error
 * severity jumps; a warning does not interrupt the flow.
 */
#define fyai_error_check(_ctx, _cond, _label, _fmt, ...) \
	do { \
		if (!(_cond)) { \
			fyai_error((_ctx), (_fmt) , ## __VA_ARGS__); \
			goto _label; \
		} \
	} while (0)

/* Configuration-scoped: option parsing and the verb argument hooks, which run
 * before there is a context to report into. */
#define fyai_cfg_error(_cfg, _fmt, ...) \
	fyai_diag_type(fyai_cfg_diag(_cfg), FYAIET_ERROR, (_fmt) , ## __VA_ARGS__)
#define fyai_cfg_warning(_cfg, _fmt, ...) \
	fyai_diag_type(fyai_cfg_diag(_cfg), FYAIET_WARNING, (_fmt) , ## __VA_ARGS__)
#define fyai_cfg_notice(_cfg, _fmt, ...) \
	fyai_diag_type(fyai_cfg_diag(_cfg), FYAIET_NOTICE, (_fmt) , ## __VA_ARGS__)

#define fyai_cfg_error_check(_cfg, _cond, _label, _fmt, ...) \
	do { \
		if (!(_cond)) { \
			fyai_cfg_error((_cfg), (_fmt) , ## __VA_ARGS__); \
			goto _label; \
		} \
	} while (0)

#endif
