# The Claude Code backend

The `claude-code` backend runs the installed Claude Code CLI as the model of
fyai. The CLI signs in with its own login, such as a Claude subscription.
fyai runs every tool call through its own tool path: its tools, its
sandbox profile, its gate and its transcript. The CLI only writes the
replies.

## Select the backend

Give the model as `claude-code/MODEL`, or set `api` to `claude-code`:

```sh
fyai -m claude-code/sonnet "explain this repository"
fyai --set api=claude-code --set model=opus
```

`/model claude-code/MODEL` (or `fyai model claude-code/MODEL`) switches a
session to the CLI and stores the grammar with the model. While the CLI
serves the session, a bare model, such as `/model opus`, stays with it. A
model that a catalogue provider pins, such as `/model openai/gpt-5.4`, leaves
it. A change of model starts a new CLI.

The CLI resolves MODEL: an alias (`sonnet`, `opus`, `haiku`, `fable`) or a
full model name, such as `claude-sonnet-5-5`. `default` lets the CLI choose. The configuration key
`claude_code` sets the program:

| Key | Default | Effect |
| --- | --- | --- |
| `claude_code/command` | `claude` | The CLI, as a name on `PATH` or a path. |
| `claude_code/api_billing` | `false` | Allow a CLI that bills an API key. |

## Authentication

The CLI owns its login and refreshes its token. fyai gives it no API key:
the CLI starts with a sanitized environment that keeps `PATH`, `HOME`, the
locale, the XDG directories, `CLAUDE_CONFIG_DIR`, `CLAUDE_CODE_OAUTH_TOKEN`
and the proxy variables. With `api_billing`, it also keeps
`ANTHROPIC_API_KEY`.

Before fyai sends any conversation content, it reads the account that the
CLI reports in its answer to `initialize`. The run stops with a diagnostic
when the CLI:

- would bill an API key (`apiKeySource`): remove that key from the
  configuration of the CLI, or set `claude_code/api_billing`;
- uses a provider other than Anthropic (`apiProvider`), such as Bedrock or
  Vertex: set `claude_code/api_billing` to use it; or
- has no subscription login: run `claude` and sign in with `/login`.

## How a request runs

A CLI process serves an exchange: the user message and every model reply and
tool call that follows it, and, while it can, the exchanges after it
([The life of a CLI](#the-life-of-a-cli)). fyai starts it with:

```text
claude -p --input-format stream-json --output-format stream-json --verbose
       --include-partial-messages --no-session-persistence --tools ""
       --mcp-config '{"mcpServers":{"fyai":{"type":"sdk","name":"fyai"}}}'
       --strict-mcp-config --allowedTools mcp__fyai --setting-sources ""
       --disable-slash-commands --system-prompt PROMPT [--model MODEL]
```

- `--tools ""` turns off each built-in tool of the CLI. The tools of fyai
  are the only tools: fyai serves them as the SDK MCP server `fyai`, and the
  model sees `mcp__fyai__NAME`. fyai removes the prefix, so the stored calls
  have the names of fyai.
- `--allowedTools mcp__fyai` keeps the CLI from asking about a call. fyai
  decides, as it does for every other model.
- `--setting-sources ""` and `--strict-mcp-config` keep the settings, hooks
  and MCP servers of the user out of the CLI. The CLI writes no session file.

The CLI calls a tool with an `mcp_message` request of `tools/call`. The
request names its `tool_use` block in `_meta`, `claudecode/toolUseId`, so
fyai answers it with the result of that call and of no other. A request for
a call that no reply of the exchange made, or for a call that already has
its answer, gets an error and runs nothing.

The CLI sends each reply as Anthropic Messages stream events. fyai reads them
as a Messages reply (`src/provider/fyai_grammar_claude_code.c`): the text and
the tool calls stream in the transcript as they arrive, and the turn stores
the canonical shape of a Messages reply. A model step ends at `message_stop`.
fyai then runs the calls of the reply, and the next model step gives the
results to the CLI, which goes on with the same process.

## The life of a CLI

A CLI whose turn ended well is kept for the next exchange of the same
process, which gives it the new user messages alone. It is kept while the
branch grows only by its own answers and new user messages, and the model,
the system prompt and the tools stay as it started with them. Each request
names its stored messages by their words in the arena, which keeps one value
for equal content, and the CLI records the ones that it has seen. A request
that does not continue them, such as one after `/checkout`, `/rewind`,
`/compact`, `/clear` or a merge, or after another process stored on the
branch, starts a new CLI that reads the branch. The CLI of an exchange that
was cancelled, or that failed, is stopped when the exchange ends.

A CLI that leaves gets EOF on its input, and fyai reaps it. A CLI that does
not leave gets SIGTERM, then SIGKILL. The CLIs end when fyai ends.

## History

The CLI cannot take earlier turns as structured messages, so a new CLI reads
the branch as text: the first user message holds the earlier conversation in
`<conversation_history>`, then the last user message, then, in `<progress>`,
what was already done for it. The calls and results of earlier exchanges are
in the transcript, with their arguments and output.

The cost of the replay: the system prompt and the tool declarations come
first in each request, and do not change from one process to the next, so
the provider cache serves them. The transcript is new in each process and
costs cache writes. In a measured branch of five exchanges and eight model
calls, the input was 106,505 tokens, of which 88,935 (83.5%) came from the
cache and 17,554 were cache writes; the transcript grew to 1,199 bytes. That
run started a new fyai process, and so a new CLI, for each exchange. A kept
CLI pays for no replay.

## Failures and cancellation

| Event | Result |
| --- | --- |
| The CLI cannot start, or refuses `initialize` | The turn fails with the cause. |
| The account check fails | The turn fails before any content is sent. |
| The CLI exits during a reply that showed no content | The request is retried with a new CLI. |
| The CLI exits in the words of a reply | The reply ends as a paused turn: its words stay, and a new CLI continues from them. |
| The CLI exits in a tool call of a reply | The turn fails with the last line of its standard error: the input of the call is cut. |
| The CLI exits while fyai runs the calls of a reply | The next step starts a new CLI with the calls and their results. Each call runs once. |
| A kept CLI exits between exchanges | The next exchange starts a new CLI that reads the branch. |
| The CLI ends its turn with an error result | The turn fails with its text; an API status is retried as for HTTP. |
| A line that is not JSON | The line is dropped. |
| An interrupt or a cancellation | The CLI is stopped with its reply. The completed steps of the turn stay. |

## Credential isolation

With `agent/transport_isolation`, the credential transport runs the CLI, as it
runs the HTTP requests of the other providers. The configuration states a
program profile: the CLI and `api_billing`, from trusted configuration, so a
request names the profile and never the program. The agent sends its
claude-code request to the transport and reads the reply frames as it reads
an HTTP reply. The transport keeps one CLI session for each execution, by
the rules of [The life of a CLI](#the-life-of-a-cli). The agent sends
`EXCHANGE_END` when its exchange ends, and the transport then stops a CLI that
cannot serve the next one; the retirement of the execution stops it too. The CLI, its login and its
token are then in no agent and no tool, sub-agents in views included
(`doc/agent-transport-isolation-sdd.md`).

## Views

Without the transport, an agent runtime runs the CLI itself. In a view, the
outside of the runtime is read-only, so the CLI could not write its state or
renew its token. The grammar grants the configuration directory of the CLI
(`$CLAUDE_CONFIG_DIR`, else `~/.claude`) writable, and the view mounts it so
(`doc/agent-filesystem-views-sdd.md`, section 8). The home stays read-only, so
in a view the CLI gets `CLAUDE_CONFIG_DIR` and keeps its `.claude.json` in that
directory. The tools of the runtime get none of this.

## Limits

- The system prompt goes in one argument and is limited to 120 KiB.
- `/compact` with the Responses compact endpoint is not available.
- Web search runs at a provider and is off: the CLI has no tools of its own.
- In a view without the transport, the CLI keeps its `.claude.json` in its
  configuration directory, not in the home, so it does not share the one that
  an unconfined CLI uses.
