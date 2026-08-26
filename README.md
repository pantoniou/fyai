# fyai

**The process is temporary. The work is durable.**

`fyai` is a Unix-native AI coding agent written in C.

Each invocation opens the repository's durable AI state, runs the requested
verb or one complete model/tool loop, atomically publishes any durable changes,
and exits. There is no daemon and no hidden process state.

Interactive mode can keep that same process alive for a terminal session, but
the architecture stays the same: processes are temporary; durable state is
explicit.

fyai stores canonical, provider-independent conversation state locally in a
content-addressed libfyaml arena. That state has branch semantics, so agent work
can be forked, inspected, reset through reflogs, rebased, merged, and continued
through a different model or provider without turning a provider's wire
transcript into the identity of the conversation.

```sh
fyai "inspect this repository and propose the highest-value cleanup"
```

When the command returns to the shell, the process is gone. The work is still
there:

```sh
fyai transcript
fyai branch create alternative
fyai checkout alternative
fyai "try a different design"
```

## Why fyai exists

Most AI coding tools treat the agent as an application or service. fyai treats
it more like a Unix process.

That leads to a different set of properties:

- **The process is temporary; the work is durable.** Conversations,
  configuration, branches, provider observations, and reflogs live in a local
  content-addressed libfyaml arena under `.fyai/arena`.

- **History belongs to you, not to a provider.** fyai stores canonical,
  provider-independent conversation content. A branch can continue through a
  different model, provider, or supported API grammar without adopting a
  provider's wire transcript as its identity.

- **Branches are first-class agent work.** Every branch owns both its
  conversation and its configuration. Create alternatives, inspect symbolic
  history, reset safely through the reflog, rebase exchanges, or merge them in
  recorded-time order.

- **Delegated agents leave useful history.** An `agent` tool call runs on its
  own persistent `agent:` branch. Forked agents inherit the conversation at the
  delegation point; fresh agents begin from the delegated task. The parent gets
  the report while the complete delegated conversation remains inspectable.

- **Models and providers are replaceable.** The same durable conversation can
  continue across supported OpenAI Responses, OpenAI-style Chat Completions,
  and Anthropic-style Messages grammars, including compatible self-hosted
  endpoints.

- **One native event-driven terminal.** Streaming model output, progressive
  Markdown, concurrent tools, sub-agent work bands, readline editing, OAuth,
  MCP, signals, and child processes share one invocation-owned event loop.

- **Tools have explicit boundaries.** Built-in file, patch, shell, question,
  and delegation tools stay Unix-shaped. On Linux, shell sub-executions can be
  confined with Landlock, including project-relative path carve-outs and TCP
  port restrictions.

- **Secrets stay outside the arena.** Configuration stores credential
  indirections, not raw keys. API keys can come from environment variables or
  a machine-local logical secret; eligible OpenAI Responses requests can use a
  machine-local ChatGPT subscription login.

- **It is native software.** fyai is C, built with CMake, and supports normal
  dynamic builds, mostly-static glibc builds, and fully static musl builds.

## Quick start

Build and initialize a repository:

```sh
cmake -S . -B build
cmake --build build
export PATH="$PWD/build:$PATH"

cd your-project
fyai init
```

Configure a provider key using its conventional environment variable:

```sh
export OPENAI_API_KEY=...
```

Then run a task:

```sh
fyai "inspect this repository and propose the highest-value cleanup"
```

With a prompt, fyai runs the complete agent loop, commits the resulting state,
prints the answer, and exits. With no prompt—or with `-i`—it opens the
interactive terminal:

```sh
fyai
fyai -i
```

Useful first commands:

```sh
fyai transcript                 # render the durable conversation
fyai context                    # context fill and next-request estimate
fyai list models
fyai config effective
fyai help branch
```

## Durable state with branch semantics

The arena is an immutable, structurally shared value graph. Publication moves
one atomic root; it does not rewrite a mutable transcript database. That makes
state cheap to share across processes and gives branches git-like expectations:
a start point supplies both the conversation and the configuration in force
there.

```sh
fyai branch create experiment
fyai checkout experiment
fyai "try the alternative design"

fyai checkout -b historical main~4
fyai branch show historical
fyai list reflog
```

References are symbolic:

```text
main
main~2
main@{3}
```

`~N` walks stored turns and `@{N}` walks branch reflog entries. A reset is
recoverable while its old entry remains in the reflog:

```sh
fyai reset HEAD~2
fyai reset main@{1}
```

Joining branches orders append-only exchanges rather than merging text:

```sh
fyai rebase other    # theirs, then our exchanges
fyai merge other     # interleave exchanges by recorded time
```

For automation, convert a moving branch reference into a stable arena root
handle:

```sh
root=$(fyai root print main@{3})
fyai --root "$root" -b main dump state
```

A `--root` invocation is read-only. Old handles remain usable while their roots
are retained by the arena reflogs; garbage collection can expire them.

## Models and providers are replaceable

The built-in catalogue resolves a model selection into its provider, endpoint,
API grammar, wire model identifier, context window, and output limit.

```sh
fyai -m gpt-5.4-mini "review this patch"   # one invocation
fyai config set model gpt-5.4-mini         # persist on this branch
fyai list providers
fyai list models
fyai catalog
```

fyai supports three provider grammars:

- OpenAI-style Responses;
- OpenAI-style Chat Completions;
- Anthropic-style Messages.

The API grammar can also be selected explicitly:

```sh
fyai api responses
fyai api chat-completions
fyai api messages
```

Self-hosted OpenAI-compatible servers need no catalogue entry when the endpoint
is explicit:

```sh
fyai --set api=chat-completions \
     --set api_url=http://localhost:11434/v1/chat/completions \
     --set no_auth=true \
     -m llama3 \
     "inspect this tree"
```

Global `--set` and `--delete` operations persist by default; add `--transient`
for a one-invocation experiment. Dedicated selectors such as `-m`, `--theme`,
`--color`, and `--sandbox` are invocation-only.

## Authentication without repository secrets

The default API-key policy is indirect:

```yaml
api_key: { type: auto }
```

After resolving the provider, fyai tries an explicit `--api-key`, the
provider's conventional `<PROVIDER>_API_KEY` environment variable, then the
logical secret `api-key/<provider>`. Raw keys are rejected from persisted
configuration.

On Linux, manage logical secrets without exposing their values:

```sh
fyai secret set api-key/openai
fyai secret status api-key/openai
fyai secret delete api-key/openai
```

Eligible OpenAI Responses requests can instead use ChatGPT subscription access:

```sh
fyai auth openai login
fyai auth openai login --device-code
fyai auth openai status
fyai auth openai usage
fyai config set auth chatgpt
```

Subscription credentials are machine-local and never enter the repository
arena or model tool arguments.

## Interactive work without a daemon

Interactive mode is an invocation-local REPL over the same durable branch
state. It supports event-driven line editing, terminal resize and interrupt
handling, progressive Markdown rendering, concurrent tool output, and branch or
request-setting changes without restarting the process.

```text
/branch experiment
/model gpt-5.4-mini
/api responses
/context
/status
/history last 10
/compact preserve the unresolved test failures
/help
```

Request-shaping changes such as `/model`, `/api`, `/effort`, and
`/temperature` persist to the current branch. `/theme`, `/tool-detail`, and
`/transcript-system` persist too. The remaining presentation toggles—including
`/markdown`, `/stream`, and `/thinking`—apply to the live session only.

## Delegated agents

When the model calls the `agent` tool, fyai allocates a named child branch below
the current branch:

```text
main/agent:reviewer
```

```sh
fyai "delegate independent reviews of the parser and event loop"
fyai branch --all
fyai --branch main/agent:reviewer transcript
```

Delegations may run concurrently. A configured persona can select instructions,
reasoning policy, reasoning visibility, timeout, and—for fresh context—the
model. A forked agent keeps the parent's resolved model and conversation
history. Agent names are durable handles, so a name clash fails and lets the
model retry with a new one rather than silently adding an ordinal.

The standalone verb is intentionally different:

```sh
fyai agent "find the parser entry points and report their responsibilities"
```

It runs one transient restricted agent, prints the final report, and writes no
conversation history. `fyai agent --rpc` exposes the same transient worker over
the documented JSON-RPC control protocol.

## Sandboxed tools

Enable Linux Landlock confinement for one invocation:

```sh
fyai --sandbox "run the tests and diagnose failures"
```

Or persist a policy:

```sh
fyai sandbox on
fyai sandbox show
```

The policy can grant paths outside the project, carve paths out of the project
grant, and restrict TCP egress:

```yaml
sandbox:
  enabled: true
  deny: [secrets, ~/.ssh]
  allow:
    - { path: /opt/toolchain, mode: ro }
    - { path: /work/generated, mode: rw }
  network:
    ports: [443]
```

The repository `.fyai` directory is always denied to sandboxed tools. Landlock
is Linux-specific and best-effort when the host kernel lacks required features;
the portable command/tool policy remains separate.

## MCP tools

fyai can connect to named MCP servers over Streamable HTTP or local stdio:

```yaml
mcp:
  enabled: true
  startup_wait: true
  servers:
    docs:
      endpoint: https://example.invalid/mcp/
    local:
      transport: stdio
      command: npx
      args: [-y, "@modelcontextprotocol/server-filesystem", .]
```

Discovered tools are exposed as `mcp__<server>__<tool>`. Interactive `/mcp`
commands report connection state, enable or disable MCP, and drive per-server
OAuth login and logout where configured.

## Inspection and maintenance

The human transcript and canonical/provider views serve different purposes:

```sh
fyai transcript              # rendered conversation
fyai dump state              # canonical provider-independent state
fyai dump anchors            # full stored turn graph
fyai dump providers          # provider wire observations
fyai stats --json            # cumulative usage for this conversation chain
fyai context                 # context-window fill and next-request estimate
```

Trace logging and arena maintenance are explicit commands:

```sh
fyai log wire start
fyai log wire view
fyai log wire stop
fyai gc --keep-reflogs 100
```

## Static builds

A mostly-static build links fyai's dependencies statically while leaving glibc
and the normal host runtime dynamic:

```sh
cmake -S . -B build-static -DFYAI_MOSTLY_STATIC=ON
cmake --build build-static
```

For a fully static musl binary, use the Docker builder:

```sh
MODE=musl scripts/build-static-docker.sh ./fyai
```

## Project status and documentation

fyai is under active development. The storage model, provider-independent
conversation, branch semantics, event loop, terminal UI, tool execution, OAuth,
and MCP paths are implemented and covered by unit and functional tests, but
interfaces may still evolve.

- [User guide](doc/user-guide.md) — workflows and command reference
- [Annotated configuration](config.yaml.sample) — supported keys and defaults
- [Branching design](doc/branching.md) — branches, refs, roots, joins, and GC
- [Sub-agent protocol](doc/agent-protocol.md) — transient agent JSON-RPC
- [Sub-agent fork model](doc/agent-fork-model.md) — what a forked child keeps, and the exec alternative
- [System requirements and design](doc/srd/fyai-srd.md) — authoritative design

Start with `fyai help VERB`, `/help` in an interactive session, and
`fyai config describe [PATH]` for the portions of the running binary they cover.
