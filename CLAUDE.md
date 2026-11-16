# CLAUDE.md

Use this file when you change this repository. The detailed rules are in
`doc/source-layout.md` (the owner of each file and component) and
`doc/terminal-ui-rules.md` (separation, documents, the page, the work pane,
tiles, themes). Read the applicable document before you change that area.

## Project

`fyai` is a stateless AI coding assistant written in C. One invocation runs
one complete tool-use loop, commits the canonical state, and exits. An
interactive invocation owns its terminal UI only while its process runs. The
one server is `fyai serve`, which the user starts and stops: it runs
invocations for clients and keeps no state of its own.

### Architecture rules

- Do not add a resident process or hidden process state. `fyai serve` (also
  `--daemon`) keeps no canonical state: its workers commit to the arena as a
  local run does.
- Store persistent manifests, references, configuration, and other structured
  state in content-addressed libfyaml arenas under `~/.fyai`. Project file
  bytes and the binary project manifests of a filesystem view can be
  immutable content-addressed files under the project `.fyai` directory; the
  arena holds a reference to each. Publish blobs and manifest files before
  arena references; never edit a published file in place.
- Keep canonical data immutable, deterministic, and address-stable between
  processes. Do not relocate an arena during normal operation.
- Tools read files, apply writes or patches, and run approved shell commands,
  compatible with Unix conventions.

### Comments

Write comments in concise Technical English. State the invariant, ownership,
or behavior. Do not narrate history, restate the code, or address the reader.
Remove a comment when the code is self-explanatory.

## Data model

Use libfyaml generics as the native data model. Do not construct JSON by
hand. Emit compact JSON only at provider boundaries with `FYOPEF_MODE_JSON`.
Parse provider responses directly into generics. Keep provider wire data
(request IDs, tool-call IDs, finish reasons, timestamps) in provider-stream
generics, and derive provider-independent content before you calculate the
canonical identity.

Use typed accessor defaults: `fy_get(obj, "content", "")`,
`fy_get(obj, "total_tokens", 0LL)`, `fy_get(obj, "items", fy_invalid)`.

### Lifetimes

- Prefer `fy_castp(&v, "")` to `fy_cast(v, "")`: a short string lives in the
  generic word, so a `fy_cast()` pointer can point into a local copy. Do not
  keep a cast pointer longer than its generic. A `const char *` loop variable
  of `fy_foreach*()` points into collection storage and is safe.
- A returned generic must name storage that outlives the function: a builder
  of the caller, or an arena value. Never return a value of a stack builder
  (`fy_mapping()`/`fy_sequence()` without a builder, `fy_sprintfa()`); do not
  free it. Copy a local C string into a builder with `fy_value(gb, s)`. State
  a borrowed lifetime in the header.
- `ctx->gb` and `ctx->durable_gb` are not stable across `fyai_arena_reopen()`.
  `ctx->transient_gb` is released on an idle loop iteration, so nothing
  asynchronous borrows from it.
- Do not copy a generic into the C heap for a consumer that can hold the
  generic or a borrowed pointer.
- Intern a long-lived bounded value (a name, an id, a path) into `cfg->gb`
  with `fyai_ctx_keep_str()`. Do not intern a value that is new for each
  turn, request, call or event, and never a token or secret
  (`fyai_secret_clear()` zeroes it; a builder never does). Those stay copies
  or live in a builder the object owns (`job->call_gb`, `p->gb`). Mutable
  buffers, environment paths, and strings for threads, fork children, curl
  and the transport stay copies.
- A value the process caches (a parsed embedded schema) lives in a builder of
  its own, never in the builder of the first caller.

### Short forms

- Use `fy_is_*()` predicates and `fy_empty()`. Keep `fy_generic_is_int()`,
  `fy_generic_is_float()`, `fy_generic_is_bool()`, which test the C range.
- Use `fy_foreach()`, `fy_foreach_key_value()`, `fy_foreach_idx_item()` in
  place of index loops; `fy_any_equal(v, "a", "b")` in place of `fy_equal()`
  chains; `fy_str(v)` (NULL for `fy_invalid`), `fy_number(v, dflt)`,
  `fy_str_empty(s)`.
- Build strings with `fy_stringf(gb, ...)` and `fy_join(gb, ...)`. For a stable
  `const char *`, use `fy_gb_intern_string(gb, fy_sprintfa(...))`.
- Use `fy_mapping(gb, ...)`/`fy_sequence(gb, ...)` and the other short forms
  in place of `fy_gb_*()`. `fy_gb_internalize()` and `fy_gb_intern_string()`
  have none.
- Do not nest `fy_mapping()`, `fy_sequence()`, `fy_assoc()` or `fy_get()` more
  than one level deep: the macro expansion multiplies (three levels made
  80 MB of preprocessed text and killed a CI runner). Build inner values into
  locals. Check with `-E` for a line that expands past a megabyte.

### Empty strings and initialization

An empty-string generic is a string, not null; the YAML emitter must write it
as `""`. Keep empty-string configuration keys `type: string` in
`data/config.schema.yaml`; do not allow null to hide an emitter defect.

A zeroed `fy_generic` is not `fy_invalid` (zero can be an empty sequence).
After `fyai_setup()` clears `struct fyai_ctx`, initialize each generic field
explicitly.

## Source layout

Sources are in one directory per component under `src/`; every directory is
on the include path. Put a new file in the directory of the component that
owns it. `doc/source-layout.md` names the owner of each file.

- `core/` entry point, engine, configuration, diagnostics, turn, attachments,
  tables, helpers. `loop/` event loop, timers, signals, waits. `store/` arena,
  branches, merge, project, blob store, import. `auth/` credentials, login,
  secret stores, OAuth. `provider/` model connection, transfers, grammars,
  catalogue. `net/` sockets, HTTP server, WebSocket, JSON-RPC, frames.
  `transport/` credential transport. `remote/` `fyai serve`, remote client,
  ACP, Claude SDK. `mcp/` MCP client. `tools/` tools, jobs, sub-agents,
  skills, questions. `sandbox/` confinement, filesystem views, exchange scope.
  `host/` OS primitives. `front/` sink, UI, page, work pane, Markdown,
  terminal. `cmd/` commands.
- `struct fyai_app` (`src/core/fyai_app.c`) owns the shared components in
  dependency order (`doc/context-components.md`); `main()` declares it and
  everything else borrows from it. The context is `app->ctx`; each component
  is reached as `ctx->NAME` (`arena`, `transient`, `link`, `loop`, `cred`,
  `out`, `uistores`, `screen`, ...).
- A component finds its context through an accessor. A member of
  `struct fyai_app` uses `container_of()` on the app (`fyai_cred_ctx()`). An
  object of which many stand on one context keeps a pointer and gives it
  through `NAME_ctx()`, defined next to the struct; set the field where the
  object is made and read it nowhere else (`tests/ctx-accessor.sh`).
- One implementation for each job, do not write a second: `struct fyai_buf`
  (`fyai_buf.c`) for every byte buffer; `fyai_httpd.c` for every HTTP
  listener, the mock provider included; `fyai_net.c` for sockets; `fyai_wsc`
  for WebSocket clients on the loop; `fyai_cred.c` for every login refresh.
- Platform code goes in a backend (`*_linux.c`, `*_macos.c`, `*_none.c`) of
  the host, sandbox, fsview, project capture and blob store. Test a
  capability (`fyai_host_memfd_available()`), never a platform name.
- Load a large embedded document with `fyai_image_load()` and keep the parse
  of its text as the fallback (`doc/embedded-images.md`).

## Tools

`data/tools.yaml` describes each model tool; `struct fyai_tool_def`
(`src/tools/fyai_tool_registry.h`) holds the run function, title row, result
formatter, ordering class and flags. Define the entry in the file that
implements the tool. Do not test a tool name elsewhere; add a field to the
entry (`doc/tools-yaml.md`).

An event is a line `[KIND 'NAME' TAIL]` with an optional body, queued for the
model. Its card is drawn by `fyai_event_pretty()`. Do not change the text the
model gets to make the card read better.

### Skills

A skill is branch state in the store member `skills` (`doc/skills.md`). It
enters only through `skill import`, `skill new`, or `skill edit`; do not
discover skills in the filesystem. `ctx->arena_skills` follows
`ctx->arena_config`. Check every front matter against
`data/skill.schema.yaml`.

A forked skill runs as a call of the `agent` tool through the `expand`
function of its entry; do not add a second sub-agent path. `ctx->tools_allow`
restricts its tools in both `make_tools_filtered()` and
`fyai_execute_tool_call()`. Skill files are written to a private `mkdtemp()`
directory in the process that runs its programs, never by a parent for a
child; `fyai_skill_dirs_release()` removes them, `fyai_ctx_fork_disown()`
forgets those of the parent.

## Commands

A verb (`fyai branch new x`) and a slash command (`/branch new x`) are one
command, defined once in `data/commands.yaml` (`doc/command-schema-plan.md`).
Do not add a verb table, a slash table, or a hand-written parser.

- `arguments` is a JSON Schema whose `x-fyai-*` keywords bind properties to
  the command line; `data/command.schema.yaml` checks the file. `surfaces` is
  `cli`, `session`, or both; a small difference tests `call->surface`.
- A handler `int fyai_cmd_NAME(struct fyai_cmd_call *call, fy_generic
  *result)` gets validated arguments with defaults: read them with
  `fyai_cmd_arg_str()`/`fyai_cmd_arg_bool()`; do not parse or type-check.
  Register it in `cmd_handlers[]`; hooks go in `cmd_earlies[]` and
  `cmd_prepares[]`. Ask the parsed command with `fyai_cmd_state_is()`; do not
  copy it into configuration fields.
- The caller presents the result. Build it in `call->gb` and describe it in
  `render` (`table`, `message`, `markdown`, `document`). Every verb takes
  `--output json|yaml` (`tests/cases/output_formats.sh`). A backend reports
  failures and returns data; it does not print its outcome. Only work that is
  presentation itself writes through the sink (history, stats, help, live
  UI, progress).
- `mode: stream` uses `fyai_cmd_emit()`; `mode: async` returns
  `FYAI_CMD_PENDING`, sets `call->cancel`/`call->cleanup`, and ends with
  `fyai_cmd_done()`.
- `flags`: `model` (sends a model request; only such a verb needs a
  credential), `model-state`, `no-storage`, `storage-optional`, `transient`,
  `interactive`. `while_busy`, `view`, `scope: view` and `setting: {key}`
  are described in `doc/command-schema-plan.md`. Mark a configuration key
  `x-fyai-scope: session` when a session may change it for itself.
- `x-fyai-complete` names a kind in `complete_kinds[]` of
  `fyai_cmd_complete.c`; a unit test fails on an unknown kind or handler.
  Test completion in `tests/fyai_cmd_test.c` and
  `tests/cases/completion_bash.sh`.
- Every command and argument has a description. After a change, run
  `ninja docs-commands`; `commands_reference` fails when `doc/commands.md` is
  stale.

## Attachments

`doc/media.md` is the design. Images are PNG, JPEG, GIF and WebP.

- The bytes are a binary string generic in the image part (`data`), in the
  arena with the message. Keep no copy outside it.
- `content` is a string for text only, else a sequence of parts:
  `{type: text, text}`, `{type: image, media_type, digest, width, height,
  bytes, data, name}`, and file parts `{type: text, text, file, bytes, lines}`.
- Do not walk `content`. Use `fyai_media_text()` (the words alone),
  `fyai_media_parts()` (the images), `fyai_media_model_text()` (what the model
  reads) and `fyai_media_text_references()` (what a person reads).
- Emit YAML with `FYOPEF_BINARY_BASE64`; do not encode bytes by hand. Import
  rebuilds image parts with `fyai_media_import_content()`.
- The wire form is derived by the `image_part` operation of each grammar.
  Image input is derived from `image` in the model `modalities`.
- An attachment enters only where the user names it: `@PATH`, `/attach`,
  `/image add|paste`, `--image`. `fyai_media_take_mentions()` reads only the
  line the user typed. `fyai_make_user_message()` takes waiting attachments;
  program-written messages use `fyai_make_instruction_message()`.
- The clipboard helper is a sanitized child with a time limit.

## Persistent state and branches

```text
{fyai: 3, HEAD, branches}
branches/<name>: {head, updated, op, from, prev, store}
store: {config, catalog, created, cwd, description, agent, import, ...}
```

- A branch entry is one ref-log record. Its `store` is an open mapping that
  holds rarely-changing branch state (configuration, catalogue, provenance).
  Put new branch state there; no sidecar files, no root-level configuration
  or catalogue. A version 2 root is not read.
- Build the store with `fyai_branch_store_build()`, which keeps unknown
  members; an unchanged store stays the same arena object. After a lost CAS,
  merge it with `fyai_branch_store_merge()`; a value both sides changed
  differently is a conflict under `branch/on_conflict`, named by path.
- `fyai_publish_root()` publishes through `fyai_publish_state()`; do not add
  a second reconciliation. Publish, reconciliation and
  `fyai_branches_refresh()` each set `ctx->branch_prev` from the adopted root.
- A publish that stops names the branch and says whether it lost the race or
  could not build the state.
- A new root key updates build, decode, validate, reflog truncation and CAS
  merge. GC limits the root and branch-entry `prev` chains.

### Branches and references

- The branch name is the hierarchy. Accept only symbolic references:
  `<branch>`, `<branch>~N` (stored turns), `<branch>^^`, `<branch>@{N}`. Never
  numeric object references: GC relocates objects.
- `--branch`, `-b`, `$FYAI_BRANCH` select a branch for one invocation. Only
  `checkout` changes stored `HEAD`. Keep `ctx->branch` separate from
  `ctx->head_branch`.
- An interactive run with no branch starts a `session/` branch with the
  configuration of the branch last updated in its directory, else of `HEAD`;
  `ctx->session_unstored` names it until its first exchange is published.
- Set the operation with `fyai_branch_op_set()`; a publish consumes it.
- A start point is conversation and configuration together: use
  `fyai_resolve_ref_state()`.
- `--root` is read-only: reject writes. Treat the handle as untrusted: walk
  the reflog comparing raw values, shape-check each entry, fully validate the
  match. Validate each next link with `fyai_branch_entry_contained()`.

### Merge and rebase

Walk a chain with `fyai_turn_foreach()` (bounded, detects cycles). Validate a
root before the peek path follows a reference. Conversations are append-only;
a join orders complete exchanges and keeps each question with its answer.
`rebase` puts ours after theirs; `merge` orders by reflog time (microseconds).
Keep one system prompt. Find the merge base by stored generic identity. After
a lost CAS, retry if only other branches changed, else apply
`branch/on_conflict`.

## Configuration and providers

Merge one configuration document in this order: the branch configuration, the
`--config` file, then `--set`. The user configuration file only initializes an
arena with no stored configuration. `cfg->config_doc` is the intent; populate
fields in one `apply_config` pass. Derive endpoint, provider and catalogue
`max_tokens` during model resolution and do not persist them; on a model
change persist the derived API grammar and URL.

- Commit every catalogue change through `fyai_catalog_commit()`, and keep the
  `model_info` block in sync (remove it when the model is not catalogued).
  `catalog update` runs `catalog_update/command` in a child that keeps only
  the credentials named in `catalog_update/credentials`.
- Give each new key of `data/config.schema.yaml` an `x-fyai-model` access
  (`hidden`, `read`, `review`, `write`). A key that weakens confinement, runs
  a program, sends data out, or lifts a model limit is `read` or `hidden`; one
  that hides model work from the user is `review` (`doc/declarative-state.md`).
- Validate `--set` and `--config` against the schema before merge; put the
  problems in the failing diagnostic.
- Paths are slash-separated; write names with `fyai_path_join()` and read
  segments with `fyai_path_segment_decode()` (JSON Pointer escapes). Values
  are YAML flow. `--transient` puts an in-memory builder above the arena.
- Never store a raw API key: `api_key: {type: env, value: VAR}`. Reject raw
  keys at every ingestion point.

### Transient failures

- `fyai_http_transient()` decides: retry refused/lost connections, timeouts,
  and HTTP 408, 429, 500, 502, 503, 504, nothing else. In a 200 stream,
  `fyai_provider_error_transient()` reads `code`/`type`; a spent quota is not
  transient.
- Double the delay up to `retry/max_delay_ms`, with jitter that is not
  `random()`. `Retry-After` replaces the delay, under the same maximum.
- Keep the text of a transient stream failure and raise it only if the last
  attempt fails. Process a parsed event before the curl write error it
  caused. Retry a stream only if it presented no content.
- Present a retry as a work band, repainted each attempt and closed once. A
  sub-agent sends its wait through `fyai_tool_progress_emit()`, never a status
  line.
- Cancellation and destruction of a request in backoff remove the timer and
  complete the request.

### Provider grammars

Responses, Chat Completions and Anthropic Messages; `api` selects one
(`doc/provider-grammars.md`). A grammar is a backend `struct fyai_grammar`.
Do not test `cfg->api_mode` outside the grammar files; ask `fyai_grammar(cfg)`
and call operations through `fyai_grammar_*()`.

- Normalize inbound items at the parse boundary into the canonical response
  (`fyai_response_canonical()`); store one canonical shape
  (`doc/canonical-format.md`) and adapt it in each request builder.
- Read provider differences and endpoint capabilities from the catalogue,
  never from the provider name. `fyai_config_resolve_model()` derives
  `cfg->shell_tool_supported`; `fyai_provider_native_shell()` is the one
  decision, and without native shell the builder rewrites stored native shell
  items to function calls.
- Requests must keep the provider cache prefix: `tests/fyai_grammar_test.c`
  and `grammar_cache_prefix_*` check it.
- A hosted tool runs at the provider; present it with
  `fyai_present_hosted_call()`, never through the local tool path.
- The session importer reads foreign sessions through the `history`
  operation; keep wire shapes out of it.

## Output and terminal UI

`doc/terminal-ui-rules.md` holds the full rules. Read it before you change
`src/front/`.

- `src/front/fyai_sink.c` is the only rendering component. Send every
  user-visible byte through it; producers give Markdown, the sink presents.
  `tests/sink-only.sh` enforces this (`tests/sink-only-allow.txt`).
- Streams say what content is: `TRANSCRIPT`, `NOTICE` (command results,
  tables), `STATUS` (banner, spinner, usage, tool echo), `DIAG`, `MACHINE`
  (never decorated). Use `fyai_result()`, `fyai_report()` (plain text),
  `fyai_sink_markdown()`, `fyai_sink_write()`.
- A backend implements `struct fyai_sink_ops`; any entry can be NULL and a
  producer does not examine the destination. While the UI is active the
  terminal backend presents through `fyai_ui_present()`, never stdout.
- `src/front/fyai_flow.c` is the one separation policy. A producer states its
  unit with `fyai_sink_unit()` and never counts rows; every path that commits
  bytes calls `fyai_flow_observe()`.
- `src/front/fyai_output.c` owns the transcript documents and stores them as
  `display_outputs`. Do not create a second transcript renderer; tool title
  rows are drawn only by `markdown_render_tool_head()`. A sub-agent or tool
  child presents nothing on fd 1 (`sink_may_present()`) and never moves a
  stream to stderr.
- Do not call `fytim_pump()` from a render path; set `frame_pending`.
- The page (`fyai_page.c`, `data/page.yaml`) is rendered every frame from
  `struct fyai_page_state` only; do not cache it. Record
  `tests/data/page-source-golden.txt` with `FYAI_PAGE_GOLDEN_WRITE` when the
  source changes, and say why in the commit.
- `src/front/fyai_workpane.c` owns pane geometry, focus and zoom;
  `src/front/fyai_chrome.c` owns tile chrome. Producers state intent and size
  nothing. Use `fyai_sink_band_*()`, never `fyai_ui_*` band functions.
- Make every Markdown renderer with `markdown_renderer_new()`. Colours,
  glyphs and gutters come from the theme roles; never put a colour escape or
  a list of theme names in C. libfypalette is optional.
- Query the terminal only through `fyai_terminal_probe()`.
- Render every Markdown table with `fyai_generic_to_markdown()` and a
  `renderopts` generic (`stats` keeps its own). Export
  (`fyai_export_view()`) is outside the sink and must work without
  `display_outputs`.

### Interactive rules

Test terminal input, signals and resize under a PTY; non-TTY tests use the
blocking fallback. `/` starts a slash command, `//` sends a literal slash.
Ctrl-C discards input, ends idle input, or cancels a turn; Escape and SIGINT
call `fyai_ui_interrupt()`. Keep `ISIG` enabled. `/resume` does not move
HEAD; `/checkout` does, and a historical reference creates a `session/`
branch. `/reset` and `/rewind` move the branch head. `/btw` forks a sub-agent
onto `agent:btw-N`. Request-shaping settings persist through the commit path;
display settings stay session-only. `/compact` stores `compacted_from`.

## Event loop and process rules

Each invocation owns one lazy event loop; curl, shell capture, jobs, MCP,
readline and OAuth borrow it.

- A borrower removes every source it registered, keeps source pointers in the
  owner, clears them on retire, and makes cleanup idempotent.
- A child that forks without `exec` calls `fyai_ctx_loop_abandon()` and
  `fyai_ctx_fork_disown()`. Abandonment never calls `epoll_ctl()` on the
  shared object.
- No nested event loop in an interactive callback. Synchronous wrappers only
  in standalone commands, setup adapters and isolated tests.
- Test with and without object pooling (`FYAI_EVENT_NO_POOL`; off by default
  under sanitizers).

### Signals

Register SIGINT with `fyai_event_interrupt_open()`, never `signalfd`; a pump
that sees it calls `fyai_event_interrupt_ack()`. SIGALRM is the watchdog's.
`fyai_event_signal_owner()` is the only mutable global a handler reads; keep
other handler state in `struct fyai_loop`. Keep handlers async-signal-safe;
the SIGUSR2 dump writes from signal context with no allocation, stdio,
`tcgetattr()` or blocking write. Keep SIGPIPE blocked in the parent and
restore it before `exec`; set `CURLOPT_NOSIGNAL`. An interrupted turn keeps
its completed steps, wrapped with `fyai_with_diag()`/`fyai_report_diag()`.

## Tool jobs, shell capture, and time limits

- A forked tool child uses fds 3 and 4 (close-on-exec) for JSON-RPC, never
  stdio. MCP servers and `fyai agent --rpc` keep stdio.
- Before `exec`, close every fd above stderr with `fyai_close_fds_from()` and
  remove credentials with `fyai_env_sanitize()` in the child, failing closed;
  apply a configured `env` after it. Children use `/dev/null` stdin and
  `setsid()`; do not `setpgid()` in the parent.
- A forked child keeps the configuration, arena, sink and credentials, and
  drops everything that names a process, fd, timer or screen of the parent in
  `fyai_ctx_fork_disown()` (the UI model first). Add new live state there.
- A child serving a terminal session ends its program when its control
  channel closes.
- Set job fields after `fyai_tool_job_spawn()`, which clears the job. Cancel a
  tree with `fyai_event_add_child_terminate_group()`.
- Finish shell capture when the direct child is reaped, not at pipe EOF.
- Arm time limits in the parent with `fyai_tool_job_submit()`, per job, and
  remove the deadline when the job finishes. No second limit in the child.
  `max_timeout_ms` caps only model-supplied values. Order: shell `timeout`
  then `shell/timeout_ms`; agent `timeout`, persona `timeout_ms`,
  `agent/timeout_ms`. Pass the right object to
  `fyai_shell_timeout_requested()` (`timeout` vs `action.timeout_ms`).
- Only the parent reports expiration, in the normal result shape (native:
  outcome `{type: timeout, timeout_ms}`), keeping all output.
- A terminal view answers program queries through
  `fyai_terminal_view_reply_cb()`.
- Apply `workdir` before confinement (exit 125: unreachable). Keep command
  admission separate from filesystem confinement. Denies are carved out of
  every granted tree, or covered by `sandbox_cover` namespaces
  (`doc/apparmor-userns.md`). Fail closed on network egress:
  `fyai_sandbox_net_restrictable()`.
- Tool processes belong to the exchange scope (`doc/exchange-scope.md`):
  `fyai_scope_ensure()`, `fyai_scope_exchange()`, `fyai_scope_pin()`. Do not
  read a pid from a scope namespace. Do not use `tc.args` after a tool ran.

## Sub-agents

`fyai_agent_run()` takes `task`, `name` and `description`. A sub-agent owns
its persona, tools, conversation, curl handle and output routing.

- Store its conversation on a child branch (`main/agent:greeter`). Keep
  `fyai_branch_name_valid()` strict; `fyai_branch_name_ref_valid()` accepts
  `agent:`. Reject duplicate names (no ordinal); refresh branches before the
  test; reserve the name before spawn and copy it into the job after.
- Set persona fields after `fyai_arena_reopen()`; reopen before publish.
- On Linux the child executes `fyai agent --tool-child`
  (`doc/agent-fork-model.md`); `agent/spawn: fork`, transient runs and pinned
  roots keep the forked child. Keep both paths correct.
- In a view, pass the arena fd only to fyai (`fyai_fsview_arena_pass()`,
  `--view-arena-fd`); drop it elsewhere. Never mount it in the view.
- `context: fork` starts at the parent head with the persona as a user
  instruction (no second system turn); `context: fresh` sends only the task.
- Expose only configured persona names and descriptions. Resolve a persona
  model in a scratch configuration; keep user-set `api_url`/`max_tokens`.
- A direct sub-agent renders to its own terminal, shown on a surface by the
  parent (`cfg->agent_pty`). Grandchildren show compact progress.

## OAuth and JSON-RPC

OAuth is a state machine on the borrowed loop; the start function arms and
returns, the synchronous wait wraps it. Bind loopback redirects to
`127.0.0.1`, allow concurrent connections, 404 other paths. Provider specifics
stay in the caller. Configured MCP clients survive logout; dynamic ones are
removed. Release discovery state on every terminal path; login/logout cancel
active work.

A JSON-RPC connection owns its reader, writer and buffers and matches by
request ID. Remove a completed request from both lists before release. Every
path that ends a connection (an invalid frame included) settles each request
in `pending` and `flush_wait` and clears its connection pointer. A frame has
a hard size limit.

## Diagnostics

- Collect in `cfg->diag`; drain only at turn, verb and slash-command
  boundaries. Define `FYAI_MODULE` once per file; do not type prefixes.
- The first error is the cause; later ones are demoted until drain or reset.
  A path that stops reports its cause there, never only "failed".
  `fyai_diag_reset()` after recovery.
- The caller adds the name of a parallel sub-agent; copy names before the
  arena reopens. A forked child returns its diagnostics with
  `fyai_diag_take_generic()`; the parent uses `fyai_diag_adopt()` and adds a
  marker (`[main/agent:greeter] reason`), keeping existing ones. A child with
  no diagnostic: the parent states how it ended, in one diagnostic.
- Quote diagnostics with `fyai_diag_string()`, which does not remove them.
- The diagnostic sink has its own builder and fd, separate from the
  rendering sink. Normal output goes to the rendering sink.
- Parse provider JSON with `FYOPPF_COLLECT_DIAG`; report with
  `parse_diag_text()`.
- `(void)` only on cleanup, a callee that reported, or a non-status result.
  Content failures reach the user; display failures are warnings through a
  per-component wrapper, once per loop. Dropped user input is reported.
- Report resource failures (allocation, builder, surface, request,
  connection) with `fyai_error_check()`, naming what could not be made.
- `$FYAI_TRACE` (`1`/`on` or a path) and `$FYAI_TRACE_LEVEL` write one line
  per record with one `write(2)` to an append-only, close-on-exec, high fd.
  `fyai_diag_trace_reopen()` after closing fds; `fyai_diag_trace_tag()`,
  `fyai_diag_tracef()`. Tracing fails silently.

## Build and test

```sh
cmake -S . -B build -G Ninja && ninja -C build
ninja -C build parallel-test          # full suite
./build/fyai_test --list; ./build/fyai_test oauth/plain_redirect
ctest --test-dir build -R fyai/unit/oauth
cmake -S . -B build-asan -G Ninja -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
```

- Use ASAN for parser, storage, tool, event and YAML changes; the full
  functional suite must pass under it. `FYAI_TIMEOUT_SCALE` scales deadlines
  only, never pacing.
- The unit binary links production sources except `main.c`; no stubs. Declare
  tests with `FYAI_TEST_ENTRY(suite, name, entry)` in `tests/fyai_*_test.c`.
- Functional cases are hermetic: localhost, private `HOME` and `XDG_*`.
  Scope process searches to the run; never `pkill` a global pattern. A case
  that kills fyai ends the programs it recorded.
- PTY cases are YAML scripts for `fyai __pty-driver` via `pty_run`
  (`doc/pty-driver.md`); no `FYAI_PTY_` variables (`tests/case-lint.sh`).
  Wait on screen state (`wait-screen`, `wait-row`, `wait-gone`,
  `wait-repaint`, `wait-copy`, `frame`), not raw bytes. A key that changes
  nothing is sent in one `write` with the next key. Drivers read the PTY for
  the whole run and answer DA1.
- Never pace with a delay (`sleep`, scenario `delay`) unless the case tests a
  wait. Order steps by state: `wait-file` on `served`, `mock_watch` +
  `wait_requests N`, FIFOs outside the project, `tests/wait_exit.py`, mock
  `wait_for:` markers (fail on `stream-wait-failed`), and `match` to select
  replies.
- Before pushing a PTY or timing change, run
  `ctest --test-dir build -j96 --repeat until-fail:5`. Fix the order; do not
  raise a deadline.
- Static modes: `FYAI_MOSTLY_STATIC` (only libc, libm, loader in `ldd`),
  `FYAI_MUSL_STATIC` (not a dynamic executable), `FYAI_STATIC` (legacy).
- When correctness depends on cells, wrapping, cursor, fill or SGR, test with
  libfyvterm: two `struct fyvt_screen`s, direct libfymd4c output vs the
  libfytimui path; compare colours with `fyvt_screen_convert_color_to_rgb()`
  and `fyvt_color_is_equal()`, never `memcmp()`. The test fails before the
  fix and is its own CTest test; run both libfytimui and fyai suites.

## C style

Linux kernel style: hard tabs (8), kernel braces, locals at the start of the
function and never inside branches or loops, `lower_snake_case`, uppercase
CMake options with four-space indent, GNU C2x with `-Wall -Wextra
-Wdeclaration-after-statement`, SPDX headers, no whitespace-only alignment
changes. State ownership and non-obvious arena, mmap, atomic, durability and
filesystem assumptions near the code.

Do not put an operation inside an error-check predicate; store the result,
then test it:

```c
	rc = epoll_ctl(el->backend_fd, op, fd, &ee);
	fyai_event_error_check(el, !rc, err_out,
			       "epoll_ctl: %s", strerror(errno));
```

Handle errors with `goto` to one label at the end that releases and returns.
Use `fyai_error_check()` when a `ctx` is available; name labels for what
they do when there are several. Define one wrapper macro for a subsystem
that reports through another handle.

Use ASD-STE100 Simplified Technical English for documentation, comments and
commit messages.

## Commits and patch series

Imperative subject with a subsystem prefix (`cli: add interactive prompt
mode`), two or three short body lines (what and why), wrapped at 80, ending
with exactly:

```text
Signed-off-by: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
```

No other attribution trailer. One logical change per patch, each buildable,
in the order implementation, tests, documentation. Fold fixes into the patch
that introduced the defect. No temporary notes. Run `git diff --check` on
each patch, tests after each test patch, and normal and ASAN suites on the
final patch.

## Patch review workflow

Review `master..devel` as a mail series. Before the first review, tag the tip
`start-of-review` and run `git format-patch -o x master..devel`.

- Notes start with `panto>>`. Before rewriting, find them with
  `rg -l '^panto>>' x/[0-9][0-9][0-9][0-9]-*.patch` and move every annotated
  file to `_ANNOTATED-NNNN-...patch` before regenerating anything. Never edit,
  replace or remove `_ANNOTATED` or `_REVIEWED` files.
- Fold each change into the commit of its patch and carry it through later
  commits; keep each commit buildable; re-resolve commits after each rewrite.
- An accepted patch's preserved file becomes `_REVIEWED-NNNN-...patch`. Remove
  only numbered files before regenerating; confirm new files have no
  `panto>>` notes and preserved files keep theirs.
- Keep branch `reviewed` on `master` with accepted commits applied in order;
  its tree matches `devel` at the last accepted patch. Never apply annotated
  mail files. Switch back to `devel`.
- Before regenerating: `git diff --check master..devel`, build, relevant
  tests; full ASAN after semantic or structural changes. Remove checkpoint
  commits and safety tags when done. Leave `devel` checked out. Touch no
  untracked review data except the requested files under `x/`.
