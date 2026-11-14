/*
 * utils.c - common fyai generic helpers
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <libfyaml/libfyaml-align.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_diag.h"
#include "fyai_event.h"
#include "fyai_host.h"
#include "fyai_sink.h"
#include "fyai_sandbox.h"
#include "fyai_fsview.h"
#include "fyai_terminal.h"
#include "fyai_terminal_view.h"
#include "fyai_tools.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"

size_t write_response(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct fyai_buf *buf = userdata;
	size_t bytes;

	bytes = size * nmemb;

	if (!bytes)
		return 0;

	if (fyai_buf_reserve(buf, buf->len + bytes + 1))
		return 0;

	memcpy(buf->data + buf->len, ptr, bytes);
	buf->len += bytes;
	buf->data[buf->len] = '\0';

	return bytes;
}

/*
 * Detect binary data the way git does: a NUL byte within the inspected
 * prefix marks the buffer as non-text. This is also exactly the condition
 * under which a strlen-based length would be truncated, so callers must use
 * the real byte length rather than strlen for any reported count.
 */
bool data_is_binary(const char *data, size_t len)
{
	size_t scan;
	size_t i;

	scan = len < 8192 ? len : 8192;

	for (i = 0; i < scan; i++) {
		if (!data[i])
			return true;
	}
	return false;
}

size_t fyai_utf8_length(const char *s)
{
	size_t len = 0;

	/* Each byte that is not a continuation byte starts a code point. */
	for (; *s; s++) {
		if (((unsigned char)*s & 0xc0) != 0x80)
			len++;
	}
	return len;
}

ssize_t fyai_utf8_scan(const unsigned char *p, size_t n)
{
	unsigned char lo = 0x80, hi = 0xbf;
	size_t len, i;

	if (p[0] < 0x80)
		return 1;
	if (p[0] >= 0xc2 && p[0] <= 0xdf)
		len = 2;
	else if (p[0] >= 0xe0 && p[0] <= 0xef)
		len = 3;
	else if (p[0] >= 0xf0 && p[0] <= 0xf4)
		len = 4;
	else
		return 0;
	/* The second byte excludes overlong forms, surrogates and > U+10FFFF. */
	if (p[0] == 0xe0)
		lo = 0xa0;
	else if (p[0] == 0xed)
		hi = 0x9f;
	else if (p[0] == 0xf0)
		lo = 0x90;
	else if (p[0] == 0xf4)
		hi = 0x8f;
	for (i = 1; i < len && i < n; i++) {
		if (p[i] < lo || p[i] > hi)
			return 0;
		lo = 0x80;
		hi = 0xbf;
	}
	return n < len ? -1 : (ssize_t)len;
}

double fyai_monotonic_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

bool data_is_wire_text(const char *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	ssize_t n;
	size_t i;

	for (i = 0; i < len; i += (size_t)n) {
		n = fyai_utf8_scan(p + i, len - i);
		if (n <= 0)
			return false;
		if (n == 1 && p[i] < 0x20 && p[i] != '\t' && p[i] != '\n' &&
		    p[i] != '\r')
			return false;
		if (n == 1 && p[i] == 0x7f)
			return false;
	}
	return true;
}

fy_generic fyai_bytes_to_generic(struct fy_generic_builder *gb,
				 const char *data, size_t len)
{
	fy_generic value;
	char *copy;

	if (!gb || !data)
		return fy_invalid;

	if (data_is_wire_text(data, len)) {
		copy = malloc(len + 1);
		if (!copy)
			return fy_invalid;
		memcpy(copy, data, len);
		copy[len] = '\0';
		value = fy_mapping(gb, "text", fy_gb_intern_string(gb, copy));
		free(copy);
		return value;
	}

	copy = fyai_base64url_encode((const unsigned char *)data, len);
	if (!copy)
		return fy_invalid;
	value = fy_mapping(gb, "data", fy_gb_intern_string(gb, copy));
	free(copy);
	return value;
}

char *fyai_bytes_from_generic(fy_generic value, size_t *lenp)
{
	fy_generic held;
	const char *text;
	char *out;
	size_t len;

	*lenp = 0;
	held = fy_get(value, "text", fy_invalid);
	if (fy_is_string(held)) {
		text = fy_castp(&held, "");
		len = strlen(text);
		out = malloc(len + 1);
		if (!out)
			return NULL;
		memcpy(out, text, len + 1);
		*lenp = len;
		return out;
	}

	held = fy_get(value, "data", fy_invalid);
	if (!fy_is_string(held))
		return NULL;
	out = (char *)fyai_base64url_decode(fy_castp(&held, ""), lenp);
	return out;
}

int append_header(struct curl_slist **headers, const char *header)
{
	struct curl_slist *next;

	next = curl_slist_append(*headers, header);
	if (!next)
		return -1;

	*headers = next;
	return 0;
}

char *make_header(const char *prefix, const char *value)
{
	size_t len;
	char *header;

	if (!value)
		return NULL;

	len = strlen(prefix) + strlen(value) + 1;
	header = malloc(len);
	if (!header)
		return NULL;

	snprintf(header, len, "%s%s", prefix, value);
	return header;
}

char *join_args(int argc, char **argv)
{
	size_t arg_len;
	size_t len;
	char *out;
	char *p;
	int i;

	len = 0;
	for (i = 0; i < argc; i++)
		len += strlen(argv[i]) + 1;

	out = malloc(len + 1);
	if (!out)
		return NULL;

	p = out;
	for (i = 0; i < argc; i++) {
		arg_len = strlen(argv[i]);
		if (i)
			*p++ = ' ';
		memcpy(p, argv[i], arg_len);
		p += arg_len;
	}
	*p = '\0';

	return out;
}

char *read_text_file_limited(const char *path, size_t max_bytes, size_t *fullp)
{
	struct fyai_buf buf = {};
	char drain[4096];
	size_t nread, want, total;
	char *msg;
	FILE *fp;

	if (fullp)
		*fullp = 0;

	fp = fopen(path, "rb");
	if (!fp)
		goto err;

	/* Keep the bounded prefix and count all bytes from the stream. */
	total = 0;
	do {
		want = sizeof(drain);
		if (max_bytes && buf.len + want > max_bytes)
			want = max_bytes - buf.len;
		if (!want)
			break;
		if (fyai_buf_reserve(&buf, buf.len + want + 1))
			goto err_close;
		nread = fread(buf.data + buf.len, 1, want, fp);
		buf.len += nread;
		total += nread;
	} while (nread == want);

	while ((nread = fread(drain, 1, sizeof(drain), fp)) > 0)
		total += nread;

	if (ferror(fp))
		goto err_close;

	fclose(fp);

	if (fullp)
		*fullp = total;

	if (data_is_binary(buf.data, buf.len)) {
		msg = malloc(64);
		if (msg)
			snprintf(msg, 64, "binary file: %zu bytes", total);
		free(buf.data);
		return msg;
	}

	if (fyai_buf_reserve(&buf, buf.len + 1))
		goto err;
	buf.data[buf.len] = '\0';
	return buf.data;

err_close:
	fclose(fp);
err:
	free(buf.data);
	return NULL;
}

char *read_text_file_window(const char *path, long long offset,
			    long long offset_bytes, long long limit,
			    size_t max_bytes,
			    struct read_text_info *info)
{
	struct read_text_info local = {};
	struct fyai_buf buf = {};
	long long lineno, collected;
	char *line = NULL;
	size_t cap = 0;
	size_t take;
	size_t skip;
	size_t line_start;
	ssize_t len;
	char *msg;
	FILE *fp;

	if (offset < 1)
		offset = 1;
	lineno = 0;
	collected = 0;

	fp = fopen(path, "rb");
	if (!fp)
		goto err;

	/* Collect the window and count the complete input in one pass. */
	while ((len = getline(&line, &cap, fp)) > 0) {
		line_start = local.full_bytes;
		lineno++;
		local.full_bytes += (size_t)len;
		if (local.byte_capped)
			continue;
		skip = 0;
		if (offset_bytes >= 0) {
			if ((size_t)offset_bytes >= local.full_bytes)
				continue;
			if ((size_t)offset_bytes > line_start)
				skip = (size_t)offset_bytes - line_start;
		} else if (lineno < offset) {
			continue;
		}
		if (limit > 0 && collected >= limit)
			continue;

		take = (size_t)len - skip;
		if (max_bytes && buf.len + take > max_bytes) {
			take = max_bytes > buf.len ? max_bytes - buf.len : 0;
			local.byte_capped = true;
		}
		if (take) {
			if (fyai_buf_reserve(&buf, buf.len + take + 1))
				goto err_close;
			if (!buf.len)
				local.first_byte = line_start + skip;
			memcpy(buf.data + buf.len, line + skip, take);
			buf.len += take;
			local.next_byte = line_start + skip + take;
			if (!local.first_line)
				local.first_line = lineno;
			local.last_line = lineno;
		}
		if (!local.byte_capped)
			collected++;
	}

	if (ferror(fp))
		goto err_close;
	free(line);
	line = NULL;
	fclose(fp);
	local.total_lines = lineno;

	if (buf.len && data_is_binary(buf.data, buf.len)) {
		local.binary = true;
		msg = malloc(64);
		if (msg)
			snprintf(msg, 64, "binary file: %zu bytes",
				 local.full_bytes);
		free(buf.data);
		if (info)
			*info = local;
		return msg;
	}

	if (fyai_buf_reserve(&buf, buf.len + 1))
		goto err;
	buf.data[buf.len] = '\0';
	if (info)
		*info = local;
	return buf.data;

err_close:
	fclose(fp);
err:
	free(line);
	free(buf.data);
	return NULL;
}

char *read_text_file(const char *path)
{
	return read_text_file_limited(path, 0, NULL);
}

int write_text_file(const char *path, const char *content)
{
	FILE *fp = NULL;
	int rc = -1;

	fp = fopen(path, "wb");
	if (!fp)
		goto err_out;

	if (fputs(content, fp) == EOF)
		goto err_out;

	if (fclose(fp))
		goto err_out;

	fp = NULL;

	rc = 0;

out:
	if (fp)
		fclose(fp);
	return rc;
err_out:
	rc = -1;
	goto out;
}

/* Close the descriptors from @from to @to, both included. */
void fyai_close_fds_range(unsigned int from, unsigned int to)
{
	int saved = errno;
	long max_fd;
	unsigned int fd;

	if (!fyai_host_close_range(from, to)) {
		errno = saved;
		return;
	}
	max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0)
		max_fd = 1024;
	for (fd = from; fd <= to && fd < (unsigned int)max_fd; fd++)
		close((int)fd);
	errno = saved;
}

void fyai_close_fds_from(int lowfd)
{
	fyai_close_fds_range((unsigned int)lowfd, ~0U);
}

void fyai_close_fds_except(int lowfd, int keepfd)
{
	if (keepfd < lowfd) {
		fyai_close_fds_from(lowfd);
		return;
	}
	if (keepfd > lowfd)
		fyai_close_fds_range((unsigned int)lowfd, (unsigned int)keepfd - 1);
	fyai_close_fds_range((unsigned int)keepfd + 1, ~0U);
}


void emit_generic_to_sink(struct fyai_ctx *ctx, const char *label,
			    fy_generic value, bool pretty)
{
	emit_generic_to_sink_anchored(ctx, label, value, pretty, false);
}

/* Send emitted machine data through the sink without rendering it. */
void emit_generic_to_sink_anchored(struct fyai_ctx *ctx, const char *label,
				     fy_generic value,
				     bool pretty, bool auto_anchor)
{
	enum fy_op_emit_flags flags;
	fy_generic emitted;
	const char *text;
	int rc;

	if (pretty) {
		flags = FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_MODE_YAML_1_2 |
			FYOPEF_STYLE_PRETTY |
			FYOPEF_WIDTH_INF |
			FYOPEF_OUTPUT_COMMENTS |
			FYOPEF_BINARY_BASE64;
	} else {
		flags = FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_MODE_JSON |
			FYOPEF_STYLE_COMPACT |
			FYOPEF_WIDTH_INF;
	}
	if (auto_anchor)
		flags |= FYOPEF_AUTO_ANCHOR;

	if (label) {
		text = fy_sprintfa("\n# %s:\n", label);
		if (text) {
			rc = fyai_sink_write(ctx->front->sink, FYAI_SINK_MACHINE,
					     text, strlen(text));
			fyai_error_check(ctx, !rc, err,
					 "could not write the %s label", label);
		}
	}
	emitted = fy_emit(fyai_ctx_transient_gb(ctx), value, flags, NULL);
	fyai_error_check(ctx, fy_is_string(emitted), err,
			 "could not emit the %s as machine data",
			 label ? label : "value");
	text = fy_castp(&emitted, "");
	rc = fyai_sink_write(ctx->front->sink, FYAI_SINK_MACHINE, text, strlen(text));
	fyai_error_check(ctx, !rc, err, "could not write the %s",
			 label ? label : "value");
err:
	return;
}
const char *emit_request_body(struct fy_generic_builder *gb, fy_generic request)
{
	fy_generic emitted;
	const char *body;

	emitted = fy_emit(gb, request,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_MODE_JSON |
		FYOPEF_STYLE_COMPACT |
		FYOPEF_WIDTH_INF |
		FYOPEF_NO_ENDING_NEWLINE,
		NULL);

	if (fy_is_invalid(emitted))
		return NULL;

	body = fy_castp(&emitted, "");
	if (!*body)
		return NULL;

	return fy_gb_intern_string(gb, body);
}

/* Preserve parser diagnostics on the returned value. */
fy_generic parse_response(struct fy_generic_builder *gb, const char *response)
{
	return parse_json_string(gb, response);
}

/* Format the first collected parser diagnostic without its input address. */
static bool parse_diag_text(fy_generic v, char *buf, size_t size)
{
	fy_generic diag, rec, gmsg, gcontent;
	const char *msg, *content, *excerpt;
	long long line, column;
	size_t len;

	diag = fy_generic_get_diag(v);
	if (!fy_is_valid(diag) || fy_is_null(diag) || !fy_len(diag))
		return false;
	/* The first record is the cause; the rest follow from it. */
	rec = fy_is_sequence(diag) ? fy_get_at(diag, 0) : diag;
	gmsg = fy_get(rec, "message");
	msg = fy_is_string(gmsg) ? fy_castp(&gmsg, "") : "malformed JSON";
	line = fy_get(rec, "line", 0LL);
	column = fy_get(rec, "column", 0LL);
	gcontent = fy_get(rec, "content");
	content = fy_is_string(gcontent) ? fy_castp(&gcontent, "") : "";
	len = strlen(content);
	excerpt = len > 76 ? content + len - 76 : content;
	snprintf(buf, size, "%s at line %lld column %lld%s%s%s", msg, line,
		 column, *excerpt ? ": " : "", len > 76 ? "..." : "",
		 excerpt);
	return true;
}

void parse_diag_report(struct fyai_ctx *ctx, fy_generic v,
		       const char *fallback, const char *what)
{
	char why[512];
	bool have_why;

	have_why = parse_diag_text(v, why, sizeof(why));
	fyai_error(ctx, "%s: %s", what, have_why ? why : fallback);
}

fy_generic fyai_join_strings(struct fy_generic_builder *gb, fy_generic chunks)
{
	struct fy_generic_op_args args = {};

	args.common.items = fy_generic_sequence_get_items(chunks,
							 &args.common.count);
	return fy_generic_op_args(gb, FYGBOPF_JOIN | FYGBOPF_MAP_ITEM_COUNT,
				  fy_value(fy_szstr_empty), &args);
}

/* find out if I'm being traced */
bool self_is_traced(void)
{
	char line[256];
	bool result = false;
	int pid;
	FILE *fp;

	fp = fopen("/proc/self/status", "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			if (strncmp(line, "TracerPid:", 10) == 0) {
			    sscanf(line + 10, "%d", &pid);
			    result = pid != 0;
			    break;
			}
		}
		fclose(fp);
	}
	return result;
}

bool self_is_valgrinded(void)
{
	FILE *fp;
	char buf[1024];
	bool result = false;

	fp = fopen("/proc/self/maps", "r");
	if (fp) {
		while (fgets(buf, sizeof(buf), fp)) {
			if (strstr(buf, "vgpreload")) {
				result = true;
				break;
			}
		}
		fclose(fp);
	}
	return result;
}

bool fyai_exec_self_available(void)
{
	return fyai_host_exec_self_available();
}

int fyai_exec_self(const char *const argv[])
{
	return fyai_host_exec_self(argv);
}

/* 0 limit satisfied, 1 not satisfied after reexec, or hard limit, -1 error */
int raise_stack(size_t bytes, char **argv)
{
	size_t aligned_size;
	struct rlimit rl;
	int rc;

	/* if traced or under valgrind, bail; ulimit must be set manually then */
	if (self_is_traced() || self_is_valgrinded())
		return 0;

	/* align to page size */
	aligned_size = fy_size_t_align(bytes, (size_t)sysconf(_SC_PAGESIZE));

	rc = getrlimit(RLIMIT_STACK, &rl);
	if (rc)
		return -1;

	/*
	fprintf(stderr, "stack_req = 0x%zx\n", aligned_size);
	fprintf(stderr, "stack_cur = 0x%zx\n", (size_t)rl.rlim_cur);
	fprintf(stderr, "stack_max = 0x%zx\n", (size_t)rl.rlim_max);
	*/

	/* current limit OK? good to go? */
	if (rl.rlim_cur >= aligned_size)
		return 0;

	/* if we have already raised the limit and it doesn't work bail */
	if (getenv("FYAI_STACK_LIMIT_RAISED"))
		return 1;

	/* we can never satisfy the limit */
	if (rl.rlim_max < aligned_size && rl.rlim_max != RLIM_INFINITY)
		return 1;

	/* change it */
	rl.rlim_cur = aligned_size;
	rc = setrlimit(RLIMIT_STACK, &rl);
	if (rc)
		return -1;

	/* mark that we tried */
	setenv("STACK_LIMIT_RAISED", "1", 1);

	/* and reexec ourselves */
	fyai_exec_self((const char *const *)argv);

	/* should never ever happen */
	perror("execv");
	exit(127);
}

/* Slurp all of stdin into a NUL-terminated malloc'd buffer (for "-" prompts,
 * where stdin may be a non-seekable pipe). Returns NULL on error. */
char *read_all_stdin(void)
{
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	char *grown;
	ssize_t r;

	if (!buf)
		return NULL;
	for (;;) {
		if (len + 1 >= cap) {
			grown = realloc(buf, cap *= 2);

			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
		r = read(STDIN_FILENO, buf + len, cap - len - 1);
		if (r < 0) {
			free(buf);
			return NULL;
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	buf[len] = '\0';
	return buf;
}

int str_in_set(const char *v, const char *const *opts)
{
	int i;

	for (i = 0; *opts; opts++, i++)
		if (!strcmp(v, *opts))
			return i;
	return -1;
}

bool is_mode_file(const char *path, int mode)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	       access(path, mode) == 0;
}

bool is_mode_directory(const char *path, int mode)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
	       access(path, mode) == 0;
}

bool is_readable_file(const char *path)
{
	return is_mode_file(path, R_OK);
}

bool is_writable_directory(const char *path)
{
	return is_mode_directory(path, W_OK);
}

/* Collect parser diagnostics on the failed value instead of standard error. */
fy_generic parse_json_string(struct fy_generic_builder *gb, const char *str)
{
	return fy_parse(gb, str,
		FYOPPF_DISABLE_DIRECTORY |
		FYOPPF_INPUT_TYPE_STRING |
		FYOPPF_MODE_JSON |
		FYOPPF_COLLECT_DIAG,
		NULL);
}

fy_generic parse_json_string_size(struct fy_generic_builder *gb,
				  const char *str, size_t len)
{
	const fy_generic_sized_string szstr = {
		.data = str,
		.size = len,
	};

	return fy_parse(gb, szstr,
		FYOPPF_DISABLE_DIRECTORY |
		FYOPPF_INPUT_TYPE_STRING |
		FYOPPF_MODE_JSON |
		FYOPPF_COLLECT_DIAG,
		NULL);
}

const char *emit_json_string(struct fy_generic_builder *gb, fy_generic v)
{
	fy_generic emitted;
	const char *body;

	emitted = fy_emit(gb, v,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_MODE_JSON |
		FYOPEF_STYLE_COMPACT |
		FYOPEF_WIDTH_INF |
		FYOPEF_NO_ENDING_NEWLINE,
		NULL);

	if (fy_is_invalid(emitted))
		return NULL;

	/* intern while `emitted` is still in scope: a short result lives
	 * inline in the generic word itself (see CLAUDE.md on fy_cast) */
	body = fy_castp(&emitted, "");
	return fy_gb_intern_string(gb, body);
}

/*
 * Stack-lifetime verification for out-of-place generics (Linux only).
 *
 * A builder-less scratch generic lives in the stack frame that created it;
 * returning one up the call chain leaves its backing pointer in an exited
 * frame. On a downward-growing stack an exited frame lies strictly below
 * every live local of the surviving callers, so: dead iff the backing
 * pointer is inside the thread stack region and below @live_floor - the
 * caller's stack pointer, captured at the call site by
 * generic_in_dead_stack_frame().
 *
 * Stack bounds come from the [stack] line of /proc/self/maps, cached after
 * the first read and refreshed only when a candidate falls below the cached
 * low bound (the region grows downward). fyai is a single-threaded,
 * one-invocation process, so the main-thread region is the only one that
 * matters.
 */
static uintptr_t fyai_stack_lo, fyai_stack_hi;

static bool fyai_stack_bounds_read(void)
{
	return fyai_host_stack_bounds(&fyai_stack_lo, &fyai_stack_hi);
}

bool generic_ptr_in_dead_stack(fy_generic v, const void *live_floor)
{
	uintptr_t ptr;

	if (fy_is_invalid(v) || fy_generic_is_in_place(v))
		return false;

	if (fy_is_collection(v))
		ptr = (uintptr_t)fy_generic_resolve_collection_ptr(v);
	else
		ptr = (uintptr_t)fy_generic_resolve_ptr(v);
	if (!ptr)
		return false;

	if (!fyai_stack_hi && !fyai_stack_bounds_read())
		return false;

	/*
	 * The stack region grows downward after the bounds were cached; a
	 * candidate below the cached low bound may sit in newly mapped
	 * stack. Re-read the bounds only then - a genuine heap/arena/mmap
	 * pointer stays outside [stack] no matter how often we look.
	 */
	if (ptr < fyai_stack_lo && ptr < fyai_stack_hi)
		(void)fyai_stack_bounds_read();

	return ptr >= fyai_stack_lo && ptr < fyai_stack_hi &&
	       ptr < (uintptr_t)live_floor;
}

int mkdir_private(const char *path)
{
	struct stat st;

	if (!mkdir(path, 0700))
		return 0;
	if (errno != EEXIST || lstat(path, &st) || !S_ISDIR(st.st_mode))
		return -1;
	if ((st.st_mode & 077) && chmod(path, 0700))
		return -1;
	return 0;
}

int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int hex_decode(const char *text, unsigned char *bytes, size_t *count)
{
	size_t length = strlen(text), i;
	int high, low;

	if (length & 1) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < length / 2; i++) {
		high = hex_nibble(text[i * 2]);
		low = hex_nibble(text[i * 2 + 1]);
		if (high < 0 || low < 0) {
			errno = EINVAL;
			return -1;
		}
		bytes[i] = (unsigned char)(high << 4 | low);
	}
	*count = length / 2;
	return 0;
}

int fyai_shell_quote(struct fyai_buf *out, const char *s)
{
	int rc = fyai_buf_append(out, "'");

	for (; !rc && *s; s++)
		rc = *s == '\'' ? fyai_buf_append(out, "'\\''") :
				  fyai_buf_append_data(out, s, 1);
	return rc ? rc : fyai_buf_append(out, "'");
}

bool fyai_path_beneath(const char *path, const char *root)
{
	size_t length = strlen(root);

	while (length > 1 && root[length - 1] == '/')
		length--;
	if (length == 1 && root[0] == '/')
		return path[0] == '/';
	return !strncmp(path, root, length) &&
	       (path[length] == '/' || path[length] == '\0');
}
