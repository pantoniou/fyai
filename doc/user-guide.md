# fyai User Guide

This guide describes the current user-facing behaviour of `fyai`. It is organized around the way people use the program: getting started, understanding durable state, working interactively, configuring providers and tools, securing execution, and consulting the command reference.

The implementation and tests are authoritative. Use `fyai help VERB` for
detailed CLI syntax, `/help` for the interactive command list, and `fyai config
schema` or `fyai config describe` for the configuration nodes covered by the
schema. The schema is intentionally permissive rather than a closed vocabulary;
`config.yaml.sample` remains the annotated catalogue of supported keys.

## 1. Getting started

`fyai` is a Unix-style AI coding agent. A normal invocation starts a process, discovers or opens the repository arena, loads the selected branch and its configuration, runs the requested command or agent loop, publishes any durable state, prints the result, and exits.

There is no required daemon or resident agent service. Interactive mode keeps one process alive for convenience, but it uses the same durable state model as batch invocations.

### Build

```sh
cmake -S . -B build
cmake --build build
```

A mostly-static build links fyai's dependencies statically while leaving the host C library dynamic:

```sh
cmake -S . -B build-static -DFYAI_MOSTLY_STATIC=ON
cmake --build build-static
```

A self-contained musl binary can be built with:

```sh
MODE=musl scripts/build-static-docker.sh ./fyai
```

### Initialize a project

```sh
fyai init
```

This creates a repository-local `.fyai` arena. Later invocations search upward from the current directory for the nearest `.fyai` directory unless `arena_dir` or another explicit selector is used.

To initialize from an explicit configuration document:

```sh
fyai init path/to/config.yaml
```

### Configure credentials

The default API-key policy is indirect:

```yaml
api_key: { type: auto }
```

After the provider is resolved, automatic lookup tries:

1. `--api-key` / `-k`;
2. the provider's conventional environment variable, such as `OPENAI_API_KEY`;
3. the logical secret `api-key/<provider>`.

Raw API keys are not valid persisted configuration values.

On Linux, a logical key may be placed in the kernel-backed secret store:

```sh
fyai secret set api-key/openai
fyai secret status api-key/openai
fyai secret delete api-key/openai
```

For OpenAI Responses requests, an eligible ChatGPT subscription may be used instead:

```sh
fyai auth openai login
fyai auth openai login --device-code
fyai auth openai status
fyai auth openai usage
fyai config set auth chatgpt
```

`auth: auto` prefers an available API key and otherwise falls back to a saved ChatGPT login. `auth: api-key` disables that fallback. `auth: chatgpt` selects subscription access explicitly.

### Run the first task

```sh
fyai "inspect this repository and suggest the next cleanup"
```

A prompt invocation may call the model several times and use tools until it reaches a final answer, the configured iteration limit, an interrupt, or an unrecoverable error.

Run with no prompt, or pass `-i`, to enter interactive mode:

```sh
fyai
fyai -i
```

Continue later by invoking `fyai` again from the project. The selected branch, canonical conversation, and branch configuration are loaded from the arena.

## 2. The mental model

### The process is temporary; the arena is durable

The running process is not the source of truth. Durable state is stored as immutable values in a libfyaml content-addressed arena. Publication replaces the arena root atomically; unchanged values are shared structurally.

This gives batch commands, scripts, interactive sessions, and delegated agents the same persistence model.

### Canonical content is provider-independent

`fyai` supports three API grammars:

- OpenAI-style Responses;
- OpenAI-style Chat Completions;
- Anthropic-style Messages.

Provider request and response details may be retained for inspection, but the durable conversation is represented canonically. A continuation can therefore be resolved through another provider or API grammar without making the provider's wire transcript the permanent conversation format.

Capabilities still vary by provider and model. Model resolution determines the provider, endpoint, API grammar, wire model identifier, context window, token limit, and relevant feature support.

### A branch is a line of agent work

Each branch owns:

- its conversation head;
- its stored configuration;
- creation and descriptive metadata;
- agent metadata where the branch belongs to a sub-agent;
- a branch-local reflog.

The arena-wide model catalogue is shared, but user intent and conversation history are branch-local.

At the start of an invocation, the current branch is selected in this order:

1. `--branch NAME` / `-b NAME`;
2. `FYAI_BRANCH`;
3. arena `HEAD`;
4. the default branch.

A one-shot `--branch` selection does not move arena `HEAD`. `checkout` does.
Within an interactive invocation, `/branch NAME` changes the selected branch
and moves `HEAD`.

### Turns, exchanges, and steps

A provider request may produce more than one model/tool step before a logical assistant answer is complete. The stored turn records canonical messages, provider observations, metadata, and normalized usage.

An exchange groups a user request with the assistant and tool activity that answers it. Branch merge and rebase operate on exchanges so a question is not separated from its answer.

```sh
fyai list turns
fyai list exchanges
fyai transcript
fyai dump anchors
```

## 3. Core workflows

### Inspect durable history

```sh
fyai transcript
fyai transcript --last 20
fyai history --range 10,30
```

`history` and `display` are aliases for `transcript`. The transcript is a human-facing rendering, not a serialization format.

For canonical inspection:

```sh
fyai dump state
fyai dump anchors
fyai dump providers
```

### Start over or compact

```sh
fyai clear
```

`clear` publishes a null conversation head on the selected branch. The branch reflog records the operation.

```sh
fyai compact
fyai compact "preserve the deployment decisions and unresolved test failures"
```

Compaction performs one tool-disabled summary request, starts a fresh canonical chain, and records the previous head as provenance.

For a one-invocation experiment that must not be published:

```sh
fyai --transient "try a different approach"
```

### Explore alternatives on branches

```sh
fyai branch create experiment
fyai checkout experiment
fyai "try the alternative design"
```

Or create and switch in one command:

```sh
fyai checkout -b experiment
```

Create a branch from a symbolic historical point:

```sh
fyai branch create keep main@{3}
fyai checkout -b retry main~4
```

Branch names are path-like:

```text
main
main/parser
main/parser/review
```

Configuration follows the branch. Different branches may deliberately use different models, API grammars, reasoning settings, and tool policies.

### Inspect and recover branch movement

```sh
fyai branch
fyai branch show
fyai list reflog
```

Move the selected branch's conversation head:

```sh
fyai reset HEAD~2
fyai reset main@{1}
```

A reset does not immediately discard the displaced state. The old branch entry remains available through the reflog until garbage collection removes entries outside the retained window.

```sh
fyai branch rename old new
fyai branch describe new "alternative provider and tool policy"
fyai branch delete new
fyai branch delete new --force
```

### Join branches

Rebase places the other branch's exchanges first, then the current branch's own exchanges:

```sh
fyai rebase other
```

Merge interleaves exchanges according to their recorded times:

```sh
fyai merge other
```

Both require a common conversation point unless `--allow-unrelated` is supplied. Joining conversations is an ordering operation rather than a textual file merge.

### Pin an exact state for automation

```sh
root=$(fyai root print)
fyai --root "$root" branch
fyai --root "$root" -b main dump state
```

A pinned invocation is read-only. Commands that would publish state are refused rather than silently discarded.

```sh
historical_root=$(fyai root print main@{3})
fyai --root "$historical_root" transcript
```

Root handles are local to one arena and remain valid only while the corresponding roots are retained. Garbage collection may invalidate old handles.
`--root` also accepts a symbolic reference such as `main@{3}`, but that
reference is resolved afresh on every invocation and moves as the reflog grows.
Capture the emitted root handle, as above, when automation must see the same
state repeatedly.

## 4. Models, providers, and authentication

### Model selection

```sh
fyai -m gpt-5.4-mini "review this patch"
fyai config set model openrouter/some-model
```

A plain model name selects its canonical catalogue offering. `provider/model` pins a provider offering.

```sh
fyai list providers
fyai list models
fyai catalog
fyai context
```

Interactive equivalents include:

```text
/model
/model gpt-5.4-mini
/status
/context
```

Changing the model re-resolves provider, endpoint, API grammar, wire model identifier, context window, token limit, and credentials where appropriate.

`fyai context` reports the projected fill of the next request. The same
projection is enforced: a prompt that cannot fit the model's context window is
refused before it reaches the provider, and the output allowance is reduced to
the room the prompt leaves. Compact or clear the conversation to recover.
Tool results are bounded for the same reason, so one large file or one chatty
command cannot fill the window: `read/max_bytes` bounds a `read_file` result
(which also takes `offset` and `limit` to read a window of lines), and
`shell/max_output_tokens` bounds shell output. Use a reported `offset_bytes`
to continue after a byte-limited read. See `doc/context-compaction.md`.
`-m` selects the model for one invocation. `/model NAME` persists the selection
on the current branch, as does `fyai config set model NAME`.

### API grammar

```sh
fyai api
fyai api responses
fyai api chat-completions
fyai api messages
```

Interactive equivalents are `/api` and `/api MODE`.

### Local and self-hosted servers

```sh
fyai \
  --set api=chat-completions \
  --set api_url=http://localhost:11434/v1/chat/completions \
  --set no_auth=true \
  -m llama3 \
  "inspect this tree"
```

### Authentication commands

```sh
fyai auth openai login
fyai auth openai login --device-code
fyai auth openai status
fyai auth openai info
fyai auth openai usage
fyai auth openai usage --json
fyai auth openai logout
```

Interactive shortcuts:

```text
/auth
/auth status
/auth login
/auth logout
/usage
```

Subscription tokens are machine-local credentials and are not written to the repository arena.

## 5. Configuration

### Sources and precedence

There is one persisted repository configuration: the selected branch's arena configuration.

The user file at `$XDG_CONFIG_HOME/fyai/config.yaml`, or `~/.config/fyai/config.yaml`, is bootstrap input only when the arena has no stored configuration.

```text
stored branch config (or user bootstrap)
    -> --config FILE
    -> command-line configuration overlays, in argument order
    -> built-in defaults for absent keys
```

`--config`, `-m`, `--sandbox`, `--color`, and `--theme` affect only the current
invocation. Global `--set` and `--delete` operations also participate in that
ordered overlay, but are persisted to the selected branch unless `--transient`
is active. `--api-key` is a separate invocation-only credential override.

### Inspect and edit configuration

```sh
fyai config show
fyai config effective
fyai config get model
fyai config export
fyai config set reasoning/effort high
fyai config set display/tool_detail full
fyai config delete reasoning/summary
fyai config edit
fyai config import path/to/config.yaml
```

Slash paths address nested keys. Values are parsed as YAML values rather than untyped strings.

Global operations apply before the selected verb or prompt:

```sh
fyai --set reasoning/effort=high --get model
fyai --delete display/prompt_top "continue the review"
```

### Validate and discover the schema

```sh
fyai config validate
fyai config schema
fyai config describe
fyai config describe sandbox/network
```

These commands describe and validate the configuration nodes covered by the
schema. The validator deliberately accepts unknown keys, and some transport
extensions are not enumerated there, so consult `config.yaml.sample` for the
annotated supported-key catalogue as well.

## 6. Tools and sub-agents

The built-in agent tool surface includes file reading, structured file writing and patching, shell execution, user questions, and delegated sub-agents where enabled.

Run one tool directly, without a model call:

```sh
fyai tool read_file '{"path":"README.md"}'
fyai tool shell '{"command":"git status --short"}'
```

### A shell command on a terminal

`shell` takes `tty: true`. The command then runs on a pseudo-terminal. The
result is the screen that libfyvterm interprets from the byte stream, with the
lines that scrolled off it in front. Use it for a program that behaves
differently without a terminal.

```sh
fyai tool shell '{"command":"git log --oneline -5","tty":true}'
```

The screen has the size of the terminal of the user. It follows that terminal
when the window changes size, because the parent watches `SIGWINCH` and gives
the new size to the session. `shell/tty_rows` and `shell/tty_cols`
set a fixed size instead, and the model can ask for one with `rows` and
`cols`.

### An interactive terminal session

Give the shell call a `name` and it stays open as a session: one program on a
terminal of its own, addressed by that name.

```sh
fyai tool shell '{"name":"repl","command":"python3"}'
```

The call answers as soon as the terminal exists. From there:

- `shell_input` types into it. The text is keystrokes, thus a control
  character works: `\u0003` interrupts, `\u001b` is escape. A return is added
  unless `enter` is false.
- `shell_output` reads it without typing: `new` for what appeared since the
  last read, `screen` for the whole screen, `region` for a part of it.
- `shell_close` ends it. Drive the program out itself where you can, such as
  `:q!` in an editor, so that it can save what it owns.

The reading follows what the program did. A program that writes lines is read
as lines, whole and in the order of arrival. A program that draws, such as an
editor, is read as the screen that it drew. Its bytes are cursor movements and
give nothing on their own.

How a session is shown. The session is the display, and not the calls that
drive it. The call that opens a session shows its screen, live and marked
running while the program is there. That screen is committed to the transcript
when the program stops. `shell_input` and `shell_output` show nothing of their
own, because their work is on that screen. Each row of the screen carries a
margin, so the screen reads as one object and as the screen of that call. It
starts under the name of the call, and `display/session_margin` gives the
margin, where an empty string draws none. The calls stay recorded as they were.
This is what the user sees and not what the arena keeps, and the model still
receives each result.

A session lives for one invocation. A later invocation cannot reach it. It
ends with the invocation, with `shell_close`, when its program ends, or after
`shell/session_timeout_ms` with no read and no write.

Refer to `doc/pty-terminal-plan.md`.

### A terminal for the user: `fyai term`

The same terminal, drawn for a user. `fyai term` runs a program on a
pseudo-terminal. libfyvterm interprets what the program draws. fyai
publishes that screen to a libfytimui surface, which draws it beside the
transcript and the prompt, with a state row of its own. The bytes of the
program do not reach the terminal. The user sees the screen that fyai holds,
which is the screen that a model reads.

```sh
fyai term                       # a shell
fyai term --login               # a login shell
fyai term -c htop               # one program
```

The keys are the program's while it runs, so its own line discipline decides
what Ctrl-C means. Ctrl-\ is the one key fyai keeps:

| Key | What it does |
| --- | --- |
| `Ctrl-\ q` | Leave. The program receives a hangup, as from a closing terminal, and is killed if it stays. |
| `Ctrl-\ r` | Draw the whole screen again. |
| `Ctrl-\ Ctrl-\` | Type one Ctrl-\ into the program. |

The screen belongs to the program. There is no prompt while the program runs,
because the keys are its own. The state row beneath the screen is the only row
that the program does not have. The screen follows the window. Resize the
terminal, and the program receives its new size in rows and in columns. When
the program ends, the transcript keeps its last screen.

`--hold` waits for a key before leaving. `--rows` and `--cols` fix the size for
a run with no terminal, and `--screen` writes the final screen as plain text.

There is no sandbox here. The user at the keyboard runs the shell, and
confinement is for a command that the model asked for.

List catalogue-defined agent tool sets:

```sh
fyai catalog tools
fyai catalog tools fyai --full
```

Interactive equivalent:

```text
/tools
/tools fyai --full
```

### Delegated agents and persistent branches

A sub-agent started by the model's `agent` tool is a proper arena branch, not a
throwaway conversation. Its branch persists after the delegated run and can be
inspected with:

```sh
fyai branch --all
```

Normal branch listings may hide agent branches to keep the primary workspace concise; `--all` includes them.

The calling model controls how the sub-agent context starts:

- **Fresh context:** the agent begins with a new context suited to the delegated task. Its work is still recorded on its own persistent agent branch.
- **Forked context:** the agent is a true continuation from the parent branch at the delegation point—a fork in the road with the parent's conversation available as its starting history.

A forked sub-agent is fixed to the same model as its parent. This preserves the semantics of a true continuation rather than silently changing the model at the branch point. A fresh-context agent may use the persona and model policy selected for that delegated role.

The parent controls delegation and receives the agent's report, while the agent branch retains its durable conversation and metadata for later inspection, comparison, or continuation.

Run one transient agent explicitly with:

```sh
fyai agent "find the parser entry points and report their responsibilities"
```

The standalone `agent` verb uses the built-in sub-agent persona and restricted
tool set, prints the final report, and does not publish conversation history or
create an agent branch. It also has an `--rpc` mode for the transient standalone
agent control protocol.

For model-delegated calls, configured personas may select a system prompt,
model, reasoning policy, or context behaviour subject to the fork rule above.
Independent sub-agent calls in one assistant message may execute concurrently.
A sub-agent cannot delegate another sub-agent.

## 7. Sandboxing and secret boundaries

### Landlock sandbox

```sh
fyai sandbox
fyai sandbox on
fyai sandbox off
```

Example policy:

```yaml
sandbox:
  enabled: true
  deny:
    - secrets
    - ~/.ssh
  allow:
    - { path: /ro/data, mode: ro }
    - { path: /src, mode: edit }
  network:
    ports: [443]
```

The `.fyai` arena is denied to sandboxed tools. Landlock is best-effort on unsupported systems; the portable tool policy remains in force.

Supported path modes include `rw`, `ro`, `edit`, and `append`. Supplying `network` restricts outbound TCP ports; an empty port list denies all configured egress.

### Secret handling

Secrets are deliberately kept out of immutable arena data:

- API keys are resolved indirectly;
- interactive secret entry is read separately from the slash line;
- subscription credentials use machine-local credential storage;
- wire logging can redact API keys;
- model tools are not given credential material.

## 8. MCP servers

MCP can expose additional tools over Streamable HTTP or local stdio transports.

```yaml
mcp:
  enabled: true
  startup_wait: true
  servers:
    github:
      endpoint: https://example.invalid/mcp/
      auth_token: { type: env, value: GITHUB_TOKEN }
    local:
      transport: stdio
      command: npx
      args: [-y, "@modelcontextprotocol/server-filesystem", .]
```

Tools are exposed as `mcp__<server>__<tool>`.

```text
/mcp
/mcp status
/mcp on
/mcp off
/mcp login NAME
/mcp logout NAME
```

Use `fyai help mcp` for the exact management and OAuth-import syntax supported by the current binary.

## 9. Observability and maintenance

### Usage and context

```sh
fyai stats
fyai stats --raw
fyai stats --json
fyai stats --yaml
fyai context
```

Interactive `/stats` reports cumulative usage over the selected conversation
chain, including turns from earlier invocations. `/usage` queries live
subscription limits and credits where supported.

### Logging

```sh
fyai log
fyai log wire start
fyai log wire stop
fyai log wire view
fyai log all clear
```

Interactive aliases are `/log` and `/logging`.

### Error trace log

The `log` verb above records the wire, the stream and the conversation of one
project. The trace log records something else: every diagnostic every fyai
process raises, as it is raised, whether or not it is ever shown. Use it when a
run fails in a way that leaves nothing on the terminal - a sub-agent that dies,
or a stress run of many of them.

```sh
FYAI_TRACE=1 fyai "..."            # append to ~/.fyai/trace.log
FYAI_TRACE=/tmp/run.log fyai "..." # append to a path you choose
FYAI_TRACE_LEVEL=warning fyai ...  # record warnings and above
```

Each line carries the time, the process, the branch of the sub-agent that
raised it, the severity, the subsystem and the source position:

```text
2026-08-18T07:13:04.262715Z 3593390 spawn: main/agent:worker, pid 3593392
2026-08-18T07:13:04.265101Z 3593392 [main/agent:worker] error stream: request returned HTTP 400 ...
2026-08-18T07:13:04.265862Z 3593390 reap: main/agent:worker, pid 3593392: exit 1
```

`start`, `spawn` and `reap` records are not diagnostics: they say that a
process began, that a child was forked, and how it ended - by exit status or by
signal. A child killed by a signal writes nothing itself, so its `reap` record
in the parent is what names the cause.

The file is only appended to, and never rotated or cleared by fyai. Remove it
yourself when you no longer need it.

### Garbage collection

```sh
fyai gc
fyai gc --keep-reflogs N
```

Garbage collection compacts unreachable arena data and requires arena quiescence. Bounding reflogs may make old branch entries and root handles unreachable.

## 10. Interactive mode

Interactive mode is a line-oriented REPL over the same durable branch state.

```sh
fyai
fyai -i
```

A line beginning with `//` is sent to the model verbatim with one slash removed. Slash command names accept an unambiguous prefix; exact matches win.

### Interactive commands

| Command | Purpose |
| --- | --- |
| `/branch [name\|list\|new\|delete\|rename\|show\|describe]` | List, create, inspect, modify, or switch branches |
| `/clear` | Start a fresh conversation |
| `/compact [hint]` | Summarize history into a fresh chain |
| `/model [name]` | Show or switch the model |
| `/api [mode]` | Show or switch API grammar |
| `/config ...` | Show, validate, describe, or edit configuration |
| `/list [what]` | List providers, models, turns, or exchanges; use `fyai list reflog` for the reflog |
| `/history ...` | Render conversation history |
| `/transcript ...` | Render conversation transcript |
| `/log ...` | Control trace logging |
| `/logging ...` | Alias for `/log` |
| `/secret ...` | Manage logical secrets |
| `/auth ...` | Inspect or control authentication |
| `/mcp ...` | Inspect or control MCP connections |
| `/context` | Report context fill |
| `/status` | Show model, provider, auth, context, and usage overview |
| `/stats` | Show cumulative token usage for the selected conversation chain |
| `/usage` | Show live subscription limits and credits |
| `/tools [agent] [--brief\|--full]` | List catalogue agent tools |
| `/help` | List commands and settings |
| `/exit`, `/quit` | Leave the session |

History selectors include:

```text
/history all
/history first N
/history last N
/history range A,B
/history --tool-detail full last N
```

### Interactive settings

Calling a setting without a value prints its current value.

`/reasoning-effort`, `/reasoning-summary`, `/temperature`, `/theme`,
`/tool-detail`, and `/transcript-system` persist to the current branch. The
remaining settings below are session-only.

| Setting | Values |
| --- | --- |
| `/reasoning-effort`, `/effort` | `minimal`, `low`, `medium`, `high` |
| `/reasoning-summary`, `/summary` | `auto`, `concise`, `detailed` |
| `/temperature` | floating-point value |
| `/theme` | embedded theme with optional `:auto`, `:dark`, or `:light` |
| `/tool-detail` | `none`, `brief`, `default`, `full` |
| `/transcript-system` | `on` or `off` |
| `/markdown` | `on` or `off` |
| `/stream` | `on` or `off` |
| `/thinking` | `on` or `off` |
| `/sandbox` | `on` or `off` |
| `/token-extents` | `on` or `off` |
| `/print-stats` | `on` or `off` |

Use `/help` in the running binary for the current command and setting spellings.

## 11. CLI reference

```text
fyai [global-options] <verb> [verb-args]
fyai [global-options] <prompt...>
```

Global parsing stops at the first non-option. A known token is dispatched as a verb; otherwise the remaining arguments form the prompt. A lone `-` reads the prompt from standard input.

### Main verbs

| Verb | Purpose |
| --- | --- |
| `init [path]` | Initialize the repository arena |
| `dump [state\|anchors\|providers]` | Inspect canonical or provider state |
| `transcript [opts]` | Human-facing conversation view |
| `history [opts]` | Alias for `transcript` |
| `display [opts]` | Alias for `transcript` |
| `stats [--raw\|--json\|--yaml]` | Cumulative token and cost usage |
| `config ...` | Show, validate, import, export, and edit config |
| `list ...` | List providers, models, turns, exchanges, or reflog |
| `branch ...` | Manage branches; `--all` includes agent branches |
| `checkout [-b] ...` | Move arena `HEAD` |
| `reset REF` | Move the selected branch head |
| `root [print\|show]` | Print or explain an exact arena root |
| `rebase BRANCH` | Reorder branch exchanges with current work last |
| `merge BRANCH` | Interleave branch exchanges by time |
| `clear` | Publish an empty conversation |
| `compact [hint]` | Summarize into a new chain |
| `context` | Report context fill |
| `api [mode]` | Show or store API grammar |
| `catalog ...` | Inspect catalogue data and agent tools |
| `agent TASK` | Run one transient sub-agent and print its final report |
| `auth ...` | Manage provider authentication |
| `secret ...` | Manage logical secrets |
| `log ...` | Manage trace logs |
| `sandbox ...` | Show or configure sandbox policy |
| `mcp ...` | MCP management and OAuth import |
| `gc ...` | Compact arena storage |
| `tool NAME [json]` | Execute one tool without a model call |
| `help [verb]` | Current command reference |

### Global options

| Option | Purpose |
| --- | --- |
| `--version` | Print version |
| `--config`, `-C FILE` | Overlay an explicit configuration file |
| `--env`, `-e FILE` | Read literal `.env` assignments used by fyai |
| `--model`, `-m MODEL` | Select model or provider/model |
| `--branch`, `-b NAME` | Select a branch for this invocation |
| `--root VALUE` | Read an exact root in read-only mode |
| `--set KEY[=VALUE]` | Apply a typed configuration delta |
| `--get KEY` | Print a key in one-line flow form |
| `--delete KEY` | Delete a configuration key |
| `--transient` | Do not publish ordinary invocation state |
| `--api-key`, `-k KEY` | Supply an API key explicitly |
| `--new` | Start a new conversation |
| `--sandbox` | Enable shell-tool confinement |
| `--color MODE` | `auto`, `off`, or `on` |
| `--theme THEME` | Select markdown theme |
| `--interactive`, `-i` | Start interactive mode |
| `--answer TEXT` | Pre-supply an `ask_user` answer; repeatable |
| `--debug`, `-d` | Increase diagnostic verbosity |

## 12. Architecture and operational notes

- Arena values are immutable after publication.
- The root is the atomic mutable publication point.
- Concurrent work on different branches can publish independently; same-branch movement follows the configured conflict policy.
- Model-delegated sub-agent work is represented by persistent agent branches.
  `fyai branch --all` exposes them; the standalone `fyai agent` verb is
  transient.
- Fresh-context delegated agents start a new task context; forked agents
  continue from the parent branch and use the parent's model.
- fyai adds repository instructions such as `AGENTS.md` and `CLAUDE.md` to
  the system prompt when it creates a conversation. A continuation uses the
  stored system turn. If the files have identical content, fyai adds the
  content only once. fyai compares the content hash, not the path.
- Streaming output is observable, not authoritative. Completed canonical state is what gets published.
- `history` is optimized for people. Use dump views for exact state inspection.
- Linux is the primary platform. macOS-specific credential and event backends are supported where implemented; Landlock and the kernel secret store are Linux-specific.

## 13. Where to look next

```sh
fyai help
fyai help VERB
fyai config schema
fyai config describe [PATH]
```

Repository references:

- `config.yaml.sample` — annotated configuration examples;
- `data/config.schema.yaml` — accepted configuration structure;
- `doc/branching.md` — detailed branch and root semantics;
- `doc/srd/fyai-srd.md` — storage and provider contract;
- unit and functional tests — executable behaviour and edge cases.

When prose disagrees with the implementation, treat the command handlers and
tests as authoritative. Detailed per-verb help and the configuration schema are
useful generated views of the portions they cover.
