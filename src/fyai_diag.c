/*
 * fyai_diag.c - collected diagnostics
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "fyai.h"
#include "fyai_diag.h"

/*
 * The severity label. An error carries none: it is the overwhelming default and
 * the subsystem prefix already reads as a failure, so labelling every one of
 * them would only add noise to the messages this replaces.
 */
static const char *const diag_type_names[FYAIET_COUNT] = {
	[FYAIET_DEBUG]		= "debug",
	[FYAIET_INFO]		= "info",
	[FYAIET_NOTICE]		= "note",
	[FYAIET_WARNING]	= "warning",
	[FYAIET_ERROR]		= NULL,
};

/* The message prefix; FYAIEM_UNKNOWN stays unprefixed. */
static const char *const diag_module_names[FYAIEM_COUNT] = {
	[FYAIEM_UNKNOWN]	= NULL,
	[FYAIEM_CONFIG]		= "config",
	[FYAIEM_AUTH]		= "auth",
	[FYAIEM_CATALOG]	= "catalog",
	[FYAIEM_STORAGE]	= "storage",
	[FYAIEM_SESSION]	= "session",
	[FYAIEM_TOOLS]		= "tools",
	[FYAIEM_STREAM]		= "stream",
	[FYAIEM_DISPLAY]	= "display",
	[FYAIEM_SECRET]		= "secret",
	[FYAIEM_INIT]		= "init",
	[FYAIEM_LOG]		= "logging",
	[FYAIEM_EVENT]		= "event",
};

int fyai_diag_setup(struct fyai_diag *diag)
{
	struct fy_generic_builder_cfg gb_cfg;

	if (!diag)
		return -1;

	memset(diag, 0, sizeof(*diag));

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	diag->gb = fy_generic_builder_create(&gb_cfg);
	if (!diag->gb)
		return -1;

	/* Not the all-zero word: an empty sequence is a tagged value. */
	fy_atomic_store(&diag->list, fy_seq_empty_value);
	diag->fp = stderr;
	diag->collect = true;
	diag->source = false;
	/* Debug and info stay off until the configuration turns them on. */
	diag->mask = (1u << FYAIET_NOTICE) | (1u << FYAIET_WARNING) |
		     (1u << FYAIET_ERROR);
	return 0;
}

void fyai_diag_cleanup(struct fyai_diag *diag)
{
	if (!diag)
		return;

	/* Nothing collected may be dropped silently. */
	fyai_diag_drain(diag);

	if (diag->gb) {
		fy_generic_builder_destroy(diag->gb);
		diag->gb = NULL;
	}
	fy_atomic_store(&diag->list, fy_seq_empty_value);
}

/* Format one diagnostic onto @fp, in the shape the subsystems used to print by
 * hand: "<module>: [<severity>: ]<message>". */
static void diag_emit(FILE *fp, bool source, enum fyai_error_type type,
		      enum fyai_error_module module, const char *msg,
		      const char *file, int line, const char *func)
{
	const char *modname, *typename;

	modname = (unsigned int)module < FYAIEM_COUNT ?
			diag_module_names[module] : NULL;
	typename = (unsigned int)type < FYAIET_COUNT ?
			diag_type_names[type] : NULL;

	if (modname)
		fprintf(fp, "%s: ", modname);
	if (typename)
		fprintf(fp, "%s: ", typename);
	fprintf(fp, "%s\n", msg);

	if (source && file)
		fprintf(fp, "  at %s:%d %s()\n", file, line, func ? func : "");
}

bool fyai_diag_got_error(struct fyai_diag *diag)
{
	fy_generic item, list;

	if (!diag || !diag->gb)
		return false;

	list.v = fy_atomic_load(&diag->list);
	fy_foreach(item, list) {
		if (fy_get(item, "type", 0LL) == (long long)FYAIET_ERROR)
			return true;
	}
	return false;
}

void fyai_diag_reset(struct fyai_diag *diag)
{
	if (!diag || !diag->gb)
		return;

	fy_atomic_store(&diag->list, fy_seq_empty_value);
	fy_generic_builder_reset(diag->gb);
}

void fyai_diagf(struct fyai_diag *diag, enum fyai_error_type type,
		enum fyai_error_module module, const char *file, int line,
		const char *func, const char *fmt, ...)
{
	fy_generic item, old, new;
	va_list ap;
	char *msg;
	int rc;

	/*
	 * The first error is the cause; the errors behind it are the callers
	 * unwinding, each noticing its callee failed. Demote those to debug so
	 * a generic "X failed" cannot bury the reason - and so a cleanup path
	 * does not have to choose between saying nothing and adding noise.
	 */
	if (type == FYAIET_ERROR && fyai_diag_got_error(diag))
		type = FYAIET_DEBUG;

	if (diag && !(diag->mask & (1u << type)))
		return;

	va_start(ap, fmt);
	rc = vasprintf(&msg, fmt, ap);
	va_end(ap);
	if (rc < 0)
		return;

	/*
	 * No sink, or one that is not collecting: report at once. This is the
	 * path every caller without a configuration takes, so a diagnostic is
	 * never lost merely because it was raised too early.
	 */
	if (!diag || !diag->gb || !diag->collect) {
		diag_emit(diag ? diag->fp : stderr, diag ? diag->source : false,
			  type, module, msg, file, line, func);
		free(msg);
		return;
	}

	/*
	 * @msg is interned into the sink's own builder; nothing here points
	 * into another builder, which is what lets a drain reset @gb wholesale.
	 */
	item = fy_mapping(diag->gb,
			  "type", (long long)type,
			  "module", (long long)module,
			  "msg", msg,
			  "file", file ? file : "",
			  "line", (long long)line,
			  "func", func ? func : "");
	free(msg);

	if (fy_generic_is_invalid(item))
		return;

	/*
	 * Publish lock-free. The builder is thread safe, but appending is a
	 * read-modify-write of the list word, so a plain store would drop a
	 * diagnostic whenever two threads raised at once. The CAS reloads the
	 * losing side's expectation, and the retry re-appends onto whatever
	 * won. A lost race orphans the sequence it built; that is garbage in
	 * @gb until the next drain resets it, which is cheaper than the
	 * coordination avoiding it would cost.
	 */
	old.v = fy_atomic_load(&diag->list);
	do {
		new = fy_append(diag->gb, old, item);
		if (fy_generic_is_invalid(new))
			return;
	} while (!fy_atomic_compare_exchange_weak(&diag->list, &old.v, new.v));
}

void fyai_diag_drain(struct fyai_diag *diag)
{
	fy_generic item, msg, file, func, list;

	if (!diag || !diag->gb)
		return;

	/* Claim the list, so a raiser racing the drain lands in the next one
	 * rather than being reported twice or not at all. */
	list.v = fy_atomic_exchange(&diag->list, fy_seq_empty_value);

	fy_foreach(item, list) {
		msg = fy_get(item, "msg", fy_invalid);
		if (fy_generic_is_invalid(msg))
			continue;
		/* Held in locals so fy_castp() has stable storage to point
		 * into: a short string lives inline in the generic word. */
		file = fy_get(item, "file", fy_invalid);
		func = fy_get(item, "func", fy_invalid);
		diag_emit(diag->fp, diag->source,
			  (enum fyai_error_type)fy_get(item, "type", 0LL),
			  (enum fyai_error_module)fy_get(item, "module", 0LL),
			  fy_castp(&msg, ""), fy_castp(&file, ""),
			  (int)fy_get(item, "line", 0LL), fy_castp(&func, ""));
	}
	fflush(diag->fp);

	/*
	 * Every message just reported lives in @gb and nothing else does, so
	 * the reset reclaims them all - along with any sequence orphaned by a
	 * lost publish race. It also invalidates anything a concurrent raiser
	 * is holding, which is why this runs only where they are quiescent.
	 */
	fy_generic_builder_reset(diag->gb);
}

struct fyai_diag *fyai_ctx_diag(struct fyai_ctx *ctx)
{
	return ctx && ctx->cfg ? &ctx->cfg->diag : NULL;
}

struct fyai_diag *fyai_cfg_diag(struct fyai_cfg *cfg)
{
	return cfg ? &cfg->diag : NULL;
}
