# CLAUDE.md

Use this file when you change this repository.

## Project

`fyai` is a stateless, daemon-less AI coding assistant written in C. One
invocation runs one complete tool-use loop, commits canonical state, and exits.
An interactive invocation owns its terminal UI only for its process lifetime.

### Architecture rules

- Do not add a daemon, resident process, or hidden process state.
- Store persistent state only in content-addressed libfyaml arenas under
  `~/.fyai`.
- Keep canonical data immutable, deterministic, and address-stable between
  processes.
- Do not relocate an arena during normal operation.
- Keep tools Unix-shaped: read files, apply writes or patches, and run shell
  commands under approval.

## Data model

Use libfyaml generics as the native data model. Build values with functions
such as `fy_gb_mapping()` and `fy_gb_sequence()`. Do not construct JSON by
hand. Emit compact JSON only at provider boundaries with `FYOPEF_MODE_JSON`.
Parse provider responses directly into generics.

Keep provider wire data, such as request IDs, tool-call IDs, finish reasons,
and timestamps, in provider-stream generics. Derive provider-independent
content before you calculate canonical identity.

Use typed accessor defaults:

```c
fy_get(obj, "content", "")
fy_get(obj, "total_tokens", 0LL)
fy_get(obj, "items", fy_invalid)
```

### Generic string lifetime

Prefer `fy_castp()` to `fy_cast()`. A short string can be stored inside the
`fy_generic` word. A pointer returned by `fy_cast(v, "")` can therefore point
into the local copy of `v`. That pointer becomes invalid when the copy leaves
scope. Use `fy_castp(&v, "")` at the use site, with a long-lived generic. Do
not cache a cast pointer beyond the lifetime of its generic.

A `const char *` loop variable of `fy_foreach()`, `fy_foreach_key_value()` or
`fy_foreach_idx_item()` is safe. The typed accessor takes the address of the
stored item, so the pointer is into collection storage, not into a copy.

### Short forms

Use the short generic API. It says the same thing with less text:

- `fy_is_string()`, `fy_is_mapping()`, `fy_is_valid()` and the other
  `fy_is_*()` predicates, and `fy_empty()` for an empty collection or string.
  Keep `fy_generic_is_int()`, `fy_generic_is_float()` and
  `fy_generic_is_bool()`, which also test the C value range.
- `fy_foreach()`, `fy_foreach_key_value()` and `fy_foreach_idx_item()` in
  place of an index loop with `fy_get_at()` and `fy_get_key_at()`.
- `fy_any_equal(v, "a", "b")` in place of a chain of `fy_equal()` tests. It
  evaluates the value one time.
- `fy_str(v)` for a printable string and `fy_number(v, dflt)` for a number.
  `fy_str()` returns NULL for `fy_invalid`.
- `fy_stringf(gb, ...)` and `fy_join(gb, ...)` to build a string generic in a
  builder arena. To get a stable `const char *`, keep
  `fy_gb_intern_string(gb, fy_sprintfa(...))`; there is no interning format
  function.
- `fy_str_empty(s)` for a NULL or empty C string.

### Empty strings

An empty-string generic is a string, not null. The YAML emitter must write an
empty string as `""` when the style is not set. A bare empty scalar, such as
`key:`, can parse as null under the core and YAML 1.1 schemas.

Keep empty-string configuration keys as `type: string` in
`data/config.schema.yaml`. Do not allow null to hide an emitter defect.

### Generic initialization

A zeroed `fy_generic` is not `fy_invalid`. Zero can decode as an empty
sequence. After `fyai_setup()` clears `struct fyai_ctx`, initialize every
generic field explicitly.

## Source layout

- `src/main.c`: global option parsing and command dispatch.
- `src/commands.c`: verb definitions, usage output, and the main runner.
- `src/fyai.c`: engine orchestration.
- `src/fyai_sink.c`: the one rendering component and its backends.
- `src/fyai_output.c`: transcript document source and fragments.
- `src/fyai_session.c`: interactive input and slash commands.
- `src/fyai_agent.c`: sub-agent execution and agent RPC.
- `src/fyai_event*.c`: the portable event loop and signal handling.
- `src/fyai_event_dump.c`: SIGUSR2 event-loop diagnostics.
- `src/fyai_render.c`: generic-to-Markdown table rendering.
- `src/fyai_diag.c`: collected diagnostics.
- `src/utils.c`: HTTP buffers, shell capture, and generic serialization.
- `src/fyai_sandbox.c`: Linux Landlock confinement.
- `src/fyai_oauth.c`: provider-independent OAuth browser flow.
- `src/fyai_jsonrpc.c`: JSON-RPC over standard I/O and HTTP.
- `src/fyai_config.c`: layered configuration and config commands.
- `src/fyai_branch.c`: branch storage, references, and root selection.
- `src/fyai_merge.c`: branch merge and rebase.
- `src/fyai_catalog.c`: provider and model catalogue.
- `src/*.h`: shared structures and internal interfaces.
- `data/`: embedded configuration schemas and catalogue data.
- `tests/`: unit tests, functional cases, mock providers, and scenarios.

## Persistent state and branches

The durable root has this versioned shape:

```text
{fyai: 2, catalog, HEAD, branches}
```

Each branch entry owns its conversation head and configuration. The root owns
the catalogue. There is no sidecar configuration file and no root-level
configuration value.

When you add a root key, update all root build, decode, validation, reflog
truncation, and CAS-conflict merge paths. A concurrent publish must preserve
changes to other branches. Bound both reflog chains during garbage collection:
the root `prev` chain and each branch entry `prev` chain.

### Branches and references

- A branch name defines the hierarchy. Do not add a separate branch tree.
- Accept only symbolic references: `<branch>`, `<branch>~N`, `<branch>^^`, and
  `<branch>@{N}`.
- Never add numeric object references. Garbage collection can relocate arena
  objects.
- `~N` counts stored turns. One user and assistant exchange normally counts as
  two turns.
- `--branch`, `-b`, and `$FYAI_BRANCH` select a branch for one invocation.
- Only `checkout` changes stored `HEAD`.
- Keep `ctx->branch` separate from `ctx->head_branch`.
- Store the operation that produced each reflog entry. Do not infer it from
  head changes.
- Set the operation with `fyai_branch_op_set()`. A publish consumes and clears
  it.

A branch start point includes both the conversation and the configuration at
that point. Use `fyai_resolve_ref_state()` for `branch create` and
`checkout -b`. Do not combine a conversation from one state with configuration
from another state.

`--root` pins one published root and makes the invocation read-only. Reject
writes. Do not silently convert them to transient writes. Treat the supplied
handle as untrusted input. Find it by walking the root reflog and comparing raw
values. Run a cheap shape check for each entry, then run full validation once
on the matching entry. Keep full root validation shallow. Code that walks a
reflog must validate each next link with `fyai_branch_entry_contained()`.

### Merge and rebase

Conversations are append-only. A join chooses an order; it does not reconcile
text edits. Work in complete exchanges so that a question stays with its
answer. `rebase` puts our exchanges after theirs. `merge` orders exchanges by
their reflog time.

Read an exchange time from the reflog entry for the head that ends the
exchange. Keep timestamps at microsecond precision. Keep one system prompt.
Use stored generic identity to find the merge base.

After a lost publish CAS, retry if only other branches changed. Apply
`branch/on_conflict` if the current branch changed.

## Configuration and providers

Build one merged configuration document in this order:

1. the active branch configuration;
2. the explicit `--config` file; and
3. command-line `--set` changes.

Use the user configuration file only to initialize an arena that has no stored
configuration. Do not overlay it on an existing arena configuration.

Treat `cfg->config_doc` as the source of configuration intent. Populate struct
fields in one `apply_config` pass. Derive the endpoint, provider, and catalogue
`max_tokens` at model-resolution time. Do not persist these derived values.
When the model changes, derive and persist the API grammar and URL for the new
provider.

Keep the informational `catalog` block synchronized on every configuration
commit. Remove it when the selected model is not in the catalogue.

Configuration paths use slash-separated keys. Parse values as YAML flow
documents. `--transient` places an in-memory builder above the durable arena
and skips reference publication.

Never store a raw API key. The `api_key` setting must have this form:

```yaml
api_key: {type: env, value: ENVIRONMENT_VARIABLE}
```

Reject raw keys at every arena ingestion point.

### Transient failures

A provider request that failed for a transient reason is made again after a
backoff. `fyai_http_transient()` is the one decision for which failures pass:
a refused or lost connection, a timeout, and HTTP 408, 429, 500, 502, 503 and
504. Every other status is the provider's answer and must not be repeated.

A status does not see every transient failure. On a streamed endpoint the
connection succeeds, the status is 200, and a rate limit arrives as an error
event inside the stream. `fyai_provider_error_transient()` reads the `code` and
`type` of the error object for that case. A spent quota is deliberately not in
the set: it does not refill while a backoff runs.

Hold the text of a transient in-stream failure in the stream and raise it only
when no attempt is left. A turn that recovers must raise nothing, and a turn
that does not must read the same as it did before it was held.

An event handler that stops a stream makes curl report a write error. Decide on
the parsed event before that transport error, which describes only how the
stream stopped.

The delay doubles with each attempt, stops at `retry/max_delay_ms`, and keeps a
random part so that requests refused together do not return together. A
`Retry-After` header replaces the calculated delay under the same ceiling. Do
not use `random()` for the jitter: nothing seeds it, so each process would draw
the same sequence.

A delegated sub-agent sends its wait through `fyai_tool_progress_emit()`, so it
reaches the parent's band for that delegation. A status line from a child would
land on the parent's terminal raw, beside the display instead of inside it.

Present a retry as a work band through the sink, not as a loose status line: a
wait is a step of the running turn. Repaint the band for each attempt and close
it once, at the single notify point every terminal path reaches. A backend with
no bands keeps the plain status line.

Retry a streamed request only when it presented nothing. A transport error in
the middle of a stream keeps the prose the user has already seen, and a second
attempt would draw it one more time.

A request that waits out a backoff has no transfer to cancel. Cancellation and
destruction must retire the timer and settle the request, or an interrupt has
nothing to act on.

### Provider grammars

The supported grammars are Responses, Chat Completions, and Anthropic
Messages. The `api` configuration key selects the grammar. Do not add separate
grammar flags.

Normalize inbound provider items at the parse boundary. Store one canonical
shape. Adapt that shape in each request builder so that a conversation can
continue on another provider.

Read endpoint capabilities from the catalogue. Do not infer them from a
provider name. `fyai_config_resolve_model()` derives
`cfg->shell_tool_supported` for the active endpoint. An endpoint with no
capability declaration does not support native shell calls.

Use `fyai_provider_native_shell()` as the single decision for native shell
support. If native shell is unavailable, declare the function shell tool and
rewrite stored native shell items to function-call items in the request
builder.

## Output and terminal UI

`src/fyai_sink.c` is the one rendering component. Every byte the user sees goes
through it. A producer builds Markdown source and hands it over; the sink
decides how to present it. Do not write to standard output or standard error
from anywhere else. `tests/sink-only.sh` fails the build on a stray write, and
`tests/sink-only-allow.txt` names the few files that cannot use the sink and
states why for each.

### Streams

A stream says what the content is, not where it goes:

- `FYAI_SINK_TRANSCRIPT`: conversation content.
- `FYAI_SINK_NOTICE`: verb and slash command results, and tables.
- `FYAI_SINK_STATUS`: banner, spinner, usage, tool echo. Commentary.
- `FYAI_SINK_DIAG`: drained diagnostics.
- `FYAI_SINK_MACHINE`: bytes another program parses. Never rendered or
  decorated.

Use `fyai_result()` for a verb result, `fyai_report()` for a status line,
`fyai_sink_markdown()` to render, and `fyai_sink_write()` for bytes that are
already in their final form. `fyai_result()` and `fyai_report()` write plain
text: a model name or a path inside a status line must not be read as markup.

### Backends

A backend is the presentation policy behind `struct fyai_sink_ops`. The
terminal backend repaints in place and owns the sole progressive Markdown
renderer in the process. The capture backend keeps what it was asked to
present; it is the substrate a document backend such as HTML is written
against, and it lets a test read back what a run would have shown. Every entry
point may be NULL: a backend that cannot present something makes the sink
discard it, so a producer never has to ask what the destination is.

### Documents

`src/fyai_output.c` owns the durable half of a transcript document: the
Markdown source and its fragments. Keep one tagged document open for each
system, user, or assistant output, and keep an assistant document open across
the complete model and tool loop. Store the final document as
`display_outputs`. Replay these documents for history. Reconstruct legacy
arenas from message and provider data only as a fallback.

- Add generated text with `fyai_output_printf()`.
- Add provider bytes with `fyai_output_append()`.
- Add source another path has already drawn with
  `fyai_output_append_recorded()`. A tool exchange is recorded this way: the
  tool path presents it, and presenting it again from the document would draw
  it twice.
- A tool exchange stores a `tool_head` fragment over its title row, with the
  outcome of the call. Replay draws the state mark and the failure cause from
  that fragment through `markdown_render_tool_head()`, the one renderer the
  live work band also uses. Do not draw a tool title row anywhere else.
- Presentation mode is fixed when the document opens: live, one shot, or
  passthrough. Do not infer it later from the current state; a paused document
  that closes would then present everything a second time.
- A delegated sub-agent and a forked tool child present nothing. They own file
  descriptor 1 for JSON-RPC frames. The sink keeps that promise centrally:
  `sink_may_present()` decides it, the document path and the direct write and
  Markdown paths all ask it, and a stream bound for descriptor 1 in such a
  process is dropped. Do not move it to standard error instead; that trades a
  corrupted frame for a corrupted display in the parent.
- Do not create a second transcript renderer.
- Do not call `fytim_pump()` from a render path. Set `frame_pending` and let
  the UI owner paint.

### Work bands

A work band is a sink object. Ask `fyai_sink_bands_available()` before you
choose a banded presentation, open with `fyai_sink_band_open()`, repaint with
`fyai_sink_band_paint()`, and commit the shared band with
`fyai_sink_band_close()`. Do not call `fyai_ui_*` band functions from a
producer; the terminal backend owns that.

### Tables

Render every Markdown table with `fyai_generic_to_markdown()`. Pass a
`renderopts` generic for titles, selected keys, names, alignment, and formats.
Build `renderopts` in the transient builder or in the same stack frame as the
render call. Do not return a builder-less `fy_mapping()` from a helper because
it uses stack storage. `stats` keeps its own table builder: its cells carry
currency and percent formatting that `renderopts` does not express.

Export is deliberately outside the sink. `fyai_export_view()` serializes
canonical messages to a `FILE *` in the `format: 1` textual grammar that
`src/fyai_import.c` reads back, and it must keep working when a conversation
has no stored `display_outputs`.

### Interactive rules

The interactive reader uses the shared event loop. Test terminal input,
signals, and resize behavior under a PTY. Non-TTY functional tests use the
blocking fallback and do not cover the event-driven path.

In the REPL, `/` starts a slash command and `//` sends a literal slash. Ctrl-C
discards nonempty input, ends idle input, or cancels an active turn. Escape and
SIGINT call `fyai_ui_interrupt()`. Keep `ISIG` enabled.

Request-shaping slash settings persist through the common commit path. Display
settings remain session-only. `--new` has the same state effect as `/clear`.
`/compact` makes one tools-disabled summary request and stores the previous
head as `compacted_from`.

## Event loop and process rules

Each invocation owns one lazy `struct fyai_ctx` event loop. Curl, shell
capture, tool jobs, MCP shutdown, readline, and OAuth borrow that loop.

- Each borrower must remove all sources that it registered.
- Store source pointers in the owner and clear them when callbacks retire a
  source. Make cleanup idempotent.
- A child that forks without `exec` must call `fyai_ctx_loop_abandon()`.
- Abandonment closes the child's descriptor copies. It must not call
  `epoll_ctl()` on the shared epoll object.
- Do not run a nested event loop from an interactive callback.
- Use synchronous wrappers only in standalone commands, setup adapters, and
  isolated tests that own the top-level loop.

Sanitizer builds disable event-loop object recycling by default. Test both
paths. Set `FYAI_EVENT_NO_POOL=1` to disable pooling in a normal build. Set it
to `0` to enable pooling under ASAN.

### Signals

Do not register SIGINT with `signalfd`. Use
`fyai_event_interrupt_open()`. The handler sets the pending flag, wakes the
loop, and starts the watchdog. A pump that sees the interrupt must call
`fyai_event_interrupt_ack()`. Reserve SIGALRM for the watchdog.

Keep SIGPIPE blocked in the parent. Restore the disposition in children before
`exec`. Set `CURLOPT_NOSIGNAL` on curl handles. Keep signal handlers
async-signal-safe.

An interrupted turn keeps its completed steps. Wrap the partial turn with the
diagnostic indirect through `fyai_with_diag()` and `fyai_report_diag()`.

## Tool jobs, shell capture, and time limits

A forked tool child carries its JSON-RPC control channel on descriptors 3 and
4, not on standard input and output. It is forked and never exec'd, so the
numbers are a private contract between the two sides. Keeping frames off
descriptor 1 means a stray write lands on the terminal, where it is untidy,
instead of inside a frame, where it makes the parent read a fragment and lose
the frame around it. The channel is close-on-exec, so a command the shell tool
runs cannot reach it.

The standard descriptors of an MCP server and of the `fyai agent --rpc` verb
are not ours to choose: those peers are separate programs whose contract is
standard input and output. There the sink is what keeps descriptor 1 clean.

Close every descriptor above standard error before exec. A command must not
hold the event loop, curl sockets, arena files, a control channel or a sibling
job's pipes. Close the range rather than track each descriptor, so a new one
cannot become a leak by being forgotten. It sends `tool/progress` notifications and
returns `{result, ok}`. It uses `/dev/null` as standard input and calls
`setsid()`. Do not call `setpgid()` in the parent.

Set job fields after `fyai_tool_job_spawn()` because that function clears the
job. Cancel a complete process tree with
`fyai_event_add_child_terminate_group()`.

Finish shell capture when the direct child is reaped. Do not wait for pipe EOF;
a shell descendant can keep a pipe open. Keep the shell child in the tool
job's process group.

Arm time limits in the parent with `fyai_tool_job_submit()`. A time limit
belongs to the complete job, not to one child command. Do not arm a second
child-side limit when `cfg->tool_child` is set.

`timeout_ms` is the user default. `max_timeout_ms` limits a value supplied by
the model. Apply the ceiling only to a model-supplied value. Use this order:

- Shell: call `timeout`, then `shell/timeout_ms`.
- Agent: call `timeout`, persona `timeout_ms`, then `agent/timeout_ms`.

The function shell tool uses argument `timeout`. A native Responses
`shell_call` uses `action.timeout_ms`. Pass the correct object to
`fyai_shell_timeout_requested()`.

Only the parent reports expiration. Preserve the normal result shape. A native
shell result remains a list of `{stdout, stderr, outcome}`. On expiration, set
the outcome to `{type: timeout, timeout_ms}` and keep all captured output. Do
not replace the result with an error string or a bare signal outcome.

Apply shell `workdir` before Landlock confinement. Exit status 125 reports an
unreachable working directory. Landlock is Linux-only. Other platforms use
the same interface with no confinement backend. Keep command admission
separate from filesystem confinement.

## Sub-agents

`fyai_agent_run()` takes the full call arguments: `task`, `name`, and
`description`. A sub-agent owns its persona, tools, conversation, curl handle,
and output routing.

- Store each sub-agent conversation on a child branch such as
  `main/agent:greeter`.
- Keep `fyai_branch_name_valid()` strict for user-created names.
- Let `fyai_branch_name_ref_valid()` accept the reserved `agent:` marker.
- Reject a duplicate agent name. Do not add an ordinal automatically.
- Refresh the branch table before you test a name.
- Reserve the name in the parent before `fyai_tool_job_spawn()`.
- Copy the name into the job after spawn.
- Set child persona fields after `fyai_arena_reopen()`.
- Reopen the arena in a forked child before it publishes.

`context: fork` starts at the parent head. `context: fresh` sends only the
task. In fork mode, add the persona as a user instruction message. Do not add a
second system turn.

Expose only configured persona names and descriptions in the tool schema.
Resolve a persona model through the catalogue in a scratch configuration.
Preserve user-set `api_url` and `max_tokens`; replace only derived values.

Stream delegated progress to one Markdown quote in the parent work band.
Flush each tool-call update before the tool starts. Hide agent tool results.
Use the final report as the last quote update.

## OAuth and JSON-RPC

Implement OAuth as a state machine on the borrowed event loop. The start
function must arm the flow and return. Keep the synchronous wait function as a
wrapper over the same state machine.

Bind loopback redirects to `127.0.0.1`. Allow concurrent browser connections.
Return 404 for unrelated paths and continue to wait. Keep issuer, client ID,
scopes, authorization query, token exchange, and credential storage in the
provider-specific caller.

Configured MCP clients survive logout. Remove dynamic clients on logout
because they are tied to one redirect URI. Release discovery state on every
terminal path. Explicit login or logout cancels active browser, discovery, and
refresh work.

A JSON-RPC standard-I/O connection owns its reader, writer, and buffers. Match
responses by request ID. Remove a completed request from both connection lists
before you release its storage.

## Diagnostics

Collect diagnostics in `cfg->diag`. Drain them only at turn, verb, and slash
command boundaries. Do not print diagnostics during streaming output.

Define `FYAI_MODULE` once in each source file. Let the module add the message
prefix. Do not type the prefix into each message.

The first error is the cause. Demote later errors to debug until the sink is
drained or reset. Use `fyai_diag_reset()` after a caller recovers from an
error. Put all detail for one failure in one diagnostic. Use a lower severity
for nonfatal reports.

The diagnostic sink owns its own builder and its own output descriptor. Do not
use the durable builder or `transient_gb`, and do not route it through the
rendering sink: diagnostics must report before that sink is created and after
it is destroyed. It is the one output owner beside the sink.

Parse provider JSON with `FYOPPF_COLLECT_DIAG`. The parser then attaches its
report to the failed value instead of writing it to standard error, where it
names the anonymous input buffer by address, sits outside the sink, and cannot
be attributed to the request that produced it. Report it with
`parse_diag_text()` from a caller that has a context, in the one diagnostic
that says what was being read.

Expand formatted text before you intern it. Diagnostic raises are lock-free,
but a drain resets storage. Drain only when all raisers are quiescent. A null
sink prints immediately.

Keep normal output out of the diagnostic sink. The banner, spinner, shell echo,
approval prompt, stats, usage, and successful command results are presented
content: send them to the rendering sink on the status or notice stream.

## Build and test

Configure and build:

```sh
cmake -S . -B build
cmake --build build
./build/fyai -m gpt-4o-mini "hello"
./build/fyai dump state
```

Run tests:

```sh
ctest --test-dir build --output-on-failure
./build/fyai_test --list
./build/fyai_test oauth/plain_redirect
ctest --test-dir build -R fyai/unit/oauth
```

The unit-test binary links production sources except `src/main.c`. Do not use
test stubs. Declare tests with `FYAI_TEST_ENTRY(suite, name, entry)` after the
includes in a `tests/fyai_*_test.c` file.

Functional cases run the real binary with the scenario-driven mock provider.
Keep them hermetic: localhost only, with private `HOME` and `XDG_*` paths.

### Sanitizers

Use ASAN for parser, storage, tool, event, and YAML changes:

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

The complete functional suite must also pass under ASAN.

### Static builds

- `FYAI_MOSTLY_STATIC`: statically link dependencies, but keep glibc, libm,
  and the loader dynamic. This mode includes the glibc C23 compatibility
  wrappers.
- `FYAI_MUSL_STATIC`: build a fully static musl binary with the Docker static
  builder.
- `FYAI_STATIC`: use the legacy fully static host-libc mode only for controlled
  same-host tests.

Static dependency modes require `libfyaml.a`. Glibc modes also require static
OpenSSL and zlib libraries. A mostly-static binary must list only libc, libm,
and the loader in `ldd`. A musl-static binary must report that it is not a
dynamic executable.

### Terminal rendering tests

Use libvterm when correctness depends on terminal cells, wrapping, cursor
position, background fill, or SGR state. A PTY byte capture cannot prove these
properties.

Use two `VTerm` screens. Feed direct libfymd4c output to one screen. Feed the
same content through the libfytimui path to the other. Compare the complete
stable row and adjacent blank rows. Inspect cells after the final glyph.
Normalize colors with `vterm_screen_convert_color_to_rgb()` and compare them
with `vterm_color_is_equal()`. Do not compare `VTermColor` with `memcmp()`.

Make the new test fail before the fix. Register it as an individual CTest test.
After the fix, run both the libfytimui and fyai suites. If renderer bytes are
correct but cells differ, trace transport writes and later compositor cleanup.

## C style

Use Linux kernel C style:

- Use hard tabs with 8-column stops.
- Use kernel braces and spacing.
- Declare local variables at the start of the function.
- Do not declare variables inside branches or loops.
- Use `lower_snake_case` for C names.
- Use uppercase names for CMake options.
- Use four spaces in CMake files.
- Compile as GNU C2x with `-Wall -Wextra` and
  `-Wdeclaration-after-statement`.
- Add SPDX headers to new source files.
- Avoid whitespace-only alignment changes.
- State ownership and non-obvious arena, mmap, atomic, durability, and
  filesystem assumptions near the code.

Do not put an operation inside an error-check predicate. Run the operation,
store its result, and then test the result:

```c
	rc = epoll_ctl(el->backend_fd, op, fd, &ee);
	fyai_event_error_check(el, !rc, err_out,
			       "epoll_ctl: %s", strerror(errno));
```

Do not write `fyai_event_error_check(el, !epoll_ctl(...), ...)`. Apply this
rule to all error-check macros. If a subsystem reports through another handle,
define one wrapper macro instead of repeating `->ctx`.

Use ASD-STE100 Simplified Technical English for changed documentation,
retained comments, and commit messages.

## Commits and patch series

Use an imperative commit subject with a subsystem prefix, for example:

```text
cli: add interactive prompt mode
```

Use two or three short body lines. State what changed and why. Wrap at 80
columns. End with exactly this trailer:

```text
Signed-off-by: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
```

Do not add another attribution trailer.

Make each patch one logical change. Build each intermediate patch. Keep these
changes in separate patches and in this order:

1. implementation;
2. tests; and
3. documentation.

Fold a fix into the patch that introduced the defect. Do not add a later fixup
patch. Remove temporary notes and revision history from the final series. Run
the applicable tests after each test patch. Run normal and ASAN suites on the
final patch. Run `git diff --check` on each patch.

## Patch review workflow

Review `master..devel` as a mail series. Before the first review, tag the tip as
`start-of-review` and generate the series:

```sh
git format-patch -o x master..devel
```

The reviewer writes notes that start with `panto>>` into numbered patch files.
Before you rewrite commits, find every annotated file:

```sh
rg -l '^panto>>' x/[0-9][0-9][0-9][0-9]-*.patch
```

Move every annotated numbered file to `_ANNOTATED-NNNN-...patch`. Preserve all
of them before you regenerate any numbered file. Never edit, replace, or
remove an `_ANNOTATED` file.

Fold each requested change into the commit represented by its patch. Carry
interface and semantic changes through all later commits. Keep every
intermediate commit buildable. Resolve commits again from the current series
order after each history rewrite.

When a patch is accepted, rename its preserved file to
`_REVIEWED-NNNN-...patch`. Never edit or replace a `_REVIEWED` file. Remove
only numbered patch files before you regenerate the series. Confirm that new
numbered files contain no `panto>>` notes and that preserved files still
contain their notes.

Keep branch `reviewed` based on `master`. Apply accepted rewritten commits to
it in order. Confirm that its tree matches `devel` at the last accepted patch.
Then switch back to `devel`. Do not apply annotated mail files.

Before regeneration, run `git diff --check master..devel`, build the normal
tree, and run relevant tests. Run the complete ASAN suite after semantic or
structural changes. Remove temporary checkpoint commits and safety tags when
the rewrite is complete. Leave `devel` checked out. Do not change untracked
review data except for the requested files under `x/`.
