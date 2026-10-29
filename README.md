# fyai

`fyai` is an AI coding assistant written in C. It has no daemon and keeps no
hidden process state.

Each invocation opens the durable state of the project, runs one command or one
complete model and tool loop, publishes the changes in one atomic step, and
exits. An interactive session keeps one process alive for the terminal UI. It
uses the same durable state.

![An interactive fyai session: a Python program written, run through the shell
tool, read back, and its algorithm drawn as a diagram in the
terminal](doc/img/demo.gif)

## Features

### Conversation state

- Content-addressed, immutable libfyaml arena in `.fyai/arena`. One atomic root
  is the only mutable value.
- Provider-independent conversation content. A branch can continue on another
  model, provider or API grammar.
- Branches that each own a conversation and a configuration. A branch start
  point supplies both.
- Symbolic references: `branch`, `branch~N` (turns) and `branch@{N}` (reflog).
- Reflog for every branch. `reset` and `rewind` can be undone.
- `rebase` and `merge` join append-only exchanges. `rebase` puts theirs first.
  `merge` orders exchanges by recorded time.
- `root print` and `--root` give a stable, read-only handle for one exact
  state, for automation.
- Export and import in a textual format. Import of foreign sessions.
- `compact` summarizes a conversation into a new chain. `clear` starts over.
- `history`, `transcript`, `dump`, `diff`, `stats` and `context` inspect state,
  token use, cost and context fill.
- `gc` collects the arena and the project storage.

### Models and providers

- Three API grammars: OpenAI Responses, OpenAI Chat Completions and Anthropic
  Messages.
- Built-in model catalogue with provider, endpoint, grammar, context window
  and output limit. Each branch owns its catalogue.
- Self-hosted OpenAI-compatible servers by an explicit `api_url`.
- Image and text-file attachments (`@PATH`, `/attach`, `/image`, `--image`).
- Reasoning effort and summary settings, structured output, and a budget limit
  for SDK runs.
- Retry with backoff for transient provider failures.
- Provider web search through hosted tools.

### Authentication

- API keys by environment variable or by a machine-local secret
  (`fyai secret`). Raw keys are rejected from stored configuration.
- ChatGPT subscription login for OpenAI Responses (`fyai auth openai`).
- OAuth for MCP servers, with dynamic client registration.
- Credential transport: a separate process holds the keys. Agents and tools run
  without them (Linux).

### Tools

- File tools: `read_file`, `write_file` and `apply_patch`. A patch can be a
  unified diff.
- Shell: `exec_command` with a time limit, a choice of shell, background
  commands and named terminal sessions (`shell_input`, `shell_output`,
  `shell_close`).
- `monitor` for a long-running command. Each output line reaches the model as
  an event.
- `wait`, `cancel`, `list` and `time` for work that the model started.
- `ask_user` asks one to four questions with two to four options each, with
  multiple choice, Markdown previews and background questions. `ask_parent`
  is the same for sub-agents.
- `todo_write` keeps a task list on the branch. The terminal UI shows it as a
  panel, and `fyai todo` prints it.
- `skill`, `settings` and `structured_output`.
- `fyai tool` runs one built-in tool from the command line.

### Sub-agents

- The `agent` tool runs a task on its own `agent:` branch. A forked agent
  inherits the conversation. A fresh agent starts from the task.
- Personas select instructions, reasoning, tools, model and time limit.
- Concurrent and background agents. `agent_message` and `agent_input` send
  text to a running agent.
- Optional view for each agent, and `project_view` to review and apply the
  result.
- `fyai agent` runs one transient agent. `fyai agent --rpc` serves it over
  JSON-RPC.
- `fyai acp` serves the Agent Client Protocol to an editor. `fyai claude-sdk`
  serves the Claude Agent SDK protocol.

### Sandbox (Linux)

- Landlock confinement of tool commands: project grant, extra paths, denied
  paths and TCP and UDP port limits.
- Profiles: `dev` (default), `lockdown`, `build`, `yocto` and your own.
  Profiles can extend each other.
- A private home for tools, with copied files and extra mounts.
- Cover of the project arena in a mount namespace (user namespace or
  bubblewrap). `fyai sandbox apparmor` writes the AppArmor profile that
  Ubuntu 24.04 needs.
- One scope for the processes of an exchange. A detached process lives until
  the exchange ends, and `list daemons` shows it.
- `sandbox_on_failure` chooses between a warning and a stop when the sandbox
  cannot be set up.

### Project views (Linux)

- A view is a private, writable overlay of the project. Changes stay in the
  view until you apply them.
- `view create`, `enter`, `diff`, `apply`, `ls`, `cp`, `rm` and `update`.
  Apply decides each path by a three-way check and reports conflicts.
- A whole session, or each sub-agent, can run in a view.
- `.gitignore` rules keep build output out of diffs and applies.
- Optional project state in each reflog entry, so `reset` rewinds the files
  too. `/undo` takes back the file changes of a group of tool calls.

### Terminal UI

- Fullscreen page with a scrollable transcript, or inline mode that keeps the
  scrollback.
- Work pane with one tile for each live shell or sub-agent, with mouse
  controls, scroll, zoom and a header panel.
- `!` runs a command of your own in a tile.
- Progressive Markdown, syntax highlighting and tables. Mermaid diagrams are
  drawn in the terminal, also in question previews. `fyai render` draws a file.
- Palette themes, configurable keys, prompt templates and completion
  (bash, zsh, fish).
- Slash commands for every verb, session settings, queued input during a turn,
  resume and a branch browser.
- `--cast` records a session. `fyai cast play` plays it.

### Integration

- MCP servers over Streamable HTTP or stdio, with `tool_search` and
  `mcp_resource`.
- Skills stored on the branch.
- Repository instructions from `AGENTS.md` and `CLAUDE.md`.
- Web page and desktop app served by a local bridge.
- JSON-RPC control protocol and the `schema` verb for the command and
  configuration schemas.

### Configuration and operation

- One layered configuration, checked against a schema. Each key has a model
  access level: `hidden`, `read`, `review` or `write`. The model proposes
  changes, and you review them.
- `--set` and `--delete` persist by default. `--transient` keeps a change to
  one invocation.
- Trace logs (`log wire`, `$FYAI_TRACE`), crash backtrace and a diagnostic
  dump on SIGUSR2.
- Native C build with CMake. Dynamic, mostly-static glibc and static musl
  builds. Linux and macOS.

## Quick start

```sh
cmake -S . -B build
cmake --build build
export PATH="$PWD/build:$PATH"

cd your-project
fyai init
export OPENAI_API_KEY=...
fyai "inspect this repository and propose the highest-value cleanup"
```

With a prompt, fyai runs the complete loop, stores the result, prints the
answer and exits. With no prompt, or with `-i`, it opens the interactive
terminal.

```sh
fyai transcript       # show the stored conversation
fyai context          # context fill and token estimate
fyai list models
fyai config effective
fyai help branch
```

## Documentation

- [User guide](doc/user-guide.md): workflows, configuration, tools, sandbox and
  command reference
- [fyai on macOS](doc/macos.md): install the release app and program, and
  store an API key in the Keychain
- [Command reference](doc/commands.md)
- [Annotated configuration](config.yaml.sample)
- [Changelog](CHANGELOG.md)
- [Branching](doc/branching.md)
- [Exchange scope](doc/exchange-scope.md) and
  [sandbox namespaces](doc/sandbox-namespaces.md)
- [Filesystem views](doc/agent-filesystem-views-sdd.md)
- [Transport isolation](doc/agent-transport-isolation-sdd.md)
- [Sub-agent protocol](doc/agent-protocol.md) and
  [fork model](doc/agent-fork-model.md)
- [Static builds](doc/user-guide.md#build)
- [System requirements and design](doc/srd/fyai-srd.md)

## Status

fyai is under active development. Interfaces can change. Unit and functional
tests cover the storage model, branches, the event loop, the terminal UI, tools,
OAuth and MCP.
