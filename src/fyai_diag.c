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
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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

/* Per-process state for the append-only, unbuffered trace log. */
#define FYAI_TRACE_UNSET (-2)
static int trace_fd = FYAI_TRACE_UNSET;
static char *trace_file;
static enum fyai_error_type trace_level = FYAIET_DEBUG;
static char trace_tag[128];

static bool trace_off_value(const char *v)
{
	return !*v || !strcmp(v, "0") || !strcasecmp(v, "off") ||
	       !strcasecmp(v, "no") || !strcasecmp(v, "false");
}

static bool trace_on_value(const char *v)
{
	return !strcmp(v, "1") || !strcasecmp(v, "on") ||
	       !strcasecmp(v, "yes") || !strcasecmp(v, "true");
}

/* The lowest severity recorded, by name; an unknown name records everything. */
static void trace_level_setup(void)
{
	const char *v = getenv("FYAI_TRACE_LEVEL");
	unsigned int i;

	if (!v || !*v)
		return;
	for (i = 0; i < FYAIET_COUNT; i++) {
		if (diag_type_names[i] && !strcasecmp(v, diag_type_names[i])) {
			trace_level = (enum fyai_error_type)i;
			return;
		}
	}
	if (!strcasecmp(v, "error"))
		trace_level = FYAIET_ERROR;
}

/* Resolve the trace path, or NULL when tracing is off. The caller frees it. */
static char *trace_path_setup(void)
{
	const char *v = getenv("FYAI_TRACE");
	const char *home;
	char *dir, *path;
	int rc;

	if (!v || trace_off_value(v))
		return NULL;
	if (!trace_on_value(v))
		return strdup(v);

	home = getenv("HOME");
	if (!home || !*home)
		return NULL;
	rc = asprintf(&dir, "%s/.fyai", home);
	if (rc < 0)
		return NULL;
	rc = fyai_mkdir_p(dir);
	free(dir);
	if (rc)
		return NULL;
	if (asprintf(&path, "%s/.fyai/trace.log", home) < 0)
		return NULL;
	return path;
}

/* Open the trace on demand and fail without raising a diagnostic. */
static int trace_open(void)
{
	char *path;
	int rc;

	if (trace_fd != FYAI_TRACE_UNSET)
		return trace_fd;

	trace_fd = -1;			/* tried; do not try again */
	path = trace_path_setup();
	if (!path)
		return -1;
	trace_level_setup();
	trace_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (trace_fd < 0) {
		free(path);
		trace_fd = -1;
		return -1;
	}
	/* Keep the trace clear of the tool-child control descriptors. */
	rc = fcntl(trace_fd, F_DUPFD_CLOEXEC, 10);
	if (rc >= 0) {
		close(trace_fd);
		trace_fd = rc;
	}
	trace_file = path;
	fyai_diag_tracef("start", "pid %ld, parent %ld",
			 (long)getpid(), (long)getppid());
	return trace_fd;
}

void fyai_diag_trace_reopen(void)
{
	/* The inherited descriptor was closed and its number may be reused. */
	trace_fd = FYAI_TRACE_UNSET;
	free(trace_file);
	trace_file = NULL;
	(void)trace_open();
}

const char *fyai_diag_trace_path(void)
{
	return trace_open() >= 0 ? trace_file : NULL;
}

void fyai_diag_trace_tag(const char *tag)
{
	if (!tag)
		tag = "";
	snprintf(trace_tag, sizeof(trace_tag), "%s", tag);
}

/* One record, one write(2): a partial line would be read as another one. */
static void trace_write(const char *head, const char *body)
{
	struct timespec ts;
	struct tm tm;
	char stamp[32];
	char *rec, *p, *cur;
	ssize_t wr;
	int len;

	if (clock_gettime(CLOCK_REALTIME, &ts) ||
	    !gmtime_r(&ts.tv_sec, &tm))
		return;
	len = (int)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
	if (!len)
		return;
	rec = fy_sprintfa("%s.%06ldZ %ld %s%s%s%s%s\n", stamp,
			   (long)(ts.tv_nsec / 1000), (long)getpid(),
			   *trace_tag ? "[" : "", trace_tag,
			   *trace_tag ? "] " : "", head, body ? body : "");
	len = (int)strlen(rec);
	/* Replace line breaks in the trace record. */
	for (p = rec; *p; p++) {
		if (p[0] == '\n' && p[1])
			p[0] = ' ';
		else if (p[0] == '\r')
			p[0] = ' ';
	}
	/* Write the complete record. Continue after a short write. */
	cur = rec;
	while (len > 0) {
		wr = write(trace_fd, cur, (size_t)len);
		if (wr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (!wr)
			break;
		cur += wr;
		len -= (int)wr;
	}
}

void fyai_diag_tracef(const char *kind, const char *fmt, ...)
{
	va_list ap;
	char *body;
	char *head;

	if (trace_open() < 0)
		return;
	va_start(ap, fmt);
	body = fy_vsprintfa(fmt, ap);
	va_end(ap);
	head = fy_sprintfa("%s: ", kind ? kind : "trace");
	trace_write(head, body);
}

/* Record one raise, whatever the sink then does with it. */
static void trace_diag(enum fyai_error_type type, enum fyai_error_module module,
		       const char *msg, const char *file, int line,
		       const char *func)
{
	const char *modname;
	char *head, *body;

	if (trace_open() < 0 || type < trace_level)
		return;
	modname = (unsigned int)module < FYAIEM_COUNT ?
			diag_module_names[module] : NULL;
	head = fy_sprintfa("%s %s: ",
			    diag_type_names[type] ? diag_type_names[type] : "error",
			    modname ? modname : "-");
	body = fy_sprintfa("%s (%s:%d %s)", msg, file ? file : "-", line,
			    func ? func : "-");
	trace_write(head, body);
}

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
	/* Open the trace here, so a process appears in it from its first
	 * moment rather than from its first complaint. */
	(void)trace_open();
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

	va_start(ap, fmt);
	msg = fy_vsprintfa(fmt, ap);
	va_end(ap);

	/* Record the severity before a filter changes it. */
	trace_diag(type, module, msg, file, line, func);

	/*
	 * The first error is the cause; the errors behind it are the callers
	 * unwinding, each noticing its callee failed. Demote those to debug so
	 * a generic "X failed" cannot bury the reason - and so a cleanup path
	 * does not have to choose between saying nothing and adding noise.
	 */
	if (type == FYAIET_ERROR && fyai_diag_got_error(diag))
		type = FYAIET_DEBUG;

	if (diag && !(diag->mask & (1u << type))) {
		return;
	}

	/*
	 * No sink, or one that is not collecting: report at once. This is the
	 * path every caller without a configuration takes, so a diagnostic is
	 * never lost merely because it was raised too early.
	 */
	if (!diag || !diag->gb || !diag->collect) {
		diag_emit(diag ? diag->fp : stderr, diag ? diag->source : false,
			  type, module, msg, file, line, func);
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
	if (fy_is_invalid(item))
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
		if (fy_is_invalid(new))
			return;
	} while (!fy_atomic_compare_exchange_weak(&diag->list, &old.v, new.v));
}

/* Render one collected item onto @fp. The item may live in any builder. */
static void diag_emit_item(FILE *fp, bool source, fy_generic item)
{
	fy_generic msg, file, func;

	msg = fy_get(item, "msg", fy_invalid);
	if (fy_is_invalid(msg))
		return;
	/* Held in locals so fy_castp() has stable storage to point into: a
	 * short string lives inline in the generic word. */
	file = fy_get(item, "file", fy_invalid);
	func = fy_get(item, "func", fy_invalid);
	diag_emit(fp, source, (enum fyai_error_type)fy_get(item, "type", 0LL),
		  (enum fyai_error_module)fy_get(item, "module", 0LL),
		  fy_castp(&msg, ""), fy_castp(&file, ""),
		  (int)fy_get(item, "line", 0LL), fy_castp(&func, ""));
}

void fyai_diag_drain(struct fyai_diag *diag)
{
	fy_generic item, list;

	if (!diag || !diag->gb)
		return;

	/* Claim the list, so a raiser racing the drain lands in the next one
	 * rather than being reported twice or not at all. */
	list.v = fy_atomic_exchange(&diag->list, fy_seq_empty_value);

	fy_foreach(item, list)
		diag_emit_item(diag->fp, diag->source, item);
	fflush(diag->fp);

	/*
	 * Every message just reported lives in @gb and nothing else does, so
	 * the reset reclaims them all - along with any sequence orphaned by a
	 * lost publish race. It also invalidates anything a concurrent raiser
	 * is holding, which is why this runs only where they are quiescent.
	 */
	fy_generic_builder_reset(diag->gb);
}

char *fyai_diag_string(struct fyai_diag *diag)
{
	fy_generic item, list;
	char *text = NULL;
	size_t len = 0;
	FILE *fp;

	if (!diag || !diag->gb || !fyai_diag_got_error(diag))
		return NULL;
	fp = open_memstream(&text, &len);
	if (!fp)
		return NULL;
	/* Read the diagnostics but do not remove them. */
	list.v = fy_atomic_load(&diag->list);
	fy_foreach(item, list)
		diag_emit_item(fp, false, item);
	if (fclose(fp)) {
		free(text);
		return NULL;
	}
	return text;
}

fy_generic fyai_diag_take_generic(struct fyai_diag *diag,
				  struct fy_generic_builder *gb)
{
	fy_generic item, msg, file, func, list, out;

	if (!diag || !diag->gb || !gb)
		return fy_invalid;

	/* Move the list to the receiver. */
	list.v = fy_atomic_exchange(&diag->list, fy_seq_empty_value);
	out.v = fy_seq_empty_value;
	fy_foreach(item, list) {
		msg = fy_get(item, "msg", fy_invalid);
		if (fy_is_invalid(msg))
			continue;
		file = fy_get(item, "file", fy_invalid);
		func = fy_get(item, "func", fy_invalid);
		/* Copy each item before the builder is reset. */
		out = fy_append(gb, out, fy_gb_mapping(gb,
				"type", fy_get(item, "type", 0LL),
				"module", fy_get(item, "module", 0LL),
				"msg", fy_castp(&msg, ""),
				"file", fy_castp(&file, ""),
				"line", fy_get(item, "line", 0LL),
				"func", fy_castp(&func, "")));
		if (fy_is_invalid(out))
			break;
	}
	fy_generic_builder_reset(diag->gb);
	return out;
}

void fyai_diag_adopt(struct fyai_diag *diag, fy_generic list,
		     const char *origin)
{
	fy_generic item, msg, file, func;
	const char *text, *open, *mark, *close;

	fy_foreach(item, list) {
		msg = fy_get(item, "msg", fy_invalid);
		if (fy_is_invalid(msg))
			continue;
		file = fy_get(item, "file", fy_invalid);
		func = fy_get(item, "func", fy_invalid);
		text = fy_castp(&msg, "");
		/* Keep a marker from a child delegation. */
		open = mark = close = "";
		if (*text != FYAI_DIAG_MARK_OPEN && !fy_str_empty(origin)) {
			open = "[";
			mark = origin;
			close = "] ";
		}
		/* Keep the recorded severity, module, and source. */
		fyai_diagf(diag,
			   (enum fyai_error_type)fy_get(item, "type", 0LL),
			   (enum fyai_error_module)fy_get(item, "module", 0LL),
			   fy_castp(&file, ""), (int)fy_get(item, "line", 0LL),
			   fy_castp(&func, ""), "%s%s%s%s",
			   open, mark, close, text);
	}
}

struct fyai_diag *fyai_ctx_diag(struct fyai_ctx *ctx)
{
	return ctx && ctx->cfg ? &ctx->cfg->diag : NULL;
}

struct fyai_diag *fyai_cfg_diag(struct fyai_cfg *cfg)
{
	return cfg ? &cfg->diag : NULL;
}
