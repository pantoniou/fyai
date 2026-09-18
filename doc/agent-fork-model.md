# Sub-agent fork model

## Overview

A sub-agent runs in a child process. On Linux, the parent creates that child
with `fork()` and the child executes `fyai` again. The new image receives only
what the parent sends to it. The forked model, in which the child keeps the
address space of the parent and continues in `fyai_tool_child_serve_loop()`,
stays available.

This document records how each model starts a child, what a forked child keeps
and must disown, and what the parent sends to an executed child.

## The forked model

`fyai_tool_job_spawn()` creates each tool child. A shell child and a forked
sub-agent continue in the forked image. In the child:

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
does not apply to it, because inheritance stops being the default. This removes
the class of defect and not only the known instances of it: state added later
cannot leak.

The cost is one process start for each delegation. A complete invocation of
`fyai` measures approximately 48 ms, of which approximately 7 ms opens the
arena and resolves the configuration. A model request takes much longer.

### Selection

`fyai_agent_spawn_exec()` makes the decision for each delegation from the
`agent/spawn` configuration key: `exec`, the default, or `fork`. The forked
child is also used when:

- the run is `--transient`, because its state is in memory and not in the
  arena;
- the run has a pinned `--root`, which is read-only; or
- the platform has no supported path for executing the current image. Linux
  uses `/proc/self/exe`; macOS uses `_NSGetExecutablePath()`.

### Start

`fyai_tool_job_spawn()` forks, installs the terminal and descriptors 3 and 4 as
for a forked child, and then calls `fyai_tool_child_exec()`. That function does
not allocate. It clears close-on-exec on descriptors 3 and 4, restores the
signal mask, and executes:

```text
fyai [-d...] -b <parent branch> agent --tool-child [--pty] --arena <dir>
```

On Linux, `/proc/self/exe` is the image that the parent runs, also when a
rebuild replaced the binary on disk. On macOS, `_NSGetExecutablePath()` names
the executable; if that file is replaced before `execv()`, the replacement is
used. The new process opens the arena and serves the tool channel on
descriptors 3 and 4 through `fyai_tool_child_exec_serve()`. It resolves no
credentials at startup: it resolves them when the parent state arrives. It
applies the sandbox before it serves the call.

### What the parent sends

The `tool/run` request of an executed child has a `spawn` mapping.
`fyai_agent_spawn_state()` builds it:

| Key | Content |
| --- | --- |
| `config` | `cfg->config_doc`, the merged configuration of the run |
| `branch_config` | the configuration of the parent branch |
| `fork` | `{branch, head}`: the parent branch and the raw value of its head |
| `parent` | the agent execution of the parent |
| `api_key` | a `--api-key` value; never stored |

The child checks `config` against the schema, which rejects a raw key, and
adopts it before it parses the call. The call is in the grammar of that
configuration. `fyai_agent_run()` adopts the branch configuration and the fork
point after it reopens the arena.

### The fork point

A forked child reopens the arena, which reads the head of the parent branch
from the published root. The parent publishes only at the end of a turn, thus
the fork point of either model is the published head at the start of the turn
that delegates. No checkpoint publish is necessary.

The executed child finds `fork.head` in the ref log of `fork.branch` with
`fyai_root_find_head()`. It compares raw values and validates the matching
root. A publish on the parent branch after the spawn thus does not move the
fork point. A head that no root holds is a head that the parent did not
publish, such as that of a sub-agent that delegates. The child then uses the
published head, as a forked child does, and the trace records it. A session
that is not stored yet sends no fork point.

### What is not sent

The child reads the persona, the catalogue, and the branch of a revived
sub-agent from the arena. It inherits the environment, thus a `-e` file and a
provider key variable reach it. `fyai_env_sanitize()` is not applied to a
sub-agent, which needs its credentials; its own tool children sanitize again.

### Future work: a smaller spawn state

A delegation sends about 1 KB in a test arena. An arena seeded from
`config.yaml.sample` sends about 35 KB, because `config` and `branch_config`
each hold the full configuration document.

The two keys are almost the same document. `config` is `branch_config` with
the `--set`, `--config`, `-m`, and `--color` layers merged over it. The child
needs `branch_config` only for a session that is not stored yet: for any other
branch it reads the same value from the arena. Send `branch_config` only in
that case, or send only the keys that differ from `config`. Either change
halves the worst case.
