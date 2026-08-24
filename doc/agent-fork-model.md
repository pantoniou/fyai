# Sub-agent fork model

## Overview

A sub-agent runs in a child process. The parent creates that child with
`fork()` and does not call `exec()`. The child keeps the address space of the
parent and continues in `fyai_tool_child_serve_loop()`.

This document records what the child keeps, what it must disown, why the model
was chosen, and what an `exec()` model would need.

## The current model

`fyai_tool_job_spawn()` creates each tool child, a sub-agent included. In the
child:

1. `setsid()` makes the child a session leader, so a signal to the parent
   group does not reach it;
2. `fyai_ctx_loop_abandon()` releases the event loop of the parent;
3. `fyai_ctx_fork_disown()` releases the live state of the parent;
4. a terminal, when the parent supplied one, becomes the standard three
   descriptors;
5. the control channel becomes descriptors 3 and 4; and
6. `cfg->tool_child` records that this process is a tool child.

The child then serves one call and exits through `_exit()`.

A sub-agent does more than another tool child. `fyai_agent_run()` reopens the
arena, applies the persona, makes a curl handle of its own, and runs a complete
model loop on its own branch.

## What a fork copies

A `fork()` copies everything. Most of it is correct: the configuration, the
arena and its builders, the sink, and the credentials all describe the work,
and the child needs them.

The rest is live state of another process. A copy of such state addresses that
process by mistake, or answers with what belongs to it. Three defects of this
class were found in use:

- A named shell session. The record named a job on the control channel of the
  parent and a view of the program of the parent. The sub-agent found the name
  taken, and a read of the session returned what the program of the parent
  wrote. The sub-agent then reported that output as its own result.
- A named wait. The sub-agent found the name taken. The timer named a loop the
  child had abandoned.
- A tool job. The copy holds descriptors of a sibling job, which keeps the
  pipes of that job open. A resize writes on a terminal this process does not
  own. `agent_input` can address a sub-agent that belongs to the parent.

## The disown list

`fyai_ctx_fork_disown()` is the one place that releases this state. It ends
nothing and signals nothing: each item belongs to the parent and continues
there. It releases:

| State | Why the copy is wrong |
| --- | --- |
| `shell_sessions` | names a program and a channel of the parent |
| `tool_jobs` | holds descriptors of the children of the parent |
| `waits` | takes the names, and the timers name an abandoned loop |
| `events` | queued for the model of the parent, not for this one |
| `patch_views`, `patch_display` | resolved for calls of the parent |
| `display_output` | the transcript document the parent has open |
| `ui`, `shell_stream`, `tty_session`, `winch_src` | the display of the parent |
| `config_edit` | an editor request of the parent |
| `mcp`, `mcp_tools` | connections to servers the parent started |
| usage counters, `last_token_extents` | the accounting of the run of the parent |

When you add live state to `struct fyai_ctx`, add it to this list.

### What stays

The environment is sanitized separately. `fyai_env_sanitize()` removes each
provider credential before a child runs another program. It does not touch the
context.

One item is deliberately kept. The child does not release the inherited curl
handle. `curl_easy_cleanup()` can write a TLS shutdown on a socket that the
parent still uses. A sub-agent makes a handle of its own with
`fyai_curl_easy_reinit()`, and the copy holds its descriptors until the child
exits.

## The exec model

An `exec()` model gives the child nothing but what it is told. The disown list
becomes unnecessary, because inheritance stops being the default. This removes
the class of defect and not only the known instances of it: state added later
cannot leak.

The cost is one process start for each delegation. A complete invocation of
`fyai` measures approximately 48 ms, of which approximately 7 ms opens the
arena and resolves the configuration. A model request takes much longer. Thus,
the cost is not the reason to keep the fork model.

Part of the mechanism exists. `fyai agent --rpc` serves one standalone
sub-agent over JSON-RPC 2.0 on standard input and output; `doc/agent-protocol.md`
records it. That worker is transient: it shares the workspace, it does not
publish arena state, and it can neither ask questions nor delegate.

### What an exec model must supply

Four things reach a sub-agent today through memory alone.

1. **The conversation of a fork.** `context: fork` starts the child at
   `ctx->last_message`, which is the turn of the parent that is not published
   yet. An executed child cannot read it from the arena. Either the parent
   publishes at the delegation point, which changes when state becomes durable,
   or the conversation goes in the start frame. `context: fresh` and a revived
   sub-agent need neither: each is addressed by branch.
2. **The merged configuration.** `--transient` puts a builder above the durable
   arena, and `--config` and `--set` do not have to be persisted. An executed
   child that reads the arena would run a different model or grammar from the
   one the parent intends. The merged document must go in the start frame.
3. **The descriptors.** Descriptors 3 and 4 are a private contract today,
   because there is no `exec()`. A terminal makes standard output unavailable
   to the protocol, thus an executed sub-agent needs the same two descriptors
   as a stated contract.
4. **The protocol.** Delegation carries `tool/progress`, an `ask_user` relay,
   `agent_input`, and delegation from a sub-agent. The public agent protocol
   supports none of the last three.

Credentials, confinement, and process isolation need no work. Landlock
restrictions continue through `exec()`, `setsid()` is called before it, and a
child resolves the credentials it needs by itself.

### The open decision

Item 2 makes an existing dependency explicit and is a small change. Item 1 is a
design decision: it asks when a turn becomes durable. Settle that question
before code moves.

Until then, the fork model stands and `fyai_ctx_fork_disown()` is the boundary.
It is one list in one place, which is what keeps the model safe to extend.
