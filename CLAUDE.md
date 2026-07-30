# CLAUDE.md

Guidance for working in this repository. See `doc/srd/fyai-srd.md` for the
authoritative spec.

## What this is

`fyai` is a **stateless, daemon-less Unix-style AI coding assistant** in C. Each
invocation runs one complete tool-use loop, commits canonical state to a
content-addressed libfyaml arena, and exits. An interactive invocation owns a
terminal UI for its process lifetime; there is no resident process or sidecar
state format.

## Architecture invariants (do not break)

- No daemon, resident process, or hidden process state. One invocation = one
  full loop + state commit + exit; the terminal UI is invocation-local.
- Persistent state lives in **libfyaml content-addressed arenas** (under
  `~/.fyai`), never in sidecar formats. Canonical data is immutable,
  deterministic, and address-stable across processes; arena relocation is
  forbidden.
- Keep the tool surface Unix-shaped: read file, write file/patch, run shell
  commands under approval.

## Data model

Use **libfyaml generics** as the native data model — build with
`fy_gb_mapping()`, `fy_gb_sequence()`, etc., not hand-written JSON strings.
Emit compact JSON only at provider boundaries (`FYOPEF_MODE_JSON`) and parse
provider responses straight back into generics. Map provider wire details
(tool-call IDs, request IDs, finish reasons, timestamps) into provider-stream
generics, then derive provider-agnostic content for canonical identity.

Use typed accessor defaults: `fy_get(obj, "content", "")`,
`fy_get(obj, "total_tokens", 0LL)`, `fy_get(obj, "items", fy_invalid)`.

**String lifetime — prefer `fy_castp` over `fy_cast`.** A short string is stored
*inline in the `fy_generic` word*, so the `const char *` from `fy_cast(v, "")`
points into `v` itself. If `v` is a by-value local (e.g. a struct field returned
by value), that pointer dangles once the holder goes out of scope — and only for
short strings, since long ones live in the arena (a nasty, data-dependent bug).
Use `fy_castp(&v, "")`, which takes the *address* of the stored generic and
returns a pointer into that stable storage; compute it at the use site from a
long-lived holder, never cache a `fy_cast` result past the generic's scope.

**Empty strings must survive a YAML round-trip.** An empty-string generic is a
string, not null — but under the core/1.1 schemas a *bare* plain empty scalar
(`key:`) reads back as null. libfyaml's generic emitter therefore quotes an
empty string (`key: ""`) whenever the style is unspecified, so it round-trips
as the empty string in block, pretty and flow output alike. fyai relies on this:
config keys whose default is `""` (e.g. `display/tool_separator`, `prompt_top`)
are stored as empty strings and must re-parse as strings, or the `type: string`
schema check fails on the next commit (an in-session `/config edit` save). Keep
these keys `type: string` in `data/config.schema.yaml` — do not widen them to
accept null to paper over an emitter that drops the quotes.

## Source map

- `src/main.c` — CLI grammar: parse global options, then dispatch a verb or run a
  prompt (`fyai [global-options] <verb|prompt>`).
- `src/commands.c` — verb dispatch (`init`/`dump`/`stats`/`config`/`gc`), the
  `fyai_run` engine runner, and the colorized `fyai_usage` (fy-tool style).
- `src/fyai.c` — core engine orchestration; functional modules live beside it.
- `src/fyai_output.c` — the context-owned transcript output. One tagged
  (`system`, `user`, or `assistant`) Markdown document stays open across an
  entire assistant model/tool loop, owns progressive rendering, and is
  finalized into the durable turn as `display_outputs`. History replays these
  exact documents; message/provider reconstruction is legacy-arena fallback.
  A render path must not call `fytim_pump()`. The pump drains input and can
  re-enter input handling. Set `frame_pending` and let the owner paint it.
  Add generated fragments with the checked `fyai_output_printf()` API and
  provider byte streams with `fyai_output_append()`. Do not create a parallel
  renderer or print transcript content from a tool/reasoning producer.
- `src/fyai_session.c` — session commands: shared backends for the interactive
  slash commands (`/clear`, `/compact`, `/model`, `/api`, `/context`, plus a
  data-driven table of settings like `/effort`, `/theme`, `/markdown`) and
  the `clear`/`compact`/`context` verbs; the tokenizer-free context estimator
  (bytes/4) and linenoise tab completion (command names, enum values,
  catalogue model names); and `fyai_readline()`, the event-driven line input
  that replaced the blocking `linenoise()` — an fd source on the terminal feeds
  `linenoiseEditFeed()` a byte per readiness event, with SIGWINCH borrowed for
  the duration so a settled resize still repaints (an idle `epoll_wait` would
  otherwise never re-enter `EditFeed`). It falls back to blocking `linenoise()`
  when the terminal cannot be edited, which is why the non-tty functional cases
  never exercise the event path — verify it under a pty. In the REPL a
  `/`-prefixed line never reaches the
  model; `//` escapes a literal slash line. Ctrl-C discards a nonempty input
  line. It ends an idle session when the line is empty and cancels an active
  turn. `fytim_cfg.intr_signal` keeps `ISIG` enabled for the INTR key. Escape
  and SIGINT both call `fyai_ui_interrupt()`. PTY signal tests need a
  controlling terminal. `/model` re-runs
  `fyai_config_resolve_model()` and `fyai_request_state_apply()` mid-session,
  re-deriving `<PROVIDER>_API_KEY` unless the key was explicit. Request-shaping
  switches (`/model`, `/api`, the reasoning options, `/temperature`) persist
  into the arena config via the one commit path, so a continuation resumes on
  them (`--transient` keeps the publish in-memory); display settings stay
  session-only. `--new` behaves exactly like `/clear`: it publishes a turnless
  head reset. `/compact`
  makes one tools-off summary call and restarts the chain from the summary,
  keeping the old head under turn metadata `compacted_from`.
- `src/fyai_agent.c` — sub-agent execution. The `agent` verb and tool use
  `fyai_agent_run()`, which takes the call arguments (`task`, `name`,
  `description`) rather than a bare task. It owns the persona, restricted tools,
  conversation, curl handle, and output routing. **A sub-agent keeps its
  conversation on its own branch** below the one that started it
  (`main/agent:greeter`). The agent's name *is* the branch name; `:` is invalid
  in a user-given name, so `agent:` marks a branch fyai wrote and nothing else
  can forge one (`fyai_branch_name_valid()` stays strict for creation,
  `fyai_branch_name_ref_valid()` accepts `agent:` for selection). A taken name
  is a **clash that fails the call** — no ordinal — so the model retries with
  another name and the name stays a handle. Name it in the parent **before**
  `fyai_tool_job_spawn()`: a clash found after the fork would have to discard a
  child already waiting for a request that never comes, and the blocking reap in
  `fyai_tool_job_discard()` deadlocks against the staged terminate, which needs
  the loop to run. Copy it into the job **after** the spawn, which clears the
  job. Call `fyai_branches_refresh()` first — a sub-agent publishes from its own
  process, so the table read at startup does not hold what an earlier one made.
  The agent tool's `context` selects **`fork`** (default — the sub-agent branch
  starts at the parent's head, so it sees the conversation) or `fresh` (task
  alone). Set the agent's `cfg` fields (persona, `agent_child`, `mcp_enabled`)
  *after* `fyai_arena_reopen()`, which re-applies the branch config and would
  otherwise overwrite them. In a fork the persona is a **user** instruction
  message, not a system turn: `fyai.c` hoists the conversation's canonical
  system turn into the request `instructions`, so a second system turn would
  never take effect.

  The agent tool takes a `timeout` in milliseconds, which stops the sub-agent
  and everything below it. It is what the model asked for, so
  `agent/max_timeout_ms` bounds it, the way `shell/max_timeout_ms` bounds the
  shell tool. A parameter the tool does not declare is a parameter the model
  never sends, so reading one is dead code that reads as if the model had a say.

  `agent/personas` in config names sub-agent personas
  (`description`, `system_prompt`, `model`, `reasoning/*`, `temperature`,
  `max_tokens`, `thinking`, `timeout_ms`, `context`); the tool's `persona`
  parameter selects one, and `make_tools_filtered()` lists the configured names
  and descriptions into that parameter's description — the model can only pick a
  persona it is told about. A persona `model` resolves through the catalogue
  into a scratch cfg (as `/model` does): selecting a model selects its provider,
  so endpoint and `max_tokens` are re-derived, and a `provider/model` persona
  pins the offering. A user-configured `api_url`/`max_tokens` survives — derived
  values are never persisted, so presence in `config_doc` is what distinguishes
  intent from derivation. The child then calls `fyai_arena_reopen()` before it publishes:
  the arena is a shared mapping and this is a fork, so it must become an
  independent writer first (see doc/branching.md §9.1). `fyai agent --rpc` services `initialize`,
  `agent/run`, and `shutdown` on standard input and output. A delegated agent
  sends its tool calls and output to the parent work band. The parent renders
  this progress as one Markdown quote. Tool calls use `display/tool_detail`.
  The agent flushes each call before it starts the tool. Agent tool results stay
  hidden. The final report is the last quote update.
- `src/fyai_event*.c` — the context-owned portable event loop. Linux uses
  epoll/signalfd/timerfd/pidfd and BSD/macOS use kqueue. fyai owns its signals;
  active operations observe interrupt/termination through loop callbacks, not
  process-global subsystem handlers or blocking EINTR loops. SIGPIPE remains
  blocked in the parent for synchronous event delivery, and forked children
  restore their own disposition before exec.
  SIGINT uses `fyai_event_interrupt_open()` because a stopped loop cannot
  service `signalfd`. The handler sets `interrupt_pending`, writes to the wake
  source, and starts a 200 ms watchdog. A pump that observes the interrupt must
  call `fyai_event_interrupt_ack()`. Do not register SIGINT with `signalfd`.
  SIGALRM belongs to the watchdog. Curl handles must use `CURLOPT_NOSIGNAL`.
  An interrupted turn keeps the steps that completed: the run
  loop wraps the partial turn (or fy_invalid) with a diagnostic via a manual
  `FYGIF_DIAG` indirect (`fyai_with_diag`/`fyai_report_diag` in fyai.c) — since
  `fy_generic_is_invalid()` dereferences indirects, a diag-wrapped invalid
  still tests invalid everywhere, so no assert-valid churn. Ctrl-Z suspend at
  the prompt and the SIGWINCH reflow live in the vendored linenoise
  (`third_party/linenoise`, fyai-local extensions).
- `src/fyai_event_dump.c` — SIGUSR2 event-loop diagnostics. The signal handler
  uses async-signal-safe operations and writes to the original standard error
  descriptor. Use `addr2line` to resolve callback addresses.
- `src/fyai_render.c` — the one generic-to-Markdown table renderer
  (`fyai_generic_to_markdown()`). Every Markdown table goes through it: a
  mapping renders as a two-column key/value table, a sequence of mappings as
  one column per key. Callers pass a `renderopts` generic (title, preamble,
  `keys` column selection, per-key `columns` overrides of name/align/format)
  rather than a hand-written table; without an override a column's name is the
  humanized key and its alignment is right for numbers/booleans, left
  otherwise. Cells are escaped and truncated, so no value can break a table.
  Build renderopts in the **transient** builder (or frame-locally, in the same
  frame as the call) — never the durable arena; a builder-less `fy_mapping()`
  is `alloca` storage and must not be returned from a helper.
- `src/fyai_diag.c` — the diagnostic layer, following libfyaml's
  `fyp_error_check()` shape: `fyai_error_check(ctx, cond, err_out, fmt, ...)`
  reports and jumps to the common cleanup label; `fyai_error`/`_warning`/
  `_notice`/`_info`/`_debug` report without jumping, and `fyai_cfg_*` variants
  serve the option parsing and `configure_*` hooks that run before there is a
  context. Each `.c` names its subsystem once (`#define FYAI_MODULE
  FYAIEM_CONFIG`) and the module supplies the message prefix, so `config: ` is
  never hand-typed. Diagnostics are *collected* into `cfg->diag`, not printed,
  and drained at the turn/verb/slash boundaries — a mid-turn write would tear
  the spinner or the streamed render. An error prints bare (`config: msg`, as
  the subsystems always did); lower severities are labelled; the captured
  file/line/func surfaces only under `debug`. **Only the first error is the
  cause**: an error raised while one is already collected is demoted to debug
  (libfyaml's `on_error` latch, but derived by scanning the sequence, so a drain
  *is* the reset). That is why a cleanup path may keep its generic "X failed" —
  it prints only when nothing else explained why, and otherwise it disappears
  instead of burying the reason. `fyai_diag_reset()` drops the collected
  diagnostics for a caller that recovered (a tried-then-worked-around failure),
  so the next one reports as a cause again. `-d` unmasks the whole unwind chain. A NULL sink prints immediately, so
  callers with no `cfg` still report. The sink owns its **own** builder: never
  the durable arena, and not `transient_gb` (destroyed per turn, absent for most
  verbs, nonexistent for the pre-context callers). `fyai_diagf()` expands the
  format before interning, so a diagnostic never points into another builder —
  that is what makes `fy_generic_builder_reset()` safe on every drain. Raising
  is lock-free from any thread (CAS on the list word; a plain store loses ~80%
  of concurrent raises, which `tests/fyai_diag_test.c` proves), but a drain's
  reset invalidates `gb` for everyone, so drain only where raisers are quiescent.
  Two rules follow from the demotion and are easy to get wrong: **detail
  belonging to a failure is part of that diagnostic, not another one** — an HTTP
  body, or a config's list of problems (`fyai_config_report_problems()`), raised
  separately would be demoted behind the status and lost; and **a non-fatal
  report must not be an error** — it would latch the state and silence the next
  real one. Files serving several verbs (`commands.c`, `fyai_storage.c`,
  `fyai_session.c`) take `FYAIEM_UNKNOWN` and keep naming their verb in the
  message. Output that is not a report about a failure stays a direct write: the
  banner, the spinner, the shell echo and `ask_user` prompt, `init`'s arena path,
  the stats line, per-verb usage, and the leaf helpers holding neither a context
  nor a configuration (`fyai_peek_arena_config()`, `fyai_secret_action()`,
  `parse_turn_selector()`, the `--env` parser).
- `src/utils.c` — HTTP response buffers, shell exec capture, generic emit/parse.
  The shell `fork`/`exec` optionally applies a `fyai_sandbox_spec` in the child
  before exec. `struct shell_command_opts` shapes one sub-execution: `workdir`
  does the `chdir` *before* the confinement (which is one-way, so the target
  must stay reachable; status 125 marks a directory that cannot be entered),
  and `timeout_ms` arms a deadline.

  The capture is finished when the child is reaped. It does not wait for the
  pipes to reach end of file. `/bin/sh` usually forks instead of execs, so some
  descendant holds the write ends open for as long as the command would have
  run. While the capture waited for that, every cancelled or timed-out command
  came back only when it stopped by itself. Do not try to fix this by giving
  the shell child its own session or process group. It is in the tool job's
  group on purpose, so that one group terminate can sweep it up.

  **A time limit belongs to the job, not to the command inside it.** Only the
  parent can stop a whole process group, and the job child is a session leader.
  One call to `fyai_tool_job_cancel()` therefore stops the shell or sub-agent,
  everything it started, and everything below that. A limit armed inside the
  job child reaches only that child's own child, and a shell forks, so the rest
  survive and are handed to init. This is why `fyai_tool_job_submit()` arms the
  deadline in the parent, and why `cfg->tool_child` stops the capture inside the
  child from arming a second, weaker one.

  **`timeout_ms` and `max_timeout_ms` are two different limits, not two names
  for one.** `timeout_ms` is the limit to use when nobody asked for one.
  `max_timeout_ms` is the largest limit the model is allowed to ask for.

  They never apply to the same number. If the model asks for a limit, that value
  is used, and the ceiling can cut it down. If the model asks for nothing, the
  default is used, and the ceiling does not come into it. Only the model's value
  is cut down, because only it comes from outside. The default, and a persona's
  `timeout_ms`, are settings the user wrote, so they are used as written.

  The most specific limit wins. For the shell tool that is the value on the
  call, and then `shell/timeout_ms`. For a delegation it is the `timeout` on the
  call, then the persona's `timeout_ms`, then `agent/timeout_ms`, which is off
  unless it is set.

  **The two kinds of shell call name the model's limit differently.** The
  `shell` function tool puts `timeout` in its arguments. The native Responses
  `shell_call` puts `timeout_ms` inside its action.
  `fyai_shell_timeout_requested()` reads the right one for each kind. Hand a
  native call to it as a plain argument mapping and the model's limit is not
  found, so the call quietly runs under the configured default instead.

  Only the parent can say that a limit expired, which is why it is said in
  `fyai_tool_job_collect()`. The child usually gets its result out before it
  dies, but a child cannot tell a group terminate from an interrupt, so it
  reports the wrong reason.

  **A time limit must not change the shape of the result.** A native shell
  result is always a list of `{stdout, stderr, outcome}`. When a limit expires,
  the outcome becomes `{type: timeout, timeout_ms}`, and everything the command
  printed is kept.

  Do not replace the whole result with an error string. That throws the output
  away, and it gives the model a shape it has never seen before on the one call
  where the detail was worth reading. Do not leave a bare `{type: signal}`
  either, because that reads as a crash the model should retry, rather than a
  limit it should respect. If the child was killed before it reported anything,
  the tail of `tool/progress` that the parent kept is the only output left.

  A forked tool child uses `/dev/null` as standard input and calls `setsid()`.
  The parent must not call `setpgid()` on the child. Cancellation uses
  `fyai_event_add_child_terminate_group()` to stop all descendants.
  A child services `tool/run` over JSON-RPC on file descriptors 0 and 1. It
  sends `tool/progress` notifications and returns `{result, ok}`. Standard
  output contains protocol frames only. Set job fields after
  `fyai_tool_job_spawn()` because the function clears the job.
- `src/fyai_sandbox.c` — Landlock confinement for shell-tool sub-executions
  (`--sandbox` / config `sandbox`, default off). ABI-probed and masked; grants
  read-only system paths + read/write project (children of the root minus the
  hidden `.fyai`) + temp dirs, denies the rest; applied one-way in the child so
  it is inherited across the exec and every process it spawns. Linux-only: on
  other platforms (macOS) it compiles to no-op stubs behind the same interface,
  where a Seatbelt (`sandbox_init`) back-end would slot in. It is the §4.5/§10
  enforcement floor only; command admission (allow-list/prompt) stays elsewhere.
- `src/fyai_oauth.c` — the provider-agnostic half of an OAuth 2.0
  authorization-code login: PKCE challenge/state generation, the loopback
  redirect receiver (RFC 8252 §7.3, bound to 127.0.0.1 only, `SO_REUSEADDR` so a
  failed login can be retried before TIME_WAIT drains), and opening a browser.
  The receiver is a **state machine on a borrowed loop**, not a blocking wait:
  `fyai_oauth_flow_start()` arms it and returns, and it advances through
  `FYAI_OAUTH_LISTENING → GOT_CODE|TIMED_OUT|BAD_STATE|FAILED` as accept/read/
  timer events fire, so a login can be armed on a loop already carrying a turn.
  `fyai_oauth_flow_wait()` is the sync convenience over the same machine — the
  async/sync pairing of `fyai_event_add_child_terminate()`/
  `fyai_event_child_terminate()`, not a second implementation. It accepts
  several connections at once because a browser also fetches favicons and opens
  speculative connections to the redirect URI; anything that is not the redirect
  path gets a 404 and the wait continues, rather than failing the login. What
  stays with the caller is everything provider-shaped: issuer, client id,
  scopes, the authorize query, the token exchange and where credentials land.
  `fyai_auth.c` drives it for the compiled-in Codex subscription flow;
  `fyai_mcp.c` drives resource discovery, configured or dynamic clients,
  browser authorization, token exchange, refresh, and 401 recovery.
  `tests/fyai_oauth_test.c` drives both ends from one loop — the client sockets
  are sources too — so a regression back to a synchronous accept/read fails the
  test rather than passing it.
  Configured MCP clients remain after logout. A dynamic client is bound to one
  receiver URI and must be removed on logout. Each terminal OAuth path must
  release its discovery state. Explicit login or logout cancels an active
  browser wait, discovery, or refresh.
- `src/fyai_jsonrpc.c` — JSON-RPC 2.0 over stdio or HTTP. A stdio connection
  owns its reader, writer, and buffers. It matches responses by request ID.
  `jsonrpc_conn_serve()` handles requests and notifications. A completed
  request must leave both connection lists before its storage is released.
- `src/fyai_config.c` — layered config loading (arena-resident repo config),
  slash-path config verb (import/export/edit/show/get/set/delete) and the
  global --set/--get/--delete ops.
- `src/fyai_branch.c` — branches: independent lines of work, each owning both
  a conversation chain and a config. The hierarchy is carried entirely by the
  name (`main/explore-1` is below `main`), so parent/child is a prefix test and
  there is no separate tree. Holds name validation, branch-entry
  decode/build/splice, the `branch`/`checkout` backends, and
  `fyai_resolve_ref()`. **References are symbolic only** — `<branch>`,
  `<branch>~N`, `<branch>@{N}` — because `gc` relocates arena objects, so a raw
  address is not a stable name; never reintroduce a numeric ref. `~N` counts
  *stored* turns (one message append each, as `fyai list turns` shows), so one
  exchange back is `~2`. `--branch`/`-b`/`$FYAI_BRANCH` pick a branch for one
  invocation; only `checkout` moves the stored `HEAD`, which is why
  `ctx->head_branch` is kept apart from `ctx->branch`. A ref-log entry stores
  the **operation** that made it (`op`, plus `from` on a rename) — never infer
  it by diffing heads, since a reset moves the head backwards and a rename
  leaves it alone, so neither is distinguishable from a turn or a config edit
  that way. Label a publish with `fyai_branch_op_set()`; it is consumed and
  cleared by that publish so it cannot leak into the next. No entry stores a
  name, which is what makes a rename a safe rekey of the mapping plus `HEAD`.
  A sub-agent name comes from the *model*, so it must go through
  `fyai_branch_alloc_child()` (sanitize to one component, unique ordinal,
  depth cap) before it can be part of a branch path. Sub-agents cannot
  delegate, so the current execution path adds only one component.

  **Aim for no surprise against git.** A start point stands for a whole state:
  `branch create <name> <start>` and `checkout -b <name> <start>` take the
  conversation *and* the configuration in force there
  (`fyai_resolve_ref_state()`), and without one the current branch is the start
  point. Taking the turns from the start point but the config from elsewhere is
  exactly the kind of half-and-half that surprises.

  `--root <handle>` (from `fyai root print`) pins the arena to one published
  root, making the invocation read-only — a write is *refused*, not silently
  skipped as under `--transient`, because automation must not believe it
  committed. The handle is untrusted input and is **never dereferenced on
  trust**: `fyai_root_find()` walks the root ref log and compares raw values,
  so an unknown handle is simply not found. Keep the per-step check cheap
  (`root_shape_ok()`) and run the full `fyai_root_validate()` once on the hit —
  validating deeply at every step cost ~126 ms versus ~45 ms for a 341-root
  walk and scaled as O(roots x branches x ref-log). For the same reason
  `fyai_root_validate()` is **shallow**: it checks only the entry each branch
  points at, and whoever walks a ref-log chain checks the next link itself with
  `fyai_branch_entry_contained()`. See `doc/branching.md`.
- `src/fyai_merge.c` — joining branches. A conversation is an append-only
  message list, so a join decides an *order*, never reconciles edits — there is
  no textual conflict to resolve. `rebase` replays ours on top of theirs;
  `merge` interleaves by time. Both work in **exchanges** (a user turn plus the
  turns answering it) so a question is never split from its answer. Exchange
  times come from the branch ref log, which records each *head* — and a head is
  the turn that *ends* an exchange, so never look a time up on the user turn
  that opens one. `fyai_branch_timestamp()` is microsecond-resolution for this
  reason: at second resolution a whole run ties and order falls to the
  tie-break. `fyai_merge_base()` is an identity test on stored values, which
  works because the store is content-addressed — two branches with the same
  system prompt genuinely share that turn, so truly unrelated histories are
  rarer than they look. A join keeps exactly one system prompt. On a lost CAS,
  `publish_reconcile()` retries when only *other* branches moved (a publish
  rebuilds one entry and carries the rest by reference) and applies
  `branch/on_conflict` when ours did; rebase and merge coincide there, since
  our unpublished turns have no ref-log time and theirs are already committed.
- `src/fyai_catalog.c` — provider/model catalogue: arena document or embedded
  snapshot, lookups, `catalog` verb.
- `src/*.h` — context structs and internal module interfaces.
- `doc/srd/` — authoritative requirements/design; update when changing
  architecture or invariants.

## Build & run

```sh
cmake -S . -B build
cmake --build build
./build/fyai -m gpt-4o-mini "hello"   # no-verb prompt
./build/fyai dump state               # a verb; see ./build/fyai -h
```

ASAN build (run for parser, storage, tool, or YAML changes):

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
```

Markdown rendering uses `libfymd4c` directly. The renderer is a required CMake
dependency and provides progressive/healing terminal rendering plus syntax-aware
fenced code blocks through libfyts.

Static build modes:

- `-DFYAI_MOSTLY_STATIC=ON` links fyai's dependencies statically but leaves
  glibc/libm dynamic. Use this for portable glibc binaries; fully-static glibc
  can fail in `getaddrinfo()`/NSS on a different host. Only this mode compiles
  the `src/glibc_compat.c` wrappers for glibc's C23 `__isoc23_*` redirects, so
  GNU C2x builds do not require `GLIBC_2.38` just for `strtol`/`sscanf` family
  calls; fully dynamic builds use the host glibc directly.
- `-DFYAI_MUSL_STATIC=ON` is the fully-static musl mode, intended for the Docker
  builder (`MODE=musl scripts/build-static-docker.sh ./fyai`), which builds
  musl, zlib, OpenSSL, libfyaml, curl, libfymd4c, and libfyts for the same musl
  toolchain.
- `-DFYAI_STATIC=ON` is the legacy fully-static host-libc mode; keep it only for
  controlled same-host/static-glibc experiments.

All static-dependency modes require an installed `libfyaml.a`; glibc modes also
use system `libssl.a`/`libcrypto.a`/`libz.a`. Mostly-static verification should
show only libc/libm/ld.so in `ldd build-static/fyai`; musl/static verification
should show `not a dynamic executable`.

There is **one event loop per invocation**, owned by `struct fyai_ctx` and
created on first use (`fyai_ctx_loop()`); every subsystem — curl, shell
capture, forked tool jobs, MCP child shutdown, readline, the OAuth receiver —
borrows it and removes only its own sources. Two rules follow, and both were
real bugs when the loops were per-subsystem:

- **A borrower must withdraw its own sources.** There is no per-call
  `loop_destroy()` to sweep up any more, and a source left behind points at a
  dead stack frame. Where a callback may retire a source itself (a pipe at EOF,
  a one-shot child), the owning struct holds the source pointer and clears it,
  so withdrawing is idempotent rather than gated on a parallel bool.
- **A child that forks without exec must call `fyai_ctx_loop_abandon()`.** An
  epoll set is shared across fork, so a child that kept the loop would register
  descriptors in its *parent's* set carrying pointers from the wrong address
  space — and the parent then dispatches on garbage. Abandoning closes the
  child's descriptor copies but must never `epoll_ctl`, which would mutate the
  shared object. Anything that execs is fine: the loop's descriptors are
  CLOEXEC. Note this corruption is **silent in a normal build** and only
  reliably caught under ASAN, which is why
  `tests/fyai_event_test.c:test_fork_child_abandons_loop` guards it directly.

The interactive path has one application-owned pump; model, tool, MCP, OAuth,
editor, and config-edit callbacks beneath it must never call `run_until()` or
manually step the loop. Thin synchronous wrappers remain only for standalone
commands, batch/setup adapters, and isolated tests that own the top-level loop.
Do not call one of those wrappers from an interactive callback: a nested run
would dispatch outer sources and can re-enter the caller.

The event layer recycles loops and sources through free lists, which hides
use-after-free from the sanitizers: a stale pointer to a retired source still
lands on live, correctly-typed memory, so ASAN and valgrind see nothing.
**Sanitizer builds therefore default to no recycling**, making every loop and
source a plain malloc/free. `FYAI_EVENT_NO_POOL` overrides that either way —
`1` disables recycling in a normal build, `0` forces the pooled path back on
under ASAN so the shipping behaviour stays testable. Both settings must pass
the full suite.

Run the test suite with `ctest --test-dir build --output-on-failure`. The
`fyai_test` binary contains all unit tests. It links the production sources
except `src/main.c`. Do not use test stubs. Declare each test with
`FYAI_TEST_ENTRY(suite, name, entry)` after the includes in a
`tests/fyai_*_test.c` file. CMake generates the registry and one CTest test for
each declaration. Use `ctest -R fyai/unit/oauth` to run one suite. Use
`./build/fyai_test oauth/plain_redirect` to run one test. Use
`./build/fyai_test --list` to list all tests.

The functional cases (`tests/cases/*.sh`) drive the real binary against the
scenario-driven mock provider (`tests/mock/mock_provider.py`,
`tests/scenarios/*.json`) in a hermetic sandbox — localhost only, private
`HOME`/`XDG_*`. The whole suite, functional cases included, runs under ASAN
builds too.

### Debugging terminal rendering with libvterm

Use libvterm whenever correctness depends on terminal cells rather than the
presence of escape bytes: background fill, blank styled rows, wrapping,
cursor placement, repaint damage, or SGR state carried across rows. Grepping a
PTY capture is useful for text, but cannot prove any of those properties.

For renderer/libfytimui bugs, use the direct-render oracle pattern in
`libfytimui/tests/test_fytim_md_vt.c`:

1. Render the Markdown directly with libfymd4c and feed those bytes into one
   `VTerm`.
2. Send the exact same rendered bytes through `fytim_commit()`,
   `fytim_tail_apply()`, or the relevant public libfytimui path; pump the UI
   through a pipe transport and feed its terminal output into a second
   `VTerm`.
3. Locate a stable text row in both screens, then compare the relevant
   `VTermScreenCell` fields across the complete row and any adjacent blank
   rows. Always inspect cells after the final glyph: erase-to-EOL and
   background-fill regressions hide there.
4. Normalize colors through each screen's palette with
   `vterm_screen_convert_color_to_rgb()`, then compare with
   `vterm_color_is_equal()`. Never `memcmp(VTermColor)` or assume indexed and
   RGB representations compare byte-for-byte.
5. Observe the new test fail against the broken implementation, register it
   individually with CTest, and run both the libfytimui and fyai suites after
   the fix.

If the cell grids differ while the renderer bytes look correct, trace the
actual transport writes (`strace -e trace=write -s 1000 ...` on Linux).
Inspect later compositor cleanup as well as the original render: a correct
background `CSI K` can be undone by a subsequent SGR reset plus another
`CSI K`.

Three provider modes: Responses API (default), Chat Completions, and the
Anthropic Messages API, selected via `config set api <mode>` / `--set
api=<mode>` (`responses|chat-completions|messages`) — there are no dedicated
`--responses`/`--chat-completions`/`--messages` flags; the config document is
the single place API grammar is chosen, durably, from the command line or
otherwise. Messages mode authenticates with `x-api-key` + `anthropic-version` headers and
always sends `max_tokens`; inbound `tool_use` blocks are normalized to
Responses-style `function_call` items at the parse boundary, so canonical
state, display and cross-provider replay share one shape. Credentials come
from `OPENAI_API_KEY`/`ANTHROPIC_API_KEY` env mappings and the `*.env` files
in the root (all gitignored).

An API grammar does not belong to one provider, but parts of it do. Several
providers speak Responses, and OpenRouter is one of them, yet the built-in
`shell` tool and the `shell_call` items that go with it are not part of the
grammar everywhere. A provider that does not know an item does not skip it.
The item matches no member of the input union, so the request is refused as a
whole, and every later turn of that conversation is refused with it.

Who takes what is the catalogue's to say, not a provider name written into the
code. Each provider entry carries its endpoints, and each endpoint its
`capabilities`, `shell_tool_supported` among them. `fyai_config_resolve_model()`
reads the one for the grammar in force into `cfg->shell_tool_supported`, keyed
by protocol rather than by position, because the endpoints are not in the same
order for every provider. It is derived and never persisted, like the endpoint
URL and `max_tokens` beside it, and it is read after the provider default is
applied, or a model the catalogue does not carry is looked up under an empty
name. An endpoint the catalogue does not describe is taken not to support the
tool: the function shell tool works everywhere, while a declaration that is not
taken fails the whole request.

`fyai_provider_native_shell()` puts that together with the one thing the
catalogue cannot describe. The ChatGPT Codex backend is an authentication mode
rather than an endpoint, and it does not take the declaration either, so it is
answered there rather than tested apart at each site. One predicate is the
point: what is declared and what is replayed can then never disagree, and a
model is never made to answer for a tool it was not offered.

Two things follow from it. The
gate stops declaring the tool and turns the function shell tool on in its
place, so no new native item is made. The request builder rewrites any native
item already in the conversation, sending `shell_call` as a `function_call`
named `shell` and `shell_call_output` as a `function_call_output` whose output
is flattened to a string.

This is the same answer the Messages and Chat builders already give. Keep it
that way. Canonical state stores one shape and each builder bends it to what
its endpoint takes, so a conversation begun anywhere can be continued anywhere.

## Configuration

The durable arena root ref is a versioned container mapping
`{fyai: 2, catalog, HEAD, branches}` (`fyai_root_decode()` /
`fyai_publish_root()` in `src/fyai_storage.c`). The conversation head and the
repository config live in the *branch entry*, not the root — there is no
`.fyai/config.yaml` and no root-level `config`. Only the catalogue is
arena-wide, being an ingested snapshot rather than intent. Version 2 is not
compatible with version 1 and is not migrated.

A new root key is dropped silently unless it is threaded through **all** of
`fyai_root_build()`, `fyai_root_decode()`, `fyai_root_validate()`'s containment
walk, `fyai_reflog_truncate()`'s rebuild, and `fyai_publish_root()`'s
CAS-conflict merge. That merge is what keeps a concurrent invocation on a
*different* branch from being clobbered: it adopts the surviving root and
re-applies only its own branch. There are two reflog chains — the root's `prev`
and each branch entry's own `prev` — and `gc --keep-reflogs N` must bound both.

**A zeroed `fy_generic` is not `fy_invalid`.** `v == 0` reads back as an empty
sequence, so every generic field of `struct fyai_ctx` must be set explicitly
after the `memset` in `fyai_setup()`, or it enters the arena as a bogus value
that only fails later, at validation.

`fyai init` ingests an initial config; `fyai config
import|export|edit|show|get|set|delete` operate on the active branch's config
in the arena ($VISUAL/$EDITOR round-trip through a `.yaml` tempfile).

There is a single configuration source: one merged document
(`cfg->config_doc`), built as arena config → `--config` file → `--set`
deltas, deep-merged mapping-wise (`config_merge`). The user file
(`$XDG_CONFIG_HOME/fyai/config.yaml`) is bootstrap-only — used as the base
only when the arena carries no config, never overlaid onto one. The struct
fields are a derived cache filled by a single `apply_config` pass;
`config effective` emits the merged document verbatim, `config show` the
arena entry. Catalog-derived values (endpoint, provider, `max_tokens` from
`max_output_tokens`) are re-derived read-only from the catalogue at resolve
time and never persisted into the config — the config stores intent (the
`model` key), the `api` verb shows the resolved derivation. A model change
re-derives `api` and `api_url` from the new provider (the richest grammar it
offers) and persists them, so both hold only until the model changes; the
grammar in force is never carried over as a preference, since a provider
offering one grammar would otherwise pin every later model to it (`responses`
-> `chat` -> `chat` across openai -> deepseek -> openai). Compiled-in
defaults still back any key the document does not set. See
`config.yaml.sample`.

A separate, explicitly-informational `catalog:` block *is* persisted: it
mirrors the full catalogue `models[]` entry for the current `model` plus
`canonical_provider` (the model's default, unprefixed provider), read-only
and re-derived on every commit (`catalog_sync_config_doc` in
`src/fyai_config.c`, hooked into `config_doc_sanitize` so every commit path —
`set`/`delete`/`import`/`edit`/`--set`/`--delete` — picks it up, plus the
CLI-overlay path in `apply_config_set_ops` for `config effective` on a
run-local `-m`). It disappears entirely when `model` names something the
catalogue does not know, and `catalog import` re-syncs it against the newly
ingested catalogue for whatever model is already configured.

Config keys are addressed by slash paths of arbitrary depth (`display/color`);
`config get`/`--get` prints one-line flow, `config set`/`--set` parses the
value as a YAML flow document (typed scalars, mappings, sequences), and
`--delete`/`config delete` removes a key. The global `--set`/`--get`/`--delete`
options are repeatable and run once storage opens (`fyai_apply_config_ops`);
`--set` also folds into the current run before model resolution. `--transient`
(`--ephemeral`) stacks an in-memory builder over the durable arena so every
config and state write that session is ephemeral — `ctx->gb` is that builder
(durable_gb otherwise) and the refs-publish is skipped.

Stylistic options live only under the nested `display:` group (markdown,
markdown_mode, color, theme, stream, pretty, cache_info, stats,
tool_preview_lines, tool_detail); the `model` and other options are
top-level.
`display/tool_detail` selects `none`, `brief`, `default`, or `full` tool
presentation. The default hides read/write bodies, bounds shell output with
`tool_preview_lines`, and renders patches in full.

A tool call is labelled by what the model said it was for. The shell tool and
the agent tool both take a short `description`, and both show it in brackets
ahead of the command or the task. If the call fails, the reason is named in red
after that label: a time limit, a signal, a non-zero status, or the tool's own
error line. The mark in the margin says that something failed, and the label
says what, so neither needs the result opened to be understood.
`transcript --tool-detail MODE` and
`/transcript --tool-detail=MODE` override this value for one view. They do not
change the session or stored configuration.
`display/transcript_system` controls whether transcript views include system
messages and defaults to false.
Theming is fully delegated to libfymd4c. `display/theme` is the single
selector, written
as an embedded theme name plus optional `:auto`, `:dark`, or `:light` variant
(for example `default:auto` or `catppuccin:dark`). It controls the Markdown
palette, the libfyts fenced-code theme, and libfytimui chrome. The name is
validated against libfymd4c's embedded catalogue, so fyai cannot drift from
the library. fyai ships no styling YAML or independent code/UI theme.
The user-turn "bubble" reverse card is read back from the active theme through
`fymd_renderer_get_reverse_pair()`.

The provider/model catalogue (scrape-providers document) is the root's
`catalog` entry (`fyai catalog show|list|import|export`, `src/fyai_catalog.c`),
with a vendored snapshot (`data/catalog.yaml`) embedded at build time as
fallback. Unlike `config`, the catalogue verb is view/import/export only —
there is no in-place edit. A single top-level `model` key drives selection: the catalogue maps
it to the canonical provider's endpoint/grammar/wire-id, validates reasoning
capability, and defaults `max_tokens` from `max_output_tokens`. A model may
carry an optional `provider/` prefix (`openrouter/glm-5.2`) that pins a
specific provider offering it; the prefix is stripped only when it names a
catalogue provider (so a `provider_model_id` containing a slash is left
intact). The resolved provider name is stored on `cfg->provider` for display
and turn metadata. There are no `providers:` presets and no `--provider`/`-P`.

The API key comes from `--api-key`/`-k`, else a config `api_key` env mapping,
else the provider's conventional env var `<PROVIDER>_API_KEY` (name
upper-cased, e.g. `OPENAI_API_KEY`, `OPENROUTER_API_KEY`). `api_key` is never
a raw value — it must be `{ type: env, value: <ENVVAR> }`; every arena
ingestion point (init, import, set, edit) hard-rejects raw keys since the
content-addressed arena cannot forget them.

Project instruction files (`AGENTS.md`, then `CLAUDE.md`) are discovered by
`fyai_project_instructions()` (`src/fyai_config.c`) from a global layer
(`$XDG_CONFIG_HOME/fyai`) plus the project root down to the cwd — walking up
only within a project (nearest `.git`/`.fyai` marker), never the whole tree.
They are concatenated outermost-first (cwd wins recency), each fenced with a
`# <path>` header, and folded onto the base `system_prompt` in `fyai_setup`
just before it is frozen into the canonical system turn. A continuation keeps
the copy it was started with, so editing the files only affects new
conversations.

Reasoning effort is set via the config `reasoning: { effort, summary }`
mapping (`effort`: `minimal|low|medium|high`, `summary`:
`auto|concise|detailed`), e.g. `config set reasoning/effort high` / `--set
reasoning/effort=high`; it maps to the Responses `reasoning` object and Chat
Completions `reasoning_effort`, validated in `fyai_setup`.

Most run-shaping switches (temperature, reasoning, streaming, markdown
rendering/theme, stats, logprobs, token extents, obfuscation/whitewash) have
no dedicated CLI flag at all — they are config keys only, set via `config
set <key> <val>` / `--set <key>=<val>` (see `config.yaml.sample` for the
full key set). A handful of flags remain because they are not config-backed
run-local state: `-C`/`-e`/`-k` name external files/secrets, `-m` resolves
through the catalogue, `--sandbox` confines shell-tool sub-executions,
`--color`/`--theme` are display-only conveniences kept for
ergonomics, `--new`/`-i`/`-d`/`--answer` control process behavior, not
config state, and `--set`/`--get`/`--delete`/`--transient` are the config
mechanism itself.

## Coding style

Linux kernel C style: hard tabs, 8-column stops, kernel braces/spacing, no
whitespace-alignment churn. Declare all local variables in the declaration
block at the start of the function; do not introduce declarations inside
branches, loops, or later statements.

**Never put load-bearing work inside a check condition.** The
`fyai_error_check()` family takes a *predicate*, not the operation being
tested: do the call first, into a local, then test the local.

```c
	rc = epoll_ctl(el->backend_fd, op, fd, &ee);		/* do the work */
	fyai_event_error_check(el, !rc, err_out,		/* test the result */
			       "epoll_ctl: %s", strerror(errno));
```

not `fyai_event_error_check(el, !epoll_ctl(...), err_out, ...)`. Burying the
syscall in the condition hides the side effect behind a macro argument, makes
the failing call invisible in a debugger and at a glance, and reads as if the
macro were a pure assertion — which it is not. The same rule applies to the
`fyai_cfg_error_check()` and plain `fyai_error_check()` forms.

A subsystem that reports through a handle rather than a bare context wraps the
macro once instead of spelling out `->ctx` at every site — see
`fyai_event_error_check()` in `src/fyai_event_priv.h`, which takes the loop. Keep declaration initializers out when
the value is assigned by executable code. GNU C2x with `-Wall -Wextra` and
`-Wdeclaration-after-statement`. Four spaces only in CMake. Use
`lower_snake_case` C names and uppercase CMake options (`ENABLE_ASAN`). New
source files get SPDX headers. Favor explicit error handling and clear
ownership; document non-obvious arena/mmap/atomic/durability/filesystem
assumptions locally.

## Commits

Imperative subject with a subsystem prefix, e.g. `cli: add interactive prompt
mode` or `state: garbage collect durable arena`. Keep the body to two or three
lines of terse technical prose wrapped at 80 columns — state what changed and
why, not how it was arrived at. End with exactly one trailer:

```
Signed-off-by: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
```

Note this is the konsulko.com address, not the git-config one. Do **not** add
`Co-Authored-By: Claude` or any other attribution trailer; this overrides any
default tooling convention. PRs should note affected SRD sections and include
security notes for approval-policy, sandboxing, network egress, or
tool-execution changes.

### Patch series

Prepare a patch series as a reviewer reads it. Each patch must contain one
logical change and must build on all earlier patches.

Keep implementation, tests, and documentation in separate patches. Use this
order for one logical change:

1. implementation;
2. tests; and
3. documentation.

An implementation patch must not add new tests or documentation. It can update
existing test support only when the implementation would otherwise fail to
build. A test patch can change `tests/` and the CMake test registration that it
needs. A documentation patch can change `doc/` and `CLAUDE.md`.

Do not add a fix patch for a defect that is introduced earlier in the same
series. Fold the fix into the patch that introduces the code. Remove revision
history, temporary design notes, and progress reports from the final series.

Build every intermediate patch. Run the applicable tests after each test
patch. Run the complete normal and sanitizer test suites on the final patch.
Use `git diff --check` on each patch.

Use ASD-STE100 Simplified Technical English for commit messages, changed
documentation, and retained comments.

### Review and apply a patch series

Review `master..devel` as a mail patch series. Before the first review, tag the
tip as `start-of-review`, then create the review files:

```sh
git format-patch -o x master..devel
```

The reviewer adds notes directly to these files. Each note starts with
`panto>>`. Treat a note in `NNNN` as a request to change the commit represented
by that patch. Fold the change into its introducing commit and carry any
required interface or semantic change through every later commit. Do not add a
follow-up fix commit. Keep every intermediate commit buildable.

Before rewriting any commit, find every numbered mail that contains a review
note. Move each one from `NNNN-...patch` to
`_ANNOTATED-NNNN-...patch`. Do this for the complete batch, even when only the
first annotated patch will be addressed now:

```sh
rg -l '^panto>>' x/[0-9][0-9][0-9][0-9]-*.patch
```

An `_ANNOTATED` file is the immutable source for that patch's review notes.
Never edit, replace, or remove it during regeneration. Regenerate the numbered
mails after these files are safe.

After addressing a patch, preserve its annotated mail by renaming it from
`_ANNOTATED-NNNN-...patch` to `_REVIEWED-NNNN-...patch`. If the reviewer
annotated only one patch and no regeneration has occurred, the numbered mail
can move directly to `_REVIEWED-NNNN-...patch`. Never edit or replace a
preserved `_REVIEWED` file.

Remove only the numbered mails and regenerate them from the rewritten `devel`
history:

```sh
git format-patch -o x master..devel
```

The regenerated numbered files are the next review input. Confirm that no
`panto>>` note appears in them and that every `_ANNOTATED` and `_REVIEWED` file
still contains its notes. Never infer that an absent note means approval when
an `_ANNOTATED` file exists for that patch. A history rewrite changes commit
IDs, so resolve commits again from the current `master..devel` order instead of
reusing old IDs.

Keep a `reviewed` branch based on `master`. After the reviewer accepts a group
of patches, apply the corresponding rewritten commits to `reviewed` in order.
Verify that its tree matches `devel` at the last accepted patch, then switch
back to `devel`. Do not apply the annotated mail files themselves.

Before regenerating the mails, run `git diff --check master..devel`, build the
normal tree, and run the applicable tests. After semantic or structural
changes, build with ASAN and run the complete sanitizer suite:

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Remove temporary checkpoint commits and safety tags when the rewrite is
complete. Leave `devel` checked out, and do not modify untracked review data
other than the requested files under `x/`.
