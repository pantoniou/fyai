# Gap analysis: shell and agent tools

This document compares the shell tools and the agent tools that `fyai`
supplies with the ones that the catalogue records for the agents it knows.
It says what is absent, what the absence costs, and what each item needs.

## 1. Method

The comparison uses two sources.

- The catalogue, `data/catalog.yaml`. Its `agents:` list holds three agents —
  `claude_code`, `codex` and `opencode` — and each one records the full
  description and JSON schema of every tool that agent supplies. This is a
  record of what those agents do. It is not a specification for `fyai`.
- The tools that `fyai` supplies, in `make_tools()` in `src/utils.c`, and the
  behaviour behind them in `src/fyai_tools.c` and `src/fyai_agent.c`.

**The catalogue does not drive the tools that `fyai` sends.** `make_tools()`
writes the schemas in C. The `agents:` block of the catalogue is used only by
`fyai catalog tools`, which shows it, and by the system prompt of a native
agent. The two can therefore move apart without an error, and this document
records how far apart they are now.

## 2. What `fyai` supplies

`make_tools()` supplies six tools:

| Tool | Parameters | Required |
| --- | --- | --- |
| `read_file` | `path` | `path` |
| `write_file` | `path`, `content` | both |
| `apply_patch` | `patch` | `patch` |
| `shell` | `command`, `workdir`, `timeout`, `description` | `command` |
| `ask_user` | `question`, `options` | `question` |
| `agent` | `name`, `description`, `task` | all three |

A sub-agent gets the same set less `agent` and `ask_user`
(`make_tools_filtered()`).

## 3. Shell tools

### 3.1 What the catalogue records

| Agent | Tool | Parameters |
| --- | --- | --- |
| `claude_code` | `Bash` | `command`, `timeout`, `description`, `run_in_background`, `dangerouslyDisableSandbox` |
| `codex` | `exec_command` | `cmd`, `workdir`, `shell`, `login`, `tty`, `yield_time_ms`, `max_output_tokens`, `justification`, `prefix_rule`, `sandbox_permissions` |
| `codex` | `write_stdin` | `session_id`, `chars`, `yield_time_ms`, `max_output_tokens` |
| `opencode` | `bash` | `command`, `timeout`, `workdir`, `description` |

### 3.2 Comparison

`fyai` declares its shell tool as `exec_command` and supplies `command`,
`workdir`, `timeout`, `description`, `shell`, `login` and `tty`.

| Function | claude_code | codex | opencode | fyai |
| --- | --- | --- | --- | --- |
| The command | yes | yes | yes | **yes** |
| A terminal | no | `tty` | no | **yes** |
| Time limit | `timeout` | `yield_time_ms` | `timeout` | **yes** |
| Working directory | no | `workdir` | `workdir` | **yes** |
| A label for the display | `description` | `justification` | `description` | **yes** |
| The shell, and its profile | no | `shell`, `login` | no | **yes** |
| Run in the background | `run_in_background` | (session) | no | **no** |
| Write to the standard input | no | `write_stdin` | no | **yes** |
| A session that stays open | no | `session_id` | no | **yes** |
| Limit the output | no | `max_output_tokens` | no | partly |
| Control of the sandbox | `dangerouslyDisableSandbox` | `sandbox_permissions` | no | **no** |
| Approval of a command prefix | no | `prefix_rule` | no | **no** |

### 3.3 The gaps that matter

**A time limit is supplied.** The `timeout` parameter gives a limit in
milliseconds. `shell/timeout_ms` in the configuration (120000 by default)
applies when the model asks for no limit, and `shell/max_timeout_ms` (600000)
bounds the limit that the model can ask for. A zero disables each of them. The
native `shell_call` grammar has no parameter for a limit, thus it gets the
configured default.

The limit is a property of the **job**, and not of the command inside it. A
tool call runs in a job child that is a session leader, thus the parent stops
the full process group with one operation. This is necessary because `/bin/sh`
usually forks and does not exec: a limit applied inside the job child stops the
shell and leaves the process that the shell started. The same machinery gives a
limit to an agent call (`agent/timeout_ms`, with no limit by default), because
an agent call is also a job. The parent adds the reason to the result, because
from inside the job child a group terminate looks the same as an interrupt.

**A working directory is supplied.** The `workdir` parameter does the `chdir`
in the child. It is done before the sandbox is applied, because the confinement
is one-way and the target must stay reachable. Status 125 keeps a directory
that cannot be entered different from the 126 and 127 of the shell.

**A label for the display is supplied.** The `description` parameter adds a row
to the header of the tool call, with `workdir` and `timeout`. A parameter that
the model does not give adds no row.

**A command that was stopped now returns immediately.** `/bin/sh` usually forks
and does not exec, thus a descendant keeps the capture pipes open for the full
time that the command would have run. A wait for EOF on those pipes made a
cancelled or timed-out command return only when it stopped by itself. The
capture is now complete when the child is reaped. This also shortens the
interrupt path.

**A background command is absent.** `run_in_background` needs a job that lives
longer than the tool call, and a way to collect its output later
(`BashOutput`/`KillShell` in `claude_code`, which the catalogue does not
record). `fyai` has no such concept. Refer to section 3.4 for the limit that
applies.

**A shell session that stays open is absent, and so is `write_stdin`.** A
forked tool child gets `/dev/null` as its standard input by design
(`src/utils.c`), thus a command cannot be answered. This blocks every
interactive program. `codex` solves it with a session that stays open and a
second tool that writes to it. Refer to section 3.4.

**Control of the sandbox by the model is absent.** `fyai` decides
confinement from `--sandbox` and the configuration. `claude_code` and `codex`
both let the model ask for more. This is a **deliberate difference and should
stay**: the SRD makes command admission a matter of policy, not of what the
model asks for. Record it as a decision and not as a gap.

### 3.4 The limit is the invocation, not the tool call

It is easy to say that a session, or a job in the background, needs state that
continues after the invocation, and that this is contrary to the rule that one
invocation is one loop and then an exit. **For a session this is not correct,
and the difference is important.**

The `session_id` of `codex` must stay usable while the agent works. That is one
loop of tool calls. In `fyai` that is one invocation. Thus a session can live
in the context, be closed when the invocation ends, and never touch the rule at
all.

The limit is therefore:

| Continues after | Possible now |
| --- | --- |
| One tool call | **Yes.** This is only a longer life for data that exists. |
| The invocation | No. This needs a decision about the arena first. |

A session and a shell in the background are both in the first row while the
agent is working. Only a job that a later invocation must find is in the
second row.

### 3.5 What a PTY needs

`fyai` gives a tool child two pipes and `/dev/null`, thus a program sees no
terminal. To supply one has three parts, and they are not equally difficult.

**The terminal itself. Small.** `openpty()`, then `setsid()` and `TIOCSCTTY`
in the child to make the slave its controlling terminal, and `TIOCSWINSZ` to
give it a size. Two pipes become one master descriptor. `fyai` already calls
`setsid()` in the tool child, which is the part that is easy to do wrongly.

**The session. Medium, and mostly built.** Look at `struct fyai_tool_job`: it
already holds the context, a control channel to the child, the process
identifier, the descriptors, an event source, an output stream, a display
band, a group and the state to stop it. That is a session. It is only released
when the tool call ends. What is absent is a register of sessions by
identifier, a way to write, a ring buffer with the output limit, and a release
of all sessions when the invocation ends.

**The interpretation of the screen. This is the true subsystem.** The bytes
from a terminal are not usable by a model as they are: they hold cursor
movements, and a program that draws a progress bar sends thousands of carriage
returns. What is of use is the *screen* and not the record of bytes. To give
that, put the stream through `libfyvterm` and supply the result. `fyai`
already uses `libfyvterm` to test its own display, thus the dependency is
present. The work that stays is the design: how much history to keep, what to
do about the alternate screen, and how to put a screen into a message.

**A caution that changes the order of work.** To add a terminal *without* a way
to write to it can make the result worse. A program that finds no terminal
usually takes the default and continues; a program that finds one will show a
prompt and wait for ever. Thus the terminal and `write_stdin` are one change,
or the time limit of section 3.3 must come first.

The reading follows what the program did. A program that writes lines is read
as lines, whole and in the order of arrival. A program that draws, such as an
editor, is read as the screen that it drew. Its bytes are cursor movements and
give nothing on their own.

**What is supplied now.** `shell` takes `tty`, `rows` and `cols`. The result
is the screen, with the lines that scrolled off it. The size follows the window
of the user. The parent watches `SIGWINCH`, because a tool child calls
`setsid()` and the kernel cannot signal it, and sends the new size on the
control channel. The alternate screen is not enabled, so a full-screen program
leaves its last screen. `write_stdin` and a session that stays open are the
next step. Refer to `doc/pty-terminal-plan.md`.

## 4. Agent tools

### 4.1 What the catalogue records

| Agent | Tool | Parameters |
| --- | --- | --- |
| `claude_code` | `Agent` | `description`, `prompt`, `subagent_type`, `model`, `run_in_background`, `isolation` |
| `opencode` | `task` | `description`, `prompt`, `subagent_type`, `task_id`, `command` |

`claude_code` also records a family of tools around the first one:
`SendMessage` (continue an agent that already ran), `TaskCreate`, `TaskGet`,
`TaskList`, `TaskUpdate`, `TaskOutput`, `TaskStop` and `Monitor`.

`codex` has no tool for delegation.

### 4.2 Comparison

| Function | claude_code | opencode | fyai |
| --- | --- | --- | --- |
| Delegate a task | `Agent` | `task` | **yes** |
| A label for the display | `description` | `description` | **yes** |
| Choose the kind of agent | `subagent_type` | `subagent_type` | **no** |
| Choose the model | `model` | no | **no** |
| Run in the background | `run_in_background` | no | **no** |
| Continue an agent | `SendMessage` | `task_id` | **no** |
| Isolate the workspace | `isolation` | no | **no** |
| Keep a list of tasks | `Task*` | `todowrite` | **no** |
| Watch for a condition | `Monitor` | no | **no** |
| An agent inside an agent | yes | yes | **no** |

### 4.3 The gaps that matter

**The kind of agent cannot be chosen.** `fyai` has exactly one sub-agent
persona, the string `fyai_agent_system_prompt` in `src/fyai_agent.c`. The
`name` that the model gives is used **only to label the display**
(`src/fyai_tools.c`); it does not reach `fyai_agent_run()`, which takes only
the task. Both of the other agents let the model select a persona with its own
instructions and its own restricted tool set.

This is the **largest difference in kind**, not only in degree: a "search
agent" and a "review agent" are different because their instructions and their
tools differ, and `fyai` cannot express either. The catalogue is the natural
place for such a list, next to the `agents:` block it already holds.

**An agent cannot start an agent.** `make_tools_filtered()` removes `agent`
from the tool set of a sub-agent, thus delegation is one level deep.

`doc/branching.md` and `CLAUDE.md` now state this one-level restriction.
`agent/max_branch_depth` remains a defensive allocator limit and reserves the
policy for a future recursive implementation.

**The conversation of a sub-agent is now kept.** Each call publishes its
conversation to its own branch below the branch that started it
(`doc/branching.md` section 9), thus what an agent did can be examined with
`fyai --branch <name> transcript` after it stops. The name of the agent is the
name of the branch (`main/agent:explore`); a name that is taken fails the call,
thus the model chooses another one.

**The kind of agent can now be chosen.** `agent/personas` in the configuration
names each persona and carries its instructions and the settings of its model
(`model`, `reasoning`, `temperature`, `max_tokens`, `thinking`, `timeout_ms`,
`context`). The `persona` parameter of the agent tool selects one, and the tool
schema lists the configured names with their descriptions, thus the model
chooses from what exists. A key that a persona does not set keeps the value in
force. A persona's `model` applies to a fresh agent; a forked agent always
inherits the parent's resolved provider/model.

**A sub-agent starts from the conversation that made it.** `context: fork`,
the default, branches at the head of the branch that started it, thus the
caller does not have to summarize the context into the task. `context: fresh`
keeps the older behaviour for self-contained work.

**An agent cannot be continued yet.** The `claude_code` `SendMessage` and the
`opencode` `task_id` both let a second message reach an agent that already ran,
keeping its context. The branch is the state that such a message needs, thus
this is now a small step: start from the branch instead of from an empty
conversation.

**A background agent and a task list are absent.** Section 3.4 applies to
these as well: an agent that runs while the loop continues is possible now,
because it ends with the invocation. A task list that a later invocation must
find is not, because it must be kept in the arena. `Monitor` needs more than
either — a process that watches while nothing else happens — and does not fit
the rule at all.

## 5. Summary and order of work

| Gap | Cost | Objection | Do |
| --- | --- | --- | --- |
| Shell time limit | Low | None | **Done** |
| Shell `workdir` | Low | None | **Done** |
| Shell `description` | Low | None | **Done** |
| Persona for a sub-agent | Medium | Config, not the catalogue | **Done** |
| An agent inside an agent | Low | Contradiction to repair first | **Decide first** |
| Keep a sub-agent conversation | Medium | Protocol change (planned) | **Done** |
| Continue an agent | Low, after the above | Needs the branch | Next |
| A terminal for the shell | Low | Do not add it without a way to write | With the next row |
| A shell session and `write_stdin` | Medium | A screen must be made readable | Yes, after the limits |
| Background, while the loop runs | Medium | None. It ends with the invocation | Yes |
| Background, that a later run finds | High | State that continues after the invocation | Decide |
| A task list, `Monitor` | High | The same | Decide |
| Model control of the sandbox | — | Contrary to the SRD | **No** |

The first three were parameters on a tool that exists and machinery that
exists, and they are done. The persona is the largest gain for the model,
because it changes what delegation can express.

Everything marked "Decide" has one objection in common: it needs state between
invocations. `fyai` has one place for such state, the arena, and one rule for
it. A decision to support work that continues after the invocation is a change
to that rule, thus it belongs in the SRD before it belongs in a tool.

Note that this objection is smaller than it appears. Refer to section 3.4: only
work that a *later* invocation must find is against the rule. A session, and an
operation in the background, are permitted while the agent works.

## 6. Correction made

The branching documentation now matches the one-level delegation restriction
described in section 4.3.
