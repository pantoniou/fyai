# Schema-defined commands — Plan

Status: accepted. The decisions are listed at the end.

## Goal

A verb (`fyai branch new x`) and a slash command (`/branch new x`) often do
the same work. Each one has its own parser, its own validation, and its own
help text:

- A verb is a `struct fyai_verb` (`src/commands.h`). Its `configure()` parses
  `argc`/`argv` into `union fyai_cmd_args`. Its `execute()` runs on the
  context.
- A slash command is a `struct fyai_slash_cmd` (`src/fyai_session.c`). Its
  `run()` gets the rest of the line as one string and parses it by hand. Its
  `args` field is help text only.
- Completion exists only in the interactive session
  (`fyai_session_completion()`). No shell can complete a command line.
- Help is a set of hand-written strings: `synopsis`, `help`, and `args`.
  Nothing makes sure that they agree with the parser.

This plan defines each command one time, as a JSON Schema document. One
engine uses that definition to:

1. parse a command line, from `argv` or from a slash line, into a
   `fy_generic` argument mapping;
2. validate that mapping with `fyai_schema_validate()`;
3. call one handler that takes a generic and returns a generic;
4. present the result for the invocation that asked;
5. complete a partial command line, in the session and in a shell; and
6. render help, a usage line, a manual page, and a reference document.

The new path runs in parallel with the old tables during the migration.

## Terms

- **Command**: one executable operation, for example `branch new`. A command
  has a path of one or more words.
- **Group**: a command word with subcommands and no handler, for example
  `branch`. A group can have a default subcommand.
- **Surface**: where a command is invoked. `cli` is a verb from `argv`.
  `session` is a slash command in an interactive run.
- **Completion kind**: the name of a set of values that an argument accepts,
  for example `branch` or `model`.
- **Registry**: the set of command definitions that the binary loads.

## Definition files

The definitions are one YAML file, `data/commands.yaml`. The build embeds it
as it embeds `data/config.schema.yaml`. A handler is bound by name from a C
table. Data lets `fyai completion` and the documentation targets read the
definitions without C code.

The registry is itself checked against `data/command.schema.yaml`, the meta
schema of a command definition, at build time and in a unit test.

### Shape of a definition

```yaml
# data/commands.yaml, an item of commands:
command: branch
title: list, create, and manage branches
description: |
  A branch holds one conversation and its configuration. ...
surfaces: [cli, session]
default: list
commands:
  - command: new
    title: create a branch
    description: |
      Create a branch at a reference. The new branch takes the conversation
      and the configuration of that reference.
    handler: branch_new
    flags: [storage, publish]
    examples:
      - args: new feature main~2
        text: start a branch two turns before the head of main
    arguments:
      type: object
      required: [name]
      additionalProperties: false
      properties:
        name:
          type: string
          description: name of the new branch
          x-fyai-positional: 0
          x-fyai-meta: NAME
        ref:
          type: string
          description: start point
          default: HEAD
          x-fyai-positional: 1
          x-fyai-meta: REF
          x-fyai-complete: ref
        force:
          type: boolean
          description: replace a branch that has this name
          x-fyai-short: f
```

`arguments` is a JSON Schema (draft 2020-12 subset, see
`doc/schema-validator-plan.md`). The `x-fyai-*` keywords bind the schema to
a command line. The validator ignores them as annotations.

### Command-line binding keywords

| Keyword | Meaning |
|---|---|
| `x-fyai-positional: N` | the property takes positional word N |
| `x-fyai-rest: true` | a string takes the remaining line; an array forwards all remaining words verbatim, including options (`view enter NAME COMMAND...`) |
| `x-fyai-long: name` | long option name; the default is the property name with `_` changed to `-` |
| `x-fyai-short: c` | one-letter option |
| `x-fyai-meta: NAME` | placeholder in the usage line and in help |
| `x-fyai-complete: kind` | completion kind, see [Completion](#completion) |
| `x-fyai-repeat: true` | an `array` property that takes the option more than one time (`--set a=b --set c=d`) |
| `x-fyai-negate: true` | a `boolean` that also accepts `--no-name` |
| `x-fyai-surfaces: [cli]` | the property exists only on these surfaces |
| `x-fyai-hidden: true` | the parser accepts it; help and completion do not show it |

The parser maps from the schema type: `boolean` is a flag, `integer` and
`number` parse as numbers, `array` with `x-fyai-repeat` collects, and a
`string` with `enum` accepts only those values. A property that does not
have `x-fyai-positional` or `x-fyai-rest` is an option.

Subcommands are nested `commands`, each with its own
`arguments` schema. Do not use `oneOf` to select a subcommand: completion and
error messages cannot tell which branch of a `oneOf` the user meant.

### Group arguments

A group can have `arguments` of its own. A word after the group that names no
subcommand fills the next positional argument of the group, so
`fyai auth openai login` gives `login` the argument `provider: openai`. Each
group is checked against its own schema, its defaults apply when no word
fills it, and its arguments merge into the arguments of the command.

### Command keywords

| Keyword | Meaning |
|---|---|
| `command` | the word |
| `aliases` | other words (`rewind` for `reset`, `switch` for `resume`) |
| `title` | one line for lists and completion descriptions |
| `description` | full help text, Markdown |
| `examples` | `{args, text}` items |
| `surfaces` | `[cli]`, `[session]`, or both |
| `handler` | name of the C handler |
| `default` | subcommand of a group that runs with no word, or with an option |
| `fallback` | subcommand of a group that takes a word that names no subcommand |
| `view` | the result is a record that a session keeps in the scrollback |
| `while_busy` | `run`: a session runs it beside a turn; `bare`: only with no arguments; `wait` (the default): it waits behind the turn |
| `flags` | the needs of a verb: `model` (sends model requests, so it needs a provider credential), `no-storage`, `transient`, `interactive`. With no `model`, a verb needs no key |
| `mode` | `result` (default), `stream`, or `async`; see [Streaming and long-running commands](#streaming-and-long-running-commands) |
| `result` | optional JSON Schema of the result, used by tests and by `--output` |
| `stream` | for `mode: stream`: `item` (JSON Schema of one item) and `render` (`rows`, `markdown`, or `lines`) |
| `see_also` | command paths and help topics |

## Execution

### Handler

```c
struct fyai_cmd_call {
	struct fyai_ctx *ctx;
	const struct fyai_cmd *cmd;	/* definition, borrowed from the registry */
	enum fyai_cmd_surface surface;
	fy_generic args;		/* validated, defaults applied */
	struct fy_generic_builder *gb;	/* result storage, owned by the caller */
	void (*cancel)(struct fyai_cmd_call *call);	/* set by an async handler */
	void *priv;			/* handler state of an async call */
};

typedef int (*fyai_cmd_fn)(struct fyai_cmd_call *call, fy_generic *result);
```

- `args` holds only validated values. The engine applies each schema
  `default` before the call. A handler does not parse or check types.
- A failure raises a diagnostic in `cfg->diag` that states the cause, and
  returns a negative value. Diagnostics drain at the command boundary, as now.
- `gb` belongs to the caller. It lives until the result is presented. A
  handler does not return a generic from a stack builder.
- A handler that must act differently on one surface tests
  `call->surface`. Keep such tests few; a large difference is two commands.

### Result and presentation

A handler returns its result as a generic. The caller
presents it. A handler does not write to the sink for its result.

- A verb renders the result on `FYAI_SINK_NOTICE`: a sequence of mappings as
  a table through `fyai_generic_to_markdown()`, a string as Markdown, and a
  mapping as a key-value table. `--output json|yaml` writes the result to
  `FYAI_SINK_MACHINE` in place of the rendering. Each ported verb thus gets
  machine-readable output.
- A slash command renders the same Markdown to the notice stream. In a
  fullscreen session, the result goes to the status rows or to the popup,
  as `fyai_ui_pane_end()` does now.
- `render.table` gives the `renderopts` of the table. A handler whose table
  options depend on the result sets `call->renderopts`.
- `render.message` is a line, or a list of lines, with `{key}` taken from a
  mapping result. A line that names a key the result does not set is left
  out, so `created branch {created}` shows only when a branch was created.
- `render.document` (`yaml` or `flow`) presents the result as a YAML
  document. A verb writes it as it stands, so it can be read back; a session
  shows it as a YAML block.
- A command that is live work, for example `resume` with its picker, or
  `/zoom`, returns an empty result and does its own presentation through the
  existing sink and UI calls.

### Surface differences

Some commands differ between the surfaces:

- `checkout` moves stored `HEAD` on both surfaces, but `/checkout <ref>@{N}`
  creates a `session/` branch.
- The `resume` picker ends the invocation on Escape; the `/resume` picker
  returns to the session.
- `/zoom`, `/kill`, `/page`, and `/sessions` need a live UI.

`surfaces` states where a command exists. A `session`-only
command is not a verb, and `fyai completion` does not offer it. A command
that behaves differently tests `call->surface` in its handler. The
definition states the difference in its `description` so that help shows it.

### Streaming and long-running commands

Some commands do not have one result that is ready when the handler
returns:

- output that arrives in parts: `history`, `transcript`, `log` views, a
  long `diff`, `list reflog` on a large arena;
- work on the event loop: `auth login` (OAuth browser flow), `mcp login`,
  `compact` (a model request), `catalog update` (a child program), `btw`
  (a sub-agent);
- interactive work: the `resume` picker, the branch browser, `/edit`.

A handler therefore has one of three modes. The definition states it with
`mode`, so the dispatcher, help, and `--output` know it before the call.

**`mode: result`** (the default). The handler returns the result as
described above.

**`mode: stream`**. The handler emits items as it makes them:

```c
int fyai_cmd_emit(struct fyai_cmd_call *call, fy_generic item);
```

- The definition gives the schema of one item in `stream.item` and the
  presentation in `stream.render`: `rows` (a table), `markdown` (fragments
  of one document), or `lines` (plain text).
- The caller presents each item when it arrives. On the terminal backend,
  the items go to one sink band that the progressive renderer paints again;
  a `rows` stream renders the table again with the new row, so the column
  widths stay correct. The band commits when the stream ends. A backend
  without bands writes each item as it arrives.
- `--output json` writes one JSON document per item (JSON Lines);
  `--output yaml` writes one YAML document per item. The format is thus
  usable in a pipe while the command runs.
- `fyai_cmd_emit()` returns a negative value when the consumer stops, for
  example when the pipe closes or the user interrupts. The handler must then
  stop and return.
- A stream is also an ordinary result: a caller that wants the whole result,
  such as a test, collects the items into a sequence.

**`mode: async`**. The handler arms work on the event loop of the context
and returns `FYAI_CMD_PENDING`. The work ends it with:

```c
void fyai_cmd_done(struct fyai_cmd_call *call, int rc, fy_generic result);
```

- The call structure then belongs to the dispatcher until `fyai_cmd_done()`.
  The handler keeps `call` in its own state and does not free it. The
  argument generics stay alive until then.
- `struct fyai_cmd_call` has a `cancel` function that the handler sets. The
  dispatcher calls it for Escape, `^C`, or the end of the session. A cancel
  must remove every source that the handler registered and then call
  `fyai_cmd_done()` with the cancel status, as the event-loop rules require.
- An async handler can also emit stream items and progress. Progress goes to
  a work band through the sink, as a retry wait does now; it is not a
  result item.
- On the CLI surface, the dispatcher runs the top-level loop of the context
  until `fyai_cmd_done()`. That is the synchronous wrapper that the
  event-loop rules permit for a standalone command.
- On the session surface, the dispatcher returns to the event loop at once.
  The session does not run a nested loop. It marks the command as active,
  keeps later input queued behind it, as it does for a queued slash command,
  and presents the result when `fyai_cmd_done()` arrives.

Work that has its own presentation keeps it. A picker, the branch browser,
and an editor tile are UI; model text that `compact` receives is conversation
content and goes through the transcript document. These handlers are
`async`, present through the existing paths, and return a small result, for
example the selected branch or the new head. `--output` then writes that
small result.

### Global options

The global options (`-b`/`--branch`, `-m`, `--set`, `--config`,
`--transient`, `--root`, `--new`, ...) get one definition,
`the `global` section of `data/commands.yaml``. `src/main.c` parses them from that definition.
Completion then knows them, and help lists them one time.

## Parser

One parser serves both surfaces:

- The CLI gives `argv` after the global options.
- The session splits the line after `/` into words with the same quoting
  rules as a shell: single quotes, double quotes, and backslash. A property
  with `x-fyai-rest` takes the unsplit rest of the line, so `/btw what's
  this?` needs no quotes.

Steps:

1. Walk the command words and descend the groups. Resolve aliases. Accept a
   unique prefix only on the session surface. The CLI takes no prefix,
   because a new command can make an old script ambiguous.
2. Collect options and positional words into a mapping. `--` ends options.
3. Apply defaults, then run `fyai_schema_validate()`.
4. Report problems in one diagnostic that names the command and each
   argument, for example
   `branch new: ref: expected string`. Say the usage line after the problems.

## Completion

### Kinds

`x-fyai-complete` names a kind. A kind is a C provider in a table:

```c
struct fyai_complete_kind {
	const char *name;
	const char *title;	/* for help */
	int (*complete)(struct fyai_complete_req *req);
};
```

A provider adds candidates, each with a value and an optional description.
The first kinds:

| Kind | Source |
|---|---|
| `branch` | branch table of the root |
| `ref` | branches, then `~N`, `^`, and `@{N}` forms of the matched branch |
| `session` | `session/` branches |
| `model`, `provider` | the catalogue of the branch |
| `persona` | configured personas |
| `config-path`, `catalog-path` | walk of the schema and of the document, slash-separated |
| `theme` | `markdown_theme_selectors()` |
| `tile` | live sessions and sub-agents (session surface only) |
| `mcp-server`, `secret` | configuration |
| `file`, `dir` | delegated to the shell; a session lists the directory |
| `enum` | implied by a schema `enum`; no provider needed |

A unit test checks that every kind that a definition names has a provider.

### The completion verb

```sh
fyai completion bash|zsh|fish          # write a script to standard output
fyai __complete -- WORD...             # candidates for the last word
```

- The script calls `fyai __complete` with the words of the line. A shell
  word boundary is the shell's, so the script sends the words, not a
  string and a point.
- `__complete` writes one candidate per line to `FYAI_SINK_MACHINE`, as
  `value<TAB>description`, then a directive line (`:nospace`, `:files`,
  `:dirs`), as cobra does. zsh and fish show the description. bash drops it.
- `__complete` opens the arena read-only, as `--root` does, and makes no
  request. It must finish in less than about 50 ms on a large arena. It
  never raises a diagnostic to the terminal: a failed provider gives no
  candidates.
- The session TAB path calls the same engine with the words of the input
  line and the `session` surface. `fyai_session_completion()` then covers a
  slash command from the registry and falls back to the old code for a
  command that is not ported.

Opening the arena on each TAB is acceptable. A cache file is not: the
architecture rules permit no state outside the arena.

## Help

Help comes from the definitions only. The hand-written `synopsis`, `help`,
and `args` strings go away for each command that is ported.

- `fyai help`, `fyai help branch`, `fyai help branch new`,
  `fyai branch new --help`, `/help`, `/help branch new`, and
  `/branch new --help` all use one renderer and give the same text. The
  session form writes `/` in front of the command and leaves out
  `cli`-only arguments.
- A group lists its subcommands with their `title`. A command shows:
  the usage line, the `description`, the positional arguments, the options
  with `x-fyai-meta`, `default`, and `enum` values, the examples, and
  `see_also`.
- The usage line is generated:
  `fyai branch new [-f|--force] NAME [REF]`. Optional arguments are in
  brackets; a `rest` argument is `TEXT...`.
- The output is Markdown on `FYAI_SINK_NOTICE`, so the theme renders it, and
  a fullscreen session shows it in the popup.
- Help topics that are not commands are in `the `topics` section of `data/commands.yaml``:
  `refs` (the reference syntax), `config-paths`, `api-key`, and one topic
  for each completion kind. `fyai help refs` shows one.
- Generated documents: `fyai help --man` writes a manual page (roff), and
  `fyai help --markdown` writes a reference for `doc/`. A build target
  regenerates `doc/commands.md`, and a test fails when the file is out of
  date.
- A unit test fails when a command has no `title` or `description`, or an
  argument has no `description`.

## Migration

The registry runs in parallel with the old tables.

1. Dispatch looks up the registry first, then `fyai_find_verb()` or
   `fyai_slash_cmds[]`. A command in the registry hides the old entry with
   the same name.
2. `fyai help` and `/help` list both sets during the migration; an old
   entry keeps its old help.
3. A ported command deletes its `configure()`, its `union fyai_cmd_args`
   member, and its slash `run()` in the same patch. The handler calls the
   existing backend function.
4. When the old tables are empty, remove them, `union fyai_cmd_args`, and
   the fallbacks.

The first commands are `branch` (a group with subcommands and refs),
`checkout` (a surface difference), `model` (catalogue completion), `config`
(path and value completion), `history` (a stream), and `auth login` (async).
Together they test every part of the engine.

## Source layout

- `src/fyai_cmd.c`, `src/fyai_cmd.h`: registry, parser, dispatch, and the
  handler table.
- `src/fyai_cmd_complete.c`: completion engine, kinds, and the
  `completion` and `__complete` verbs.
- `src/fyai_cmd_help.c`: help, usage, manual page, and reference rendering.
- `src/fyai_cmd_<group>.c`: handlers, one file for each group as they move
  out of `commands.c` and `fyai_session.c`.
- `data/command.schema.yaml`: meta schema of a definition.
- `data/commands.yaml`: definitions, global options, and help topics.
- `tests/fyai_cmd_test.c`: parser, validation, and help unit tests.
- `tests/cases/completion_*.sh`: shell completion through `__complete`,
  and bash with `COMP_WORDS` when bash is present.

## Patch series

Implementation, then tests, then documentation, as `CLAUDE.md` requires:

1. `cmd: add the command definition meta schema and the registry`
2. `cmd: parse and validate a command line from a definition`
3. `cmd: dispatch registry commands from the CLI and the session`
4. `cmd: render help and usage from definitions`
5. `cmd: add completion kinds and the completion verbs`
6. `session: complete slash commands from the registry`
7. `cmd: add stream and async handler modes`
8. `cmd: port branch`, `checkout`, `model`, `config`, `history`,
   `auth login` (one patch each)
9. tests for 1–8
10. `docs: describe schema-defined commands`, and the generated reference

## Out of scope

- Commands defined by a user or by an MCP server.
- Exposing commands to the model as tools. The definitions are close to a
  tool schema, so this is a possible later step; it needs a separate policy
  for which commands a model may run.
- Localized help.

## Decisions

1. Definitions in one file, `data/commands.yaml`.
2. Nested `commands` for subcommands, each with its own `arguments` schema.
3. The handler returns a generic, and the caller presents it; `--output`
   on verbs.
4. `surfaces` on each command; `call->surface` for small differences.
5. Unique-prefix matching only in the session, never on the CLI.
6. `__complete` opens the arena read-only on each TAB. A pinned root
   (`--root`) can need special handling; not addressed in the first series.
7. Help topics are in the first series.
8. First commands: `branch`, `checkout`, `model`, and `config`. `config`
   needs completion of the configuration items: the paths from
   `data/config.schema.yaml`, and the values from each item's `enum` and
   type.
9. Three handler modes (`result`, `stream`, `async`); JSON Lines for a
   stream with `--output json`; an async command on the session queues later
   input behind it. `history` (stream) and `auth login` (async) are ported
   in the first series so that it tests both modes.
