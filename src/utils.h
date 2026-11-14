/*
 * utils.h - fyai utilities
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>

#include <curl/curl.h>
#include <libfyaml.h>
#include <libfyaml/libfyaml-util.h>

#include "fyai_buf.h"
#include <libfyaml/libfyaml-generic.h>

#define assert_valid_generic(_v, _msg) \
	({ \
		fy_generic __v = (_v); \
		const char *__msg = (_msg); \
		\
		if (!fy_is_valid(__v)) { \
			if (!__msg) \
				__msg = "internal error or out of memory"; \
			fprintf(stderr, "invalid generic: %s\n", __msg); \
			assert(0); \
			abort(); \
		} \
		assert(!generic_in_dead_stack_frame(__v)); \
		__v; \
	})

/*
 * Detect the classic out-of-place-generic lifetime bug: a builder-less
 * scratch generic (fy_mapping()/fy_sequence()/fy_value() without a gb) lives
 * in the stack frame of the function that created it. If such a generic is
 * *returned*, its backing storage sits in an exited frame - below the
 * caller's frame on a downward-growing stack - and dangles. Release builds
 * often "work" until the stack is reused; ASAN/Debug usually hide it.
 *
 * generic_in_dead_stack_frame(v) evaluates the check at the call site: true
 * when v's backing pointer lands in the thread stack strictly below the
 * caller's stack pointer - i.e. in frames that have already returned. The
 * floor must be the SP, not a named local: scratch generics are
 * alloca-backed, and the calling frame's own live allocas sit below its
 * locals but above its SP. Linux only (stack bounds from /proc/self/maps,
 * main thread); elsewhere always false.
 */
bool generic_ptr_in_dead_stack(fy_generic v, const void *live_floor);

#if defined(__has_builtin)
#if __has_builtin(__builtin_stack_address)
#define generic_in_dead_stack_frame(_v) \
	generic_ptr_in_dead_stack((_v), __builtin_stack_address())
#elif __has_builtin(__builtin_alloca)
/* No __builtin_stack_address (gcc < 14): a fresh one-byte alloca lands
 * within a few words of the caller's SP - below every live local and
 * alloca of this frame, above (to within call-frame overhead) anything
 * in an exited callee frame. Precise enough for a debugging aid. */
#define generic_in_dead_stack_frame(_v) \
	generic_ptr_in_dead_stack((_v), \
		({ char *__fyai_sp = __builtin_alloca(1); \
		   *__fyai_sp = 0; __fyai_sp; }))
#endif
#endif
#ifndef generic_in_dead_stack_frame
#define generic_in_dead_stack_frame(_v) \
	(((void)(_v)), false)
#endif

struct fyai_ctx;

/* utility methods */

bool data_is_binary(const char *data, size_t len);

/* True when JSON can carry @data unchanged as UTF-8 text. */
bool data_is_wire_text(const char *data, size_t len);

/*
 * Scan the UTF-8 character at @p, which has @n >= 1 bytes. Return its length,
 * 0 when the bytes are not a valid character, or -1 when the @n bytes are
 * the start of a valid character that needs more bytes.
 */
ssize_t fyai_utf8_scan(const unsigned char *p, size_t n);

/* Seconds of the monotonic clock. */
double fyai_monotonic_sec(void);

/* The number of code points of the valid UTF-8 string @s. */
size_t fyai_utf8_length(const char *s);

struct fy_generic_builder;
/* Carry bytes as plain `text`, or base64 `data` when required. */
fy_generic fyai_bytes_to_generic(struct fy_generic_builder *gb,
				 const char *data, size_t len);
/* Take the bytes back out. The caller frees the result. */
char *fyai_bytes_from_generic(fy_generic value, size_t *lenp);
size_t write_response(void *ptr, size_t size, size_t nmemb, void *userdata);
int append_header(struct curl_slist **headers, const char *header);
char *make_header(const char *prefix, const char *value);
char *join_args(int argc, char **argv);
char *read_text_file(const char *path);
/* Read at most @max_bytes bytes. Set @fullp to the complete file size. */
char *read_text_file_limited(const char *path, size_t max_bytes,
			     size_t *fullp);

/* Details about a windowed read. */
struct read_text_info {
	size_t full_bytes;	/* complete file size */
	long long total_lines;	/* complete line count */
	long long first_line;	/* 1-based first line returned, 0 if none */
	long long last_line;	/* 1-based last line returned, 0 if none */
	size_t first_byte;	/* zero-based first byte returned */
	size_t next_byte;	/* zero-based byte after returned data */
	bool byte_capped;	/* the byte limit stopped the read */
	bool binary;		/* the report replaces the content */
};

/* Read a byte-bounded line window. A nonnegative @offset_bytes overrides
 * @offset. Zero values for @limit and @max_bytes remove those limits. */
char *read_text_file_window(const char *path, long long offset,
			    long long offset_bytes, long long limit,
			    size_t max_bytes,
			    struct read_text_info *info);
int write_text_file(const char *path, const char *content);
/* Close the descriptors from @from to @to, both included. */
void fyai_close_fds_range(unsigned int from, unsigned int to);
/* Close every descriptor from @lowfd upward. */
void fyai_close_fds_from(int lowfd);
/* Close descriptors from lowfd, except keepfd. A negative keepfd keeps none. */
void fyai_close_fds_except(int lowfd, int keepfd);

void emit_generic_to_sink(struct fyai_ctx *ctx, const char *label,
			    fy_generic value, bool pretty);
void emit_generic_to_sink_anchored(struct fyai_ctx *ctx, const char *label,
				     fy_generic value,
				     bool pretty, bool auto_anchor);
const char *emit_request_body(struct fy_generic_builder *gb,
			      fy_generic request);
fy_generic parse_response(struct fy_generic_builder *gb, const char *response);
void parse_diag_report(struct fyai_ctx *ctx, fy_generic v,
		       const char *fallback, const char *what);
fy_generic fyai_join_strings(struct fy_generic_builder *gb, fy_generic chunks);

bool self_is_traced(void);
bool self_is_valgrinded(void);
bool fyai_exec_self_available(void);
int fyai_exec_self(const char *const argv[]);
int raise_stack(size_t bytes, char **argv);

char *read_all_stdin(void);

/* >= 0 index in set, -1 not found */
int str_in_set(const char *v, const char *const *opts);

bool is_mode_file(const char *path, int mode);
bool is_mode_directory(const char *path, int mode);

bool is_readable_file(const char *path);
bool is_writable_file(const char *path);
bool is_writable_directory(const char *path);

fy_generic parse_json_string(struct fy_generic_builder *gb, const char *str);
fy_generic parse_json_string_size(struct fy_generic_builder *gb,
				  const char *str, size_t len);

const char *emit_json_string(struct fy_generic_builder *gb, fy_generic v);

int mkdir_private(const char *path);

/* The value of a lowercase hex digit, or -1. */
int hex_nibble(char c);

/*
 * Decode lowercase hex text into bytes and store the count, which is half the
 * length of the text. Return 0, or -1 with errno set to EINVAL for an odd
 * length or a character that is not a lowercase hex digit.
 */
int hex_decode(const char *text, unsigned char *bytes, size_t *count);

/* Append @s to @out in single quotes, so that a shell reads it as one word. */
int fyai_shell_quote(struct fyai_buf *out, const char *s);
/* True when @path is @root or a path under it. A '/' at the end of @root is ignored. */
bool fyai_path_beneath(const char *path, const char *root);

#endif
