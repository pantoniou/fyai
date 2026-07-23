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
#include <libfyaml/libfyaml-generic.h>

#define assert_valid_generic(_v, _msg) \
	({ \
		fy_generic __v = (_v); \
		const char *__msg = (_msg); \
		\
		if (!fy_generic_is_valid(__v)) { \
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

#if defined(__linux__) && defined(__has_builtin)
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

/* utility methods */
struct response_buffer {
	char *data;
	size_t len;
	size_t cap;
};

struct shell_command_result {
	char *stdout_data;
	char *stderr_data;
	size_t stdout_len;
	size_t stderr_len;
	int exit_code;
	int signal;
	bool signaled;
};

enum shell_output_stream {
	SHELL_OUTPUT_STDOUT,
	SHELL_OUTPUT_STDERR,
};

typedef void (*shell_output_fn)(void *userdata, enum shell_output_stream stream,
				const char *data, size_t len);

bool data_is_binary(const char *data, size_t len);
int response_buffer_reserve(struct response_buffer *buf, size_t need);
int response_buffer_append(struct response_buffer *buf, const char *text);
size_t write_response(void *ptr, size_t size, size_t nmemb, void *userdata);
int append_header(struct curl_slist **headers, const char *header);
char *make_header(const char *prefix, const char *value);
char *join_args(int argc, char **argv);
char *read_text_file(const char *path);
int write_text_file(const char *path, const char *content);
struct fyai_sandbox_spec;
struct fyai_ctx;
/* @ctx is the diagnostic sink for the capture loop; it must not be NULL. */
int run_shell_command_capture_cb(struct fyai_ctx *ctx, const char *command,
				 struct shell_command_result *result,
				 shell_output_fn output_fn,
				 void *userdata,
				 const struct fyai_sandbox_spec *sandbox);
void shell_command_result_cleanup(struct shell_command_result *result);
void emit_generic_to_stdout(const char *label, fy_generic value, bool pretty);
void emit_generic_to_stdout_anchored(const char *label, fy_generic value,
				     bool pretty, bool auto_anchor);
fy_generic make_tools(struct fy_generic_builder *gb);
const char *emit_request_body(struct fy_generic_builder *gb,
			      fy_generic request);
fy_generic parse_response(struct fy_generic_builder *gb, const char *response);
fy_generic response_content(struct fy_generic_builder *gb, fy_generic doc);
fy_generic response_message(struct fy_generic_builder *gb, fy_generic doc);
fy_generic response_tool_calls(struct fy_generic_builder *gb, fy_generic doc);
fy_generic fyai_join_strings(struct fy_generic_builder *gb, fy_generic chunks);

/* Run $VISUAL/$EDITOR (else vi) on @path. The child restores the signal
 * mask captured in @ctx before fyai took ownership of its signals. */
int fyai_spawn_editor(struct fyai_ctx *ctx, const char *path);
int fyai_spawn_editor_readonly(struct fyai_ctx *ctx, const char *path);

bool self_is_traced(void);
int raise_stack(size_t bytes, char **argv);

const char *find_cli_option(int argc, char **argv, const char *long_opt, char short_opt);
bool has_cli_flag(int argc, char **argv, const char *long_opt);
char *read_all_stdin(void);
void usage_print_option(FILE *fp, bool color, const char *opt);
void usage_item(FILE *fp, bool color, const char *opt, const char *desc);

/* >= 0 index in set, -1 not found */
int str_in_set(const char *v, const char *const *opts);

bool executable_in_path(const char *name);

bool is_mode_file(const char *path, int mode);
bool is_mode_directory(const char *path, int mode);

bool is_readable_file(const char *path);
bool is_writable_file(const char *path);
bool is_executable_file(const char *path);
bool is_readable_directory(const char *path);
bool is_writable_directory(const char *path);

fy_generic parse_json_string(struct fy_generic_builder *gb, const char *str);
fy_generic parse_json_string_size(struct fy_generic_builder *gb,
				  const char *str, size_t len);

fy_generic parse_json_generic(struct fy_generic_builder *gb, fy_generic v);
const char *emit_json_string(struct fy_generic_builder *gb, fy_generic v);

int mkdir_private(const char *path);

#endif
