/*
 * fyai_cmd.h - schema-defined commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * data/commands.yaml defines each command one time. One engine parses a verb
 * from argv and a slash command from a session line into a validated argument
 * mapping, calls one handler, and presents its result. The same definitions
 * give completion and help. See doc/command-schema-plan.md.
 */

#ifndef FYAI_CMD_H
#define FYAI_CMD_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_cfg;
struct fyai_ctx;
struct response_buffer;

enum fyai_cmd_surface {
	FYAI_CMD_CLI,
	FYAI_CMD_SESSION,
};

enum fyai_cmd_format {
	FYAI_CMD_OUT_MARKDOWN,
	FYAI_CMD_OUT_JSON,
	FYAI_CMD_OUT_YAML,
};

enum fyai_cmd_mode {
	FYAI_CMD_MODE_RESULT,
	FYAI_CMD_MODE_STREAM,
	FYAI_CMD_MODE_ASYNC,
};

/* An async handler returns this after it arms its work. */
#define FYAI_CMD_PENDING	1

struct fyai_cmd_call;

/*
 * A handler. @call->args holds validated arguments with the schema defaults
 * applied. On success the handler stores its result in *@result, built in
 * @call->gb, and returns 0. On failure it raises the cause and returns -1. An
 * async handler returns FYAI_CMD_PENDING and ends with fyai_cmd_done().
 */
typedef int (*fyai_cmd_fn)(struct fyai_cmd_call *call, fy_generic *result);

/*
 * A prepare hook of a verb. It runs after the arguments are validated and
 * before the context is set up, to set what setup reads from @cfg. It raises
 * the cause and returns -1 on failure.
 */
typedef int (*fyai_cmd_prepare_fn)(struct fyai_cfg *cfg, fy_generic args);

struct fyai_cmd_call {
	struct fyai_ctx *ctx;
	fy_generic def;			/* definition, registry storage */
	const char *path;		/* "branch new", in @gb */
	enum fyai_cmd_surface surface;
	enum fyai_cmd_format format;
	enum fyai_cmd_mode mode;
	fy_generic args;		/* validated, defaults applied */
	struct fy_generic_builder *gb;	/* result storage, owned by the caller */
	/* Set by an async handler; removes its sources, then calls done. */
	void (*cancel)(struct fyai_cmd_call *call);
	void *priv;			/* handler state of an async call */
	/* Consume command input before queueing or history recording. */
	void (*input)(struct fyai_cmd_call *call, const char *line);
	/* Set by an async handler; releases @priv with the call. */
	void (*cleanup)(struct fyai_cmd_call *call);
	/* Set by a handler whose table options depend on the result. */
	fy_generic renderopts;

	/* Dispatcher state. */
	fy_generic rows;		/* collected items of a rows stream */
	bool stopped;			/* the consumer of a stream went away */
	bool done;
	int rc;
	fy_generic result;
	void (*finish)(struct fyai_cmd_call *call);
};

/*
 * The parsed and validated registry, or fy_invalid when the embedded
 * definitions do not load. The value lives as long as the process.
 */
fy_generic fyai_cmd_registry(void);

/* Why fyai_cmd_registry() failed; an empty string when it did not. */
const char *fyai_cmd_registry_why(void);

/* True if the parsed command @st is the command at @path ("branch new"). */
struct fyai_cmd_state;
bool fyai_cmd_state_is(const struct fyai_cmd_state *st, const char *path);

/* True if @word is a top-level command that the CLI surface can run. */
bool fyai_cmd_is_verb(const char *word);

/* ---- the CLI surface ---------------------------------------------------- */

/*
 * Parse the verb slice @argc/@argv (argv[0] is the verb) into @cfg, before
 * the configuration loads, and run the early hook of the verb. The values
 * live in @cfg->gb. Returns 0 on success, -1 with a diagnostic raised.
 */
int fyai_cmd_early(struct fyai_cfg *cfg, int argc, char **argv);

/*
 * Parse the verb, when fyai_cmd_early() did not, after the configuration
 * loads, and run its prepare hook. Returns 0, or -1 with a diagnostic raised.
 */
int fyai_cmd_configure(struct fyai_cfg *cfg, int argc, char **argv);

/* Run the verb that fyai_cmd_configure() parsed. */
int fyai_cmd_execute(struct fyai_ctx *ctx);

/* ---- the session surface ------------------------------------------------ */

/*
 * The definition of the slash command that @line (after the '/') names, or
 * fy_invalid when the registry does not define it for the session.
 */
fy_generic fyai_cmd_session_lookup(const char *line);

/*
 * Run the slash command @line (after the '/'). Returns 0 when the command ran
 * or is pending, -1 with a diagnostic raised. *@viewp is set when the result
 * is a record the user reads, which the session commits to the scrollback.
 */
int fyai_cmd_session_run(struct fyai_ctx *ctx, const char *line, bool *viewp);

/* True if @name (@len bytes) is exactly a slash command of the registry. */
bool fyai_cmd_session_exact(const char *name, size_t len);

/* The number of slash commands of the registry that @name prefixes. */
size_t fyai_cmd_session_prefix(const char *name, size_t len);

/* True when the slash command @line ends the session. */
bool fyai_cmd_session_ends(const char *line);

/* True when the slash command @line may run while a turn is in flight. */
bool fyai_cmd_session_immediate(const char *line);

/*
 * Present a finished async command and release it. The session calls it
 * between turns; returns true when a command finished.
 */
bool fyai_cmd_session_step(struct fyai_ctx *ctx);
/* Return true when the active command consumed the line. */
bool fyai_cmd_session_input(struct fyai_ctx *ctx, const char *line);

/* Ask an active async command to stop; it completes as cancelled. */
void fyai_cmd_session_interrupt(struct fyai_ctx *ctx);

/* Cancel an active async command, and release it. */
void fyai_cmd_session_cancel(struct fyai_ctx *ctx);

/* ---- handlers ------------------------------------------------------------ */

/* Emit one stream item. Returns -1 when the consumer stops the stream. */
int fyai_cmd_emit(struct fyai_cmd_call *call, fy_generic item);

/* End an async call. @result is built in @call->gb. */
void fyai_cmd_done(struct fyai_cmd_call *call, int rc, fy_generic result);

/* A string argument, or NULL when it is absent. Borrowed from @call->args. */
const char *fyai_cmd_arg_str(struct fyai_cmd_call *call, const char *name);
bool fyai_cmd_arg_bool(struct fyai_cmd_call *call, const char *name);

/* ---- completion ---------------------------------------------------------- */

enum fyai_cmd_directive {
	FYAI_CMD_COMPLETE_NOSPACE	= 1u << 0,	/* do not add a space */
	FYAI_CMD_COMPLETE_FILES		= 1u << 1,	/* the shell lists files */
	FYAI_CMD_COMPLETE_DIRS		= 1u << 2,	/* the shell lists directories */
};

typedef void (*fyai_cmd_candidate_fn)(void *arg, const char *value,
				      const char *description);

/*
 * Complete the last word of @words (@nwords >= 1; the last word can be
 * empty). On the CLI surface words[0] is the first word after the program
 * name. @ctx can be NULL: a completion kind that needs the arena then gives
 * no candidates. Returns the directives.
 */
unsigned int fyai_cmd_complete(struct fyai_ctx *ctx,
			       enum fyai_cmd_surface surface,
			       size_t nwords, const char *const *words,
			       fyai_cmd_candidate_fn add, void *arg);

/*
 * Complete a session input line @buf that starts with '/'. Each candidate is
 * the whole line with the last word replaced.
 */
void fyai_cmd_session_complete(struct fyai_ctx *ctx, const char *buf,
			       fyai_cmd_candidate_fn add, void *arg);

/*
 * The byte offset in the session line @buf of the word that
 * fyai_cmd_session_complete() replaces, or 0 when it completes nothing.
 */
size_t fyai_cmd_session_word(const char *buf);

/*
 * Split @line into words with the quoting rules of a shell: blanks separate,
 * single quotes are literal, double quotes and a backslash escape. *@wordsp
 * and *@offsp (the start of each word in @line) are allocated; free them with
 * free(). A line that ends in a blank, or in an open quote, ends in an empty
 * word when @partial is set. Returns the word count, or -1.
 */
int fyai_cmd_split(const char *line, bool partial, char ***wordsp,
		   size_t **offsp);
void fyai_cmd_split_free(char **words, int n, size_t *offs);

/* ---- help ---------------------------------------------------------------- */

/*
 * Append the help of @nwords/@words (a command path or a topic; none for the
 * command list) to @out as Markdown. Returns 0, or -1 with a diagnostic
 * raised through @cfg.
 */
int fyai_cmd_help_source(struct fyai_cfg *cfg, enum fyai_cmd_surface surface,
			 size_t nwords, const char *const *words,
			 struct response_buffer *out);

/* The whole reference as Markdown, and as the manual page fyai(1). */
int fyai_cmd_reference_markdown(struct response_buffer *out);
int fyai_cmd_reference_man(struct response_buffer *out);

/* The usage line of @def at @path on @surface, allocated. */
char *fyai_cmd_usage(fy_generic def, const char *path,
		     enum fyai_cmd_surface surface);

/* ---- internal interfaces of the command sources -------------------------- */

/* The handler named @name, or NULL. */
fyai_cmd_fn fyai_cmd_handler(const char *name);

/* True if the completion kind @name has a provider. */
bool fyai_cmd_kind_known(const char *name);

/*
 * The words that name the commands and topics, for the help-topic kind. After
 * the @npath words of @path, the subcommands of the group that they name.
 */
void fyai_cmd_complete_help_topics(const char *const *path, size_t npath,
				   const char *partial,
				   fyai_cmd_candidate_fn add, void *arg);

/* Handlers. */
int fyai_cmd_view_create(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_update(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_mount(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_unmount(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_show(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_list(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_enter(struct fyai_cmd_call *call, fy_generic *result);

int fyai_cmd_help(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_completion(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_complete_verb(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_list(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_new(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_delete(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_rename(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_show(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_describe(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_checkout(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_model(struct fyai_cmd_call *call, fy_generic *result);

#endif
