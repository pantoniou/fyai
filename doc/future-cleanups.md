# Future cleanups

Status: one plan is open, the split of the TUI from the core, as two
processes. Every other cleanup that this document listed is done or was
checked and left as it is.

Nothing keeps compatibility with old arenas or old interfaces.

## Plan: separate the TUI from the core

Status: plan. The measurement is done and one defect it found is fixed. No
step of the split is done.

Related designs: `doc/remote-sessions-sdd.md` (the protocol),
`doc/fyai-background-sessions-sdd.md` (retained sessions, controller and
watchers, protocol 2) and `doc/fyai-mobile-tailscale-sdd.md` (a web client with
no arena and no terminal). The split is a client of the same worker as those
two, so it takes their rules and adds none of its own to the protocol.

An earlier version of this plan put the TUI in a second thread of the
invocation. That needs two contexts, a loop context, a diagnostic context, a
shared-memory channel and a safe `fork` with two threads. The plan now uses two
processes, and the code for it exists.

### Goal

An interactive invocation starts two processes from one binary:

- The worker runs the engine with no display: the context, the event loop, the
  arena, the tool jobs, the curl transfers and the commit. It is what `fyai
  serve` starts for a client (`claude-sdk --remote-worker --session-type
  fyai`).
- The client owns the terminal: the renderer, the prompt, focus, zoom, scroll,
  the theme and the layout. It is the full-screen client of `--remote`
  (`src/remote/fyai_remote_ui.c`), over a mirror of the UI model.

A socketpair joins them, in image mode from the first frame. The invocation
stays stateless: both processes end with it, the worker commits the canonical
state as it does now, and the client keeps none. No resident process is added.
`fyai serve` is the same worker over another transport.

### Measurement

Same PTY script against a local session and against `--remote` with the mock
provider, 2026-11-16. Wall time of the whole run.

- Image mode is on for the Unix socket: after the hello every frame is an
  image, and nothing is parsed or written as text. A `strace` of the server and
  worker shows it.
- The first run of the remote client was up to 70% slower for 200 KB answers.
  The cause was not the transport. A frame that a producer requested did not
  shorten the armed UI timer, so the paint of the answer waited up to 100 ms
  for the activity timer. The fix is in `eb2d20a5`, and local sessions gain
  from it too.
- After the fix the total CPU of client, server and worker equals that of a
  local session. A turn of a 1 KB answer takes the same time in both, and
  most of it is the 33 ms frame throttle. A turn of a 200 KB answer is 3 to
  10 ms slower remote (5, 20 and 40 turns: 279, 1230 and 2970 ms against 258,
  1070 and 2580 ms), and the gap grows with the turns. The cause is not found.
  The profile has no large item: the markdown render and the parse of the
  reply of the provider cost the same in both.
- Start, the first prompt and `/quit` take 15 ms more with a worker that is
  forked by a server. The local fork has no server to connect to.

Not measured: a fast program in a shell tile, whose bytes now cross the socket.

### What exists already

- The UI model (`doc/ui-model.md`) holds data and Markdown source, and holds
  no rows, no width and no theme. View state is already outside it.
- The worker is the engine with no display. It sends `fyai`/`store` and
  `fyai`/`text` frames. The client mirrors the model, and sends `fyai_answer`,
  `fyai_command`, `fyai_terminal_input`, `fyai_bang` and `fyai_sync`.
- `scope: view` commands and `display/` settings already run in the client.
- Frames are generics. Image mode (`doc/remote-sessions-sdd.md`, 5.2.1) sends
  a frame as a relocatable image. Flow control (section 5.3) says what to drop
  when a client is slow.
- `fyai_remote_spawn()` makes a socketpair and forks a program with the
  channel on its standard input and output. `remote_spawn()` of the server
  starts a worker the same way.
- The oracle cases (`tests/cases/remote_oracle_*.sh`) compare the screen of a
  remote session with that of a local one.

So the work is not a new protocol. A local session becomes a remote client of
a worker that it forks.

### What the split needs

1. The entry. An interactive run with a terminal makes the socketpair, forks
   the worker with the arguments of `remote_spawn()`, and runs
   `remote_view_run()`. Check how the credential transport admits a worker
   that has no server above it.
2. The parity of the client. What a local session does and `--remote` does
   not yet. The list comes from the PTY suite run in split mode. Known
   places to check: the first-run steps of the interactive loop in
   `fyai_prompt_interactive_async()` (the MCP gate, the catalogue update, the
   configuration editor), `$EDITOR` and the clipboard, the terminal probe and
   the size of the window, images and the browser tile, login and consent
   that open a browser, and `/btw` and sub-agent screens.
3. The worker has no terminal. It runs with `/dev/null` for standard input. A
   question of the worker is an `ask` frame already. Each remaining
   `fyai_ui_*` call in the core must be a frame or a no-op in the worker. A
   call that is neither is a defect of this step.
4. Signals and lifecycle. The worker starts its own session (`setsid()`), so a
   key of the terminal reaches the client only, and the client sends
   `interrupt`. The retention policy of the session decides what an end of the
   channel does (background sessions, section 6.2). The local fork is an
   ephemeral session: the worker finishes the turn as an interrupted one,
   commits and exits, and it is never retained, so no resident process
   appears. A session of `fyai serve` with `retention: server` keeps running.
   A death of the worker makes the client restore the terminal and print the
   cause. The client waits for the worker and returns its status.
5. Diagnostics. The worker drains at its boundaries and sends the drain as log
   entries. The client reports its own faults to its terminal.
6. A client with no arena. A client on another machine has no arena, so it
   cannot take the display configuration from one. Today the worker sends the
   `display` group in the `state` store (`remote_ui_state()`), which makes the
   view settings of a person the property of a branch on the worker. The
   client must own them (see Display configuration below), and send the
   facts about its terminal to the worker.

### The data of the client

The client has no arena, and a client on another machine cannot have one. Some
things it draws and some commands it runs read data that comes from the arena:
the branch table (`ctx->bstate->branches`), the turns of a branch for a
preview, the recent sessions of the resume picker, skills, the catalogue, and
the files of the project. The branch and resume browsers (`src/front/
fyai_browser.c`) call `fyai_branch_*()` on the arena. The completion engine
(`src/cmd/fyai_cmd_complete.c`) reads it, and it runs in the client process,
so it lists the files of the client, not those of the worker.

Rules:

- The worker is the only reader of the arena. The client reads data from
  frames and from nothing else. A client does not map the arena.
- Data is in a store if it is small and changes with the conversation, and it
  is a query if it is large, rare or paged. A branch table of a thousand rows
  is a query: a store would send it again at each turn, because each publish
  changes `updated` of the current branch.
- An action is not a query. Create, rename, delete, describe, join, reset and
  checkout go as `fyai_command` lines (`/branch rename A B`) that the command
  registry already defines. Their output is in the log store, as now. No
  second action protocol.

#### The query

One request subtype, `fyai_query`, in the envelope that the other
requests use. The arguments are in `params`: the front reads `args` of any
request as the command-line words of a session, and drops a client whose
`args` is not a list of them.

```json
{"type":"control_request","request_id":"q7","request":{"subtype":"fyai_query","what":"branches","params":{"all":false},"after":null,"limit":200}}
```

The answer is a `control_response` with `{rows, next, rev}`, or an error with a
code: `unsupported`, `bad_request`, `busy`, `too_large`, `stale_cursor`.

- `what` names the data. `rows` is a sequence of generics, a projection that
  holds only what a renderer reads, and not the branch entry with its `store`.
- `after` is an opaque cursor from `next`. `limit` is at most 1000 rows, and
  one answer is at most 1 MiB, so it stays under the frame limits of every
  transport. The worker walks at most a fixed budget of rows in one turn of
  its loop and returns what it has with a `next`, so a query never holds a
  turn that runs.
- `rev` says the generation of the data. It is the generic word of the arena
  value that the answer was made from: the mapping of the branch table for
  `branches`, the `skills` member of the store of the branch for `skills`, the
  entry of a branch for its row. The client compares it for equality and
  never reads through it. It crosses the wire as an integer. Any generic
  value serves: a non-empty mapping or sequence is an address, and a short
  value or an empty collection is its content in the word. The arena keeps
  an unchanged subtree at the same address, so a publish that moves only the
  head leaves the `rev` of `skills`, `catalog` and `config` as it was, and
  a row of `branches` that did not change keeps its own `rev`.
- The `rev` is taken from the value in the arena and never from the rows of
  the answer, which are built for the answer in a builder: the same content
  has another word at each build.
- Equal words are equal content. Different words may be equal content, after
  a collection moved objects or when dedup missed a copy, and the cost is one
  refresh. The one wrong case is a new value at the address that an old value
  had. A client that sees each change cannot meet it, so the `rev` is bound
  to the epoch of the worker: a new epoch, or a collection by another
  process, drops everything the client holds and it fetches again. Check
  how a running worker learns of a collection.
- A cursor from an older `rev` fails with `stale_cursor` and the client starts
  again. A request may carry `if_rev`. When it is the `rev` of the data now,
  the answer is `{unchanged: true}` and no rows. A browser that opens again
  and a page that is cached cost one small frame.
- The worker puts `rev: {branches, skills, catalog, config}` in the `state`
  store, which is small. A client that holds a cached answer drops it when its
  `rev` moved. It never polls. This push is the optional part: with `if_rev`
  a client can also ask when it needs the data.
- A query is read-only, so a watcher may send it. A device scope for
  observation is enough (mobile design 8.2). A query that names a path or a
  file follows the read rules of the sandbox of the worker, and a name that
  leaves the project is `unsupported`.
- The client keeps at most 8 queries in flight. A later one waits. The reply of
  a request that nobody waits for (the popup moved on) is dropped by its
  sequence number.
- The names a worker serves are in the `initialize` answer (`queries`), so a
  client of another version, or the web client, asks only for what exists.
  The mobile design (7.2) asks for the same structured list of sessions, so
  `sessions` and `branches` serve it too.

#### Who asks what

| Consumer | `what` | Note |
| -------- | ------ | ---- |
| `/branches` browser | `branches` | `{dir, all, filter}`. Rows: name, updated, op, from, description, directory, turn count. |
| Preview of a branch | `branch_turns` | The last exchanges of a branch, as `fyai_sync` gives them for the attached branch, with a `branch` argument. |
| `/resume` picker | `sessions` | `{all}`; the recent sessions of this directory or of all. |
| Completion of a kind that reads data | `complete` | `{kind, given, partial}`; candidates `{value, description}`. See below. |
| Skills, catalogue, profiles, MCP servers | `skills`, `catalog`, `profiles`, `mcp` | Rows for a list command or a picker. |
| History selection | `fyai_sync` | Exists now. |

#### Completion

The engine knows the command line from the registry, which is in the binary
and so in the client. Commands, options and enum values need no data, so the
client completes them itself and with no wait. Only a kind that reads data goes
to the worker: `branch`, `ref`, `agent-branch`, `session`, `view`, `skill`,
`skill-file`, `model` and `provider` (the catalogue), `catalog-*`,
`mcp-server`, `sandbox-profile`, `history-select`, `file`, `dir`. Each entry of
`complete_kinds[]` gets a field that says where it runs (`local` or `worker`),
and no code tests a kind name. The popup shows the last candidates while the
answer comes, and drops an answer that is not for the word that is typed now.
`file` and `dir` are now right for a remote client: the files are those of the
project of the worker.

#### The browsers

The browser stops calling `fyai_branch_*()`. It reads rows from a source with
three calls, list, preview and the generation, and it has a loading state
for a page that is on its way. No callback waits in a nested loop. There is one
implementation of the data: `fyai_query()` in the worker, a function of the
context that returns a generic. The frame is its transport. An in-process
session, until the default step removes it, calls the same function directly.

#### Considered and left

- A client with a read-only map of the arena. A remote client has none, and the
  rules for `fyai_arena_reopen()` and for relocation then reach a second
  process.
- The browser in the worker as the screen of a program, shown in a tile like
  a shell. It needs no protocol, but the theme, the layout and the width are
  then the worker's, and a web client could not draw it.
- A store for each of these lists. The size and the churn above.
- One frame subtype for each list. One `fyai_query` with a `what` has one
  limit, one error set and one place for the scope check.

#### Work

1. Done. `fyai_query_answer()` and `fyai_query()` in `src/store/fyai_query.c`
   serve `branches`, with unit tests of the projection, the cursor, the limits
   and the `rev`. The optional `rev` member of the `state` store is not made.
2. Done. The `fyai_query` handler of the worker, `queries` in the answer of
   `initialize`, the queue of the client (at most 8 in flight, a cancelled
   answer dropped), `fyai_uimodel_query()` for a renderer, and the command
   `query` as the first user: `fyai query branches` in process, and `/query`
   in a session, which runs in the client and asks the worker. The cases
   `query_cli`, `split_query` and `remote_query` run it in process, in a split
   session, and over the socket, HTTP, HTTPS and ssh. Two faults came with it.
   The front read `args` of a request as the words of a session and dropped a
   client whose `args` was a mapping, so the arguments are `params`. The
   client numbered its requests twice and cut an id of two digits
   (`fy_sprintfa()` reads its arguments twice), which no earlier request had
   met because none waited for its answer.
3. Done. Each kind of `complete_kinds[]` says where it runs (`worker` or
   local). A mirror completes commands, options and the local kinds (`theme`,
   `config-path`, `config-assign`, `setting-value`, `page-layout`,
   `help-topic`, `history-select`) itself, and asks the worker for the rest
   with the query `complete {kind, partial, given, prev}`. The answer is kept
   by kind, given words and partial, and serves any longer partial by
   filtering; a new line forgets it. The popup of libfytimui asked the host
   for candidates at each edit and could not be told that more came, so the
   first ask of a word found none. The library now has
   `fytim_completion_refresh()`, which asks the host again for the line that
   is typed and serves a Tab or an automatic popup that found nothing for it,
   once. The client calls it when an answer comes late, and the pin of
   libfytimui in `CMakeLists.txt` is the commit that has it. A file or
   directory completion is the worker's
   by the same rule, and it follows the local rules (`~/`, absolute paths): the
   bearer token gives the full authority of the user, and a device scope for
   observation does not exist yet.
4. In progress. The browser of a client reads its tree by `branch_tree` (every
   branch, a stored agent too, the live agents, HEAD; paged; the rev moves with
   the root and with the generation of the agents) and shows "Loading
   branches" before it. The page builder reads the fields of the rows and no
   entry. An action of a client with no arena is a command that the worker
   runs (`/checkout`, `/branch new --stay`, `rename`, `delete -f`, `describe`,
   `/reset`), the tree is read again when it ends, and the focus returns to the
   list. The card of an action (`i`, `l`, `c`) is a render by the worker
   (`branch_preview`). The line that a mirror gets goes to the forms of this
   process first (the browser, a command that asks), and the loop of the
   client runs `fyai_browser_step()`. With this, 8 of the 10 `ui_branch_*`
   cases pass in split mode, and the other two have the causes below. What is left:
   - The preview beside the list and the recap (`p`, `t`) are done: a worker has
     no display records to recap, so the client asks `branch_turns` (the last
     turns, in the form of the sync) and renders them. `ui_branch_preview`
     still fails in split mode, only because a worker without a branch names
     it `sdk/<uuid>`, which is wider than the pane of the preview, so the label
     after the name is cut. The worker of a split session has to start as a
     local session does: a `session/` branch with the configuration of the
     branch last updated in its directory.
   - The resume picker and the foreign sessions (`ui_branch_preview_theme`,
     `ui_resume_*`): rows with tokens and the import data, the files of the
     other tools read by the worker (`foreign`), and a `--split` that takes a
     `resume`. The worker must also start on the session that was chosen.
   - Merge and rebase from the browser (CLI commands only), and `list_long`: the
     output of a `view` command of the worker goes to a pane of the client and
     not to its scrollback.
   - A line typed at once after the second Escape of a browser that was opened,
     changed and opened again is not taken, with or without a worker (checked on
     a build before this work). The process is not hung: it waits at its prompt,
     and a wait of three frames before the line takes it. The case leaves the
     reopen out.
   - Done: a worker ended with its client only when it was idle. A question
     that waited for an answer, and a wait that the model asked for, kept the
     worker for good (13 stood after the cases that time out, and none ended
     on SIGTERM). The connection now calls the worker when it closes, and the
     worker settles its questions, interrupts the turn, and asks the waits to
     look. `split_worker_asked` and `split_worker_waiting` kill the client
     while the worker waits; each fails without the fix.
5. The PTY cases of the branch and resume family (`ui_branch_*`,
   `ui_resume_*`) pass in split mode.

### Steps

Each step builds, passes the suites and lands as its own patch series, in
this order: implementation, tests, documentation.

0. Done. The measurement, and the timer fix (`eb2d20a5`). Add the benchmark
   to `tests/bench/` so that the numbers can be repeated.
1. Done. Spawn. The hidden global option `--split` forks the worker on a
   socketpair and runs the client of `--remote` against it, with no server
   (`remote_split_open()`). The worker has its own session, and its standard
   error is kept and reported if it fails. `tests/cases/split_session.sh` runs
   two sessions on a PTY. A session of 10 turns with a 200 KB answer takes 515
   to 566 ms, against 460 ms in process and 480 to 530 ms through a server.
   Start and `/quit` take 71 ms, against 55 ms in process. Gaps left for the
   steps that follow: the client waits for the worker without a limit when it
   ends, `resume` is refused, and a command line is refused.
2. Parity. Run the PTY suite with the option on, by an environment variable of
   the harness, and fix each divergence in the worker or in the client. The
   oracle cases must pass. Do not add a second path in the worker for a split
   session. `FYAI_TEST_SPLIT=1 ctest -R case_` puts `--split` in each `pty_run`
   of a command that is not a client of a server. Of 480 cases, 78 fail in
   split mode and 1 fails (`catalog_available`) without it. The first run had
   183: the client replayed `--set` against an arena that it has not, which
   `config: leave the --set ops to the worker of --remote` fixed. What stays,
   from a first look at the output of each family and not yet a diagnosis of
   each case:
   - The commands of the client that need data of the worker. `/branches`
     draws a table from the worker where a local session draws the branch
     browser, so the branch and resume cases (`ui_branch_*`, `ui_resume_*`,
     12) fail. The client has no arena, so each such command needs a request
     to the worker for its data. This is the largest family. The design is
     in The data of the client, above.
   - Sub-agent screens and tool bands (`ui_agent*`, `ui_inline_tool_agent*`,
     `ui_agents_parallel`, 11), and shell tiles with their focus and keys
     (`ui_inline_tool_focus*`, `ui_shell_*`, `ui_tile_maximize`,
     `ui_keys_surface`, `shell_input_wanted` and others, 13).
   - Work during a turn: `/btw`, events, pending panels, waits, cancel and
     Escape (`ui_btw_*`, `ui_event_mid_turn`, `ui_wait_*`, `ui_pending_*`,
     `ui_cancel_background`, `slash_midturn`, `wait_plain_escape`,
     `ui_idle_escape`, 11).
   - Sessions and state of the project: a session in a sandbox view (`session_*`
     says that the session exists already), the approval of a setting,
     reload, skills and the catalogue update (9).
   - `ui_page_*` (7), the recordings of `--cast` (2), and the rest.
   - The session branch of a worker without a branch is named `sdk/...`, and a
     local session names it `session/...`. The remote worker has to take the
     name of the local rule.
   - `resume` and a command line are refused, which three cases show.
3. Lifecycle. The rules of signals, shutdown and death of either process, each
   with a test that kills one side. Test the terminal restore.
4. Measure again, with a shell tile and with a long session (the growth of
   the gap per turn). Find the cause of the 3 to 10 ms of a large answer. If
   a number needs it, send the head of the arena with an arena fd in place of
   a store frame, and drop a hop. Not before.
5. Display configuration. The view keys move to the file of the client, the
   output keys move to `output/` in the branch, and the worker stops sending
   `display`. No compatibility is kept: an old `display` key in an arena is an
   error. The capabilities (size of the window, colour depth, image
   input) and the interest of the client (see Interest below) go in the
   `initialize` request of the client, which carries only the model version
   today, and in a frame at each change. This lands with the protocol 2
   change of the background sessions design (section 7), not as a second
   bump of the protocol constant. The web client of the mobile design has no
   arena and no terminal either, so the same request serves it. Check each of
   the 68 keys against the four kinds below. A client with no arena must work:
   add a case that connects with an empty `HOME`.
6. Default. The split is the way of an interactive run. Remove the option, the
   in-process presentation of an interactive run and the `fyai_ui_*` calls of
   the core that nothing uses. This removes the blockers that the thread plan
   had to solve first. Update the layout and the event-loop rules of
   `CLAUDE.md` and `doc/remote-sessions-sdd.md`.

A run with no terminal (a verb, `--print`, a sub-agent child) has no client and
keeps the plain sink. This plan does not change it.

### Risks and open questions

| Risk | Note |
| ---- | ---- |
| Latency of a key to a shell tile | One more hop. Measure it in step 4 before a change of this rule. |
| Completion and pickers need data of the core | Branches, the catalogue, skills. The remote client does it already; check each source. |
| Terminal features that only a local session has | The list of step 2. A feature that cannot be a frame is a reason to keep it in the client. |
| Credentials | The worker holds them, and the client needs none. The transport of a worker with no server needs a check. |
| A client that is slow | The flow control of section 5.3 applies. A store frame replaces the root. |
| A stream that is off but a consumer needs it | A tool band that waits for an answer or a question must still reach the client: a question is an `ask` frame, never a stream. List the producers and check that none of them mixes presentation with a request. |
| Order against the background sessions work | Both change the frames of `initialize` and the attachment. Steps 1 to 4 run on protocol 1 and the existing attach. Step 5 and the protocol 2 change land together, so a client is not changed twice. |
| Memory | The client mirrors the model, so the live document is twice in memory, in two processes. Check with a long session. |
| The start time | One fork more. Measure it with the local fork. |
| Slash commands | A command runs in the worker, with `scope: view` as the exception. Its output reaches the client as a store frame. |

The thread plan is not needed. Its ideas stay if a measurement ever asks for
them: one loop context for each loop, and one diagnostic context for each
reporter.

### Display configuration

Status: proposal, step 5 of the plan. The first steps do not need it: the
client takes the display configuration that the worker sends. A client with no
arena does, because the worker would otherwise own the view settings of a
terminal that it cannot see. Check the key lists against the code and the
schema.

Today the `display` group (68 keys in `data/config.schema.yaml`) is in the
merged configuration document, which is the branch configuration plus
`--config` plus `--set`. The group is `x-fyai-scope: session`, so a slash
command does not store it, but `config set` and the stored layer can. The
model can write it (`x-fyai-model: write` on the group).

Decide the seam by consumer, not by key prefix. A key has one of three kinds,
and a fourth thing, the interest of a client, is not a key:

1. View. Only the renderer reads it: theme, layout, work pane, keys, page,
   completion, focus, glyphs, scroll, popup. It describes a terminal and a
   person, not a conversation. It must not travel with a branch through a
   merge, a rebase, a checkout on another machine or a remote session. This
   is the large majority.
2. Output. The core reads it and the result is stored or sent to a program,
   so it is intent of the conversation. Examples to check: the preview and
   history limits of a tool exchange (the stored exchange keeps the preview),
   `command_output` (it decides if a command turn is recorded), and whether
   the output is Markdown. These keys leave `display/` and go to a group of
   their own, such as `output/`, which stays in the branch.
3. Capability. What the TUI knows and the core needs: the size of the window
   for the size of a pseudo-terminal, image input, the colour depth. They go
   from the TUI to the core as a frame, at start and at each change. They are
   never configuration. Only the controller of a session gives them: a watcher
   cannot resize or send keys (background sessions R8, mobile design 9.5), so
   its values are ignored.

Interest. What a client wants the worker to make and send of the live
presentation. A session that does not show the progress of a shell, the
output of a tool call or the screen of a sub-agent must not make that data in
the worker, and must not pay to send it. It is not a view key: the worker acts
on it. It is not an output key: it never changes what is stored or what the
model reads.

- The client sends it as a set of named streams in the `initialize` request,
  and changes it with a request frame. It is the `fyai_subscribe` of
  `doc/remote-sessions-sdd.md` section 7.4, with a stream for each live
  producer: the text deltas of a model, the progress of a tool, the output
  of a shell, the terminal bytes of a tile, the screens of sub-agents.
- A stream that is off is not made. The worker does not format its rows, does
  not copy its bytes and does not queue its frames. A stream that is off
  still ends in the store: the result of the tool call is in the
  transcript store, with the preview that the output keys give.
- A stream that is turned on gets a snapshot first and then the deltas, as in
  section 7.4, so a late client sees the present state and no gap.
- The terminal of a shell is the case to design with care: with its stream off
  the program still runs and the core still captures its output for the
  model. Only the copy for a tile stops, and a user that opens the tile later
  gets a fresh screen.
- The set is per attachment, and the worker acts on the union. A session
  has one controller and any watchers. A stream is made if any attachment
  wants it, and each attachment gets only the streams it named. A phone that
  watches a session for the transcript turns the terminal bytes off (the
  mobile design leaves rich terminals to a later gate), and the controller on
  the desktop keeps them on. When the last attachment that wants a stream
  leaves, the worker stops making it. An attachment that turns a stream on
  later gets a snapshot first.
- A slow attachment is evicted from the stream, never from the session, and
  never slows another one (background sessions R9). The interest of an
  attachment does not outlive it. The defaults of the client are all streams
  on, and a file of the client (the view file) can set the default for a
  person.

Place the view keys in a regular user configuration file, with a session
layer in the TUI context, and not in an arena:

- An arena is the history of a conversation: content-addressed, immutable,
  with a reflog. A theme change would be an entry in it, and `config undo`
  and `config reset` would mix the two.
- A file is what a person edits and copies between machines. It is the
  existing user configuration file, or a file next to it, checked against the
  same schema.
- The TUI reads it. The core in an interactive run never does. A headless run
  (a verb, `--print`) still renders with the sink, so it reads the same file
  for its colour and Markdown settings. The renderer library takes one small
  structure, `fyai_render_cfg` (planned: it does not exist yet), from whoever
  renders.
- The layers are the file, then `--set display/...` of the command line, then
  the session. A project file (`.fyai/ui.yaml`) can come later. It is not
  needed now.

This reverses one rule of `CLAUDE.md`. Today the user file only initializes an
arena that has no configuration. For view keys the file is the source and the
arena has no copy.

Effects:

- The `settings` tool and the model write access. A change of a view key
  becomes a request frame to the controller of the session, which applies it.
  A watcher gets none, and a device scope that lacks control cannot make
  one (mobile design 8.2). The keys keep a
  `review` access, because a key that hides the work of the model from the user
  must stay visible to the user. A headless run has no such keys to change.
- Remote sessions. The client reads its own file, so the `display` frame that
  the worker sends for a `--remote` command is not needed. A command with
  `scope: view` has a clear owner: the TUI.
- Old arenas hold `display` keys. There is no compatibility rule. The schema
  does not know them, so an old key is an error, and the user removes it. A
  store that holds one is not read until then.
- Schema. `data/config.schema.yaml` splits into the core schema and a UI
  schema. The command definitions bind a setting to a key path, so each
  setting command names the schema it belongs to, and completion reads it.
