# fyai

`fyai` is a Unix-style AI coding agent built for the next phase of AI tools.

The first wave of agents treated the model as the product. That world is
already changing. Models are becoming cheaper, more interchangeable, and more
widely available. The durable value is shifting to the system around the model:
who controls the data, how state is stored, how work is audited, and whether the
tool can survive a change of provider without dragging your history with it.

`fyai` exists for that world.

It is not a long-running daemon, desktop app, or hidden background service. Each
invocation starts, loads the durable conversation state, performs the requested
agent loop, commits canonical state, and exits. Startup is designed to be
measured in milliseconds, not seconds, because the state lives in a compact
content-addressed storage engine rather than in a resident process.

## Why it is different

- **You control the data.** Conversation state is stored locally in durable
  libfyaml arenas. The storage is deterministic, content-addressed, and built for
  structural sharing rather than opaque provider logs.

- **No resident agent process.** There is no daemon to keep alive and no hidden
  process state. The filesystem and the arena are the source of truth.

- **Fast startup.** The storage engine is designed for millisecond process
  startup, so using the agent feels like running a normal Unix command.

- **Provider freedom.** `fyai` supports the three major assistant API shapes:
  OpenAI-style Responses, Chat Completions, and Messages APIs. That makes it
  practical to mix and match models and providers without rewriting the tool or
  abandoning your stored state.

- **Plain C implementation.** `fyai` is written in C. That matters: low memory
  use, predictable startup, straightforward static builds, and no giant runtime
  stack just to run a command-line agent.

- **Packageable open source.** The project is fully open source and built like
  normal Linux software. It is suitable for standard distribution packaging
  without vendoring a jillion language-level dependencies.

## Beta status

This is a first beta drop. The core direction is in place: local durable state,
catalogue-driven provider selection, markdown rendering through native C
libraries, and a Unix-shaped tool surface for coding work.

Expect rough edges. The point of this beta is to make the architecture usable
early, gather real workflows, and keep tightening the storage, provider, and
tool layers without compromising the basic premise: the agent should be
stateless as a process, durable as a tool, and yours as data.

## Building

```sh
cmake -S . -B build
cmake --build build
```

Portable mostly-static builds are supported when the required static archives
are available. This links fyai's dependencies statically but leaves glibc
dynamic, which avoids fully-static glibc `getaddrinfo()`/NSS failures on other
hosts:

```sh
cmake -S . -B build-static -DFYAI_MOSTLY_STATIC=ON
cmake --build build-static
```

For a truly self-contained binary, build the musl static Docker target:

```sh
MODE=musl scripts/build-static-docker.sh ./fyai
```

Mostly-static builds include compatibility wrappers for glibc's C23 symbol
redirects so a binary built on a new glibc does not pick up a `GLIBC_2.38`
runtime requirement just from `strtol`/`sscanf` family calls.

## Quick start

Initialize a repository-local `.fyai` directory:

```sh
fyai init
```

This creates the local arena used for durable conversation state and installs
the starting configuration for the repository.

Configuration never contains raw provider secrets. Set `OPENAI_API_KEY`,
`OPENROUTER_API_KEY`, `ANTHROPIC_API_KEY`, or another provider's conventional
`<PROVIDER>_API_KEY` variable, or use an `api_key: { type: env, value: NAME }`
configuration mapping. The configured `model` is resolved through the
catalogue to its provider, endpoint, API grammar, and wire model ID.

Run `fyai` with a prompt to use it as a normal coding-agent command:

```sh
fyai "inspect this tree and suggest the next cleanup"
```

In this mode, `fyai` runs one complete tool-use loop, commits the resulting
state, prints the assistant response, and exits. Tool use is supported: the
agent can read files, apply patches, write files, run shell commands, and ask
the user for clarification according to the configured tool policy.

Run `fyai` with no prompt, or pass `-i`, for interactive mode:

```sh
fyai
fyai -i
```

Interactive mode keeps the same durable state model, but lets you enter turns
one after another from the terminal.

Show the stored conversation history:

```sh
fyai history
```

`history` renders the committed conversation state as readable markdown,
including user turns, assistant replies, tool calls, and tool results. It is a
view of the local durable state, not a separate transcript format.

## Usage

Run a prompt:

```sh
fyai "explain this repository"
```

Show stored conversation history:

```sh
fyai history
```

List configured providers:

```sh
fyai list providers
```

Use `fyai help` for the current command reference. See `config.yaml.sample`
for the configuration keys and `doc/srd/fyai-srd.md` for the Phase 1 storage
and provider contract.
