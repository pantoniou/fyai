# Changelog

## 0.5.0 - 2026-11-24

### Added

- **Interactive sessions run in a split worker.** The terminal is a client and
  the model, tools and arena are in a local worker. The worker keeps durable
  turns, restarts with the view, and ends when no client keeps it. The client
  has a resume picker, a branch browser, completion from the worker, skills,
  sub-agent tiles, image attachments, and its own prompt history.
- **A fleet of sessions.** `fyai fleet` scans nested projects, lists and
  creates members, and `fleet kill` stops them. A root session supervises
  its members, restarts crashed ones, and queues starts over `fleet/max_live`.
  The `fleet` model tool and `/jump`, `/root`, `/split` and `session adopt`
  work with `PROJECT:BRANCH` names. The fleet panel shows the members and the work.
- **`/title`, `/work-height`, `/tool-display`**, the `title/auto` key, and the
  keys `display/tool_command_lines` and `display/tile_live_rows`.
- **`branch/on_conflict=fork`** keeps the work of a concurrent writer on a new
  branch.
- **`sandbox enter` and `sandbox profiles`**, native macOS confinement and
  project capture, and a seccomp filter for UDP below Landlock ABI 10.
- **Static and Fil-C builds**: scripts, CMake targets that run the cases against
  them, a static build under ASAN, and a Fil-C build in the release.

### Changed

- **The alternate screen is the default page**, and a page can take the whole
  normal screen. Frozen rows of a streaming answer go to the scrollback.
- **Each turn records the project, and tool calls show diffs by default.**
- **The platform code of the host, sandbox and view is in backends.**
- **The catalogue has new models**, among them `gemini-4-neon`.

### Fixed

- A call that ended stays on the screen until its rows commit, and live rows
  render again at a new width.
- Command-line overrides stay when a session changes branch.
- Refresh storage of credentials no longer waits for an active turn.

## 0.4.2 - 2026-10-26

### Added

- **A web page for fyai.** `web/` is a fork of the llama.cpp web UI. A small
  bridge (`web/server/bridge.mjs`) runs `fyai claude-sdk` for each turn and
  streams the frames to the page. The page installs as a PWA, and the Electron
  app opens it in a window. See `doc/fyai-web.md`.
  - A conversation is a branch of the project, named as the CLI names it, so a
    session of the CLI is a conversation of the page. An edit or a regenerate cuts
    the branch back before the new turn.
  - The picker lists the models of the catalogue that have a key, with the input
    types and abilities of each. Images go to fyai as attachments, and stored images
    come back with the conversation.
  - The page shows the tools, thinking, usage and task lists of a turn, and
    renders the output of a slash command as Markdown.
  - The bridge requires a token and a known origin. The page settings live in the
    bridge, and a provider key goes to the secret store of fyai, never to the page.
  - Each window of the desktop app is one project, on a fixed port. The sidebar
    names the project, and the app opens another one or a recent one from it.
    `/api/fyai/info` reports the project.
- **`claude-sdk` serves the branch that `-b` names**, and attaches the image blocks
  of a user message.
- **A command in the background ends as a monitor.** The end of the command is
  reported to the model. See `doc/events.md`.

### Changed

- **The default build is Release.**
- **A table is written as Markdown source when `display/markdown` is off.**
- **A plain wait ends on an interrupt or an event**, and the nested loops of a call
  stop on an interrupt. The synchronous MCP call wrappers are gone.
- **The SIGUSR2 dump goes to `~/.fyai/dump.log`.**

## 0.4.1 - 2026-10-24

### Added

- **`list_root`** is a profile key that lets a tool open and list `/`. The `dev`
  profile has it: the package scripts of a build change to `/` and open `.`.
- **The top row of the prompt names the sandbox profile.** The new `{sandbox}` value of
  `display/prompt_top`, which the default row ends with, is ` · sandbox PROFILE`, or
  ` · no sandbox` when nothing confines the tools.
- **`--cast FILE`** records the screen of an interactive session, and replaces
  the environment variable `FYAI_CAST`. The recording leaves out a repeated
  cursor update of an idle page. Its header says what the program knew at the
  start, and its markers say what the program did: requests, the end of a turn,
  tool calls, scopes, and each warning and error. `fyai cast play` opens a
  player with play and pause, steps, marker jumps, a list of markers, a timeline
  and the mouse; `markers` and `info` list the markers and show the header.
  `--cast-compat` writes plain asciicast.
- **A scope for the processes of an exchange.** A program that a tool detaches,
  such as a build server, stays alive for the later calls of the same exchange
  and ends with it. A terminal session, a monitor or a background sub-agent keeps
  the scope while it lives. A host with no namespace tracks the processes and
  stops them instead. The result of a call says when it left a process.
  `sandbox_scope` chooses `exchange`, `session` or `call`, and `list daemons`
  names what a scope holds. See `doc/exchange-scope.md`.
- **`sandbox_grace_ms`** sets the time that the processes of a scope have to stop
  after SIGTERM.
- **A home of its own for the tools, and mounts in a profile.** A sandbox profile can
  set `home: {private: true}`. A new tree is mounted over the real home in the
  namespace of the tool, so `HOME` and the paths under it are unchanged, and the
  tool sees only the tree. `copy` names files and directories of the real home, such
  as `~/.bashrc` or `~/.config/nvim`, and `env` names variables that hold a path,
  such as `CARGO_HOME`. The tools get a private version of each at the same place. A
  directory is an overlay of the original, so nothing is copied and the original
  never changes; `method: copy` copies it and shares blocks where the file system
  can, with the code of the content store. The paths that the sandbox grants under
  the home, the project among them, stay visible. A path under a `deny` entry is not
  given. `mounts` makes other binds and overlays. A host that cannot make a namespace
  stops the tool: there is no fallback.
- **`view enter --profile NAME`** confines the command by any sandbox profile, as a
  tool of a session under `/session profile NAME` is confined. `--lockdown` is the same
  as `--profile lockdown`. A profile with mounts is not supported in a view yet, and
  stops the run.
- **`expose: [userns]`** in a sandbox profile lets a program make a user namespace of
  its own, as BitBake does for each task. The tool may write `/proc`, with `/proc/sys`
  and the other paths that change the kernel mounted read-only, in a PID namespace.
  The new `build` profile is `dev` with it.
- **The `yocto` profile** is `build` with a private home and a copy of `~/.gitconfig`.
  `~/.netrc` is not in that home, so the readers of BitBake and Poky get "not found"
  and not "permission denied", and a build needs no `HOME` override.
- **`background` for `exec_command`** starts the command detached: a session of its own,
  `/dev/null` for standard input, output and error, and the call returns at once. The
  command lives for the exchange, like any process that a tool leaves, and `list daemons`
  names it.
- **A daemon can be named and stopped.** `name` on a `background` call labels the daemon
  in `list daemons`. `cancel` and `/kill` stop it by its label or its pid, with SIGTERM
  and SIGKILL after `sandbox_grace_ms`.
- **`sandbox_on_failure`** says what happens when the sandbox cannot be set up on the
  host, as when a profile makes mounts and user namespaces are not allowed. `warn`, the
  default, reports the cause one time and runs the tool without the sandbox. `fail`
  stops the tool and says why. A host with no AppArmor profile for fyai thus still works.

### Fixed

- **A failed turn keeps its work.** An error in the tool loop no longer drops
  the whole turn: the steps up to the last complete tool round are stored. A
  tool call that returns no result gets a tool error, and the turn continues.
  The log of a fullscreen session shows the rows that report errors first.
- **A monitor ran for the limit that it was given.** A monitor can ask for 30
  minutes, but its command ran under the 10 minute ceiling of
  `shell/max_timeout_ms` and ended early, which the model could not tell from the
  limit it had asked for. The ceiling no longer applies to a monitor, and the end
  of a monitor that reached its limit names the limit.
- **A program that draws on a pipe.** A command with no terminal can still send
  control sequences, as a build tool does for its progress. The result is now
  what the screen of those sequences shows, and not the bytes. Output that does
  not draw is unchanged.
- **A process of a scope was killed when the run ended.** The run ended at once
  and the kernel ended the namespace with SIGKILL, so a build server could not
  save its state and left a stale lock. The run now waits for the processes to
  stop, up to `sandbox_grace_ms`, and waits for no one when none runs.
- **A view removes the work directory that OverlayFS leaves with no mode.** Some
  kernels leave a work directory with mode 0, which not even its owner can open,
  and `gc` then could not remove the runtime of a view. A view that cannot change
  the mode of a directory now stops and gives the cause.
- **Build warnings.** The player of `fyai cast` keeps its time in buffers that
  fit the longest value, and the PTY driver uses `snprintf`, which macOS does
  not deprecate.

## 0.4 - 2026-10-22

### Added

- **Attachments and image input.** Attach images and text files to a message
  with `@PATH` (completed on Tab), `/attach`, `/image add`, `/image paste`
  (clipboard helper), or `--image`. Each provider grammar carries images; an
  attachment shows as a bold reference row on the card. A pinned root refuses
  attachments. Exports carry image bytes as `!!binary` and imports restore
  them. See `doc/media.md`.
- **Claude Agent SDK compatibility.** `fyai` serves the SDK protocol with
  streaming, forks, structured output, hooks, permissions, and per-invocation
  MCP servers. A persona can limit the tools of its agent, and a sub-agent
  relays its tool calls and steps to the parent.
- **ACP.** Editor protocol support for v1 and v2.
- **MCP.** Deferred discovery and resource access.
- **Web page and desktop app.** A chat page (imported from the llama.cpp web
  UI) served by a local bridge that streams `fyai` turns, plus a desktop
  window for Linux and macOS with the native static `fyai` bundled. CMake
  targets build and install the page and the app; CI builds the releases. The
  page takes its slash commands from the `schema` verb and lists only models
  that have a credential.
- **`settings` tool.** The model can read, select, and export the
  configuration and the catalogue, and propose changes for review. The user
  keeps or reverts them. Each configuration key carries an `x-fyai-model`
  access level (`hidden`, `read`, `review`, `write`).
- **`list` command** now covers live shells, sub-agents, waits, monitors, and
  questions.
- **Terminal UI.** A log of results with levels in a small panel that expands
  to a popup; a maximize button that gives a tile the whole page; a prompt
  mark and waiting lines while input is queued; `display/idle_poll_ms`.
- **Turn control.** A turn reports why it stopped and can stop at a cost; a
  turn can answer with structured output; `branch` can start a fresh session
  from a branch.
- **Sandbox cover.** A sandboxed tool hides the project arena in a mount namespace,
  so it can create files in the project root. `sandbox_cover` chooses how: a user
  namespace of the tool, a bubblewrap holder that the tool joins, or none. On Ubuntu
  24.04 and later, which restrict user namespaces, fyai says at the start what to do:
  `fyai sandbox apparmor | sudo tee /etc/apparmor.d/fyai` (a new command that writes
  an AppArmor profile for the binary that runs), or install bubblewrap with its AppArmor profile.
- A session that runs in a view applies the view to the project when it ends from
  an idle prompt and no path conflicts (`view/apply_on_exit`). A new session
  refuses to start while the view holds a change that the project does not have.
- `FYAI_CAST` records a terminal session; the mock provider is served by a
  hidden `fyai` verb.
- `patch` accepts a unified diff inside the Begin Patch envelope.
- `root` prints its handle as text.
- `reasoning` effort and summary accept `none`, which sends no field.

### Changed

- **Sandbox profiles.** `sandbox_profile` names a profile, and the profiles are
  data: the `sandbox_profiles` key of `config.yaml.sample` holds `dev` and
  `lockdown`, so a new arena stores them and `config show` lists them, and the
  key takes more (`extends`, `deny`, `allow`, `network`, `view`). Paths may use `{{hostname}}`, `{{os}}`,
  `{{arch}}` and `{{host_specific}}`. `dev` is the new default: the project,
  the toolchains and package caches of the user, and TCP to ports 80, 443 and
  9418 (git and registries). `dev-view` is `dev` with the session in a view. `/session profile [NAME]` lists or selects a
  profile and restarts the session; `/session dev` selects `dev`. A profile may
  carry `view: true`: the session then runs in a view of the project, and a
  switch to a profile without one applies the view to the project first and
  refuses on a conflict. PATH keeps
  only the directories that the sandbox grants reach.
- The warning that TCP or UDP egress remains open on an old kernel appears
  once for each process, not at each tool call.

- Configuration and catalogue paths escape `/` and `~` as `~1` and `~0`.
- Model changes persist and show effective YAML diffs; sourced catalogue
  changes are reviewed before commit.
- `api_url` is derived from the grammar when it names another grammar, and
  the grammar stays with the endpoint that speaks it.
- A verb that sends no model request no longer resolves the model; it starts
  faster and needs no credential. Large embedded documents load from a
  prebuilt image, and the embedded data is checked once at build time.
- A signed-out login counts as no login, and an interactive resume works
  without API credentials.
- The shell tool accepts `cmd` as the command and rejects a call without one.
- The shell tool runs bash where the system has it, and else `/bin/sh`, so the
  model meets one dialect on every host. `shell/shell` still chooses another.
- The mostly-static build is shared between hosts, Docker, and CI, and the
  glibc baseline is selected by architecture.

### Fixed

- `--version` printed a stale value because an ignored `.tarball-version` file
  overrode the git version, and a snapshot version string failed CMake's
  `project(VERSION)` check. CMake now keeps the full string for display.
- A closed shell no longer leaves a wait pending; a failed JSON-RPC write
  reports no output.
- A configuration value that a layer repeats is kept.
- A tool title row cannot be read past its end; a tile title that is cut keeps
  its text; a patch that does not parse gets a title row.
- Page slot ids no longer overflow the slot buffer, and skill path building
  checks lengths.
- Libcurl initialisation moves to the first handle.
- Build fails on a returned address of stack storage; `true` and `false` are
  `_Bool` in every translation unit.

### Documentation

New or updated: attachments, the SDK, ACP, MCP discovery, the settings tool,
declarative state, the web page and desktop app, build targets, the PTY
driver, `FYAI_CAST`, the mock provider, and the `web_fetch` design.
