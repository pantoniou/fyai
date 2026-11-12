# Exchange scope

Status: implemented, except `sandbox_scope` as a session setting.

## Problem

Some programs start a process that detaches from them. `bitbake` starts a
server and then exits. A build tool can start a compiler daemon. A test run can
start a database. Such a process calls `setsid()` or forks twice, so it leaves
the process group of the call that started it.

A tool call cannot stop such a process with a group signal, and a tool call
must not end it at once: the next call of the same task needs the server. Today
the result depends on the host.

| Host | What happens to the detached process |
|------|--------------------------------------|
| The tool makes its own PID namespace (`USERNS` cover) | The kernel ends it when the call ends. A build server starts again on every call. |
| A bubblewrap holder (`BWRAP` cover) | It stays until the invocation ends. |
| No cover | It stays after the call, after the session, and after `fyai` exits. |

No case has a defined lifetime. This document defines one.

## Terms

- **Exchange**: the work that starts when the user sends a prompt and ends when
  the turn finishes, fails, or is interrupted. Lines that the user queues while
  the turn runs belong to the same exchange.
- **Daemon**: a process that outlives the tool call that started it. An
  ordinary process of a call ends with the call. A program that detaches, such
  as a build server, becomes a daemon.
- **Scope**: the set of processes that the tools of one exchange start. A
  process belongs to the scope even if it detaches.
- **Pin**: a reason to keep a scope after its exchange ends.

## Rules

1. One exchange has one scope. The first tool call that needs it creates it.
   No tool call creates it for a call that needs no sandbox process.
2. All tool calls of an exchange run in the same scope. A process that one call
   starts stays alive for the next call of the exchange.
3. The scope ends when the exchange ends, except while a pin holds it. When the
   scope ends, every process in it ends.
4. A call that ends, times out, or is cancelled ends its own process group.
   It does not end the scope. An interrupt of the exchange ends the scope.
5. The scope belongs to the invocation. No process survives the invocation.
   This follows the architecture rule: no daemon, no resident process, no
   hidden state.
6. A sub-agent that runs in a child process of the invocation uses the scope of
   the exchange that started it. It does not create a scope.

## Pins

A named terminal session and a monitor are objects of the run with a lifetime
of their own. Each keeps the scope from the time it starts until it ends or the
user closes it. While a pin holds the scope, the exchange that ends does not end
it. A new exchange then uses the scope that is alive.

A pin is a reference count. A session ends its pin when its program ends, not
when the user dismisses it. A detached process of a plain call can therefore
live longer than its exchange when a session is open. This is accepted: the
alternative is one scope for each session, and it costs a second holder for
little gain.

## Mechanism

`doc/sandbox-namespaces.md` describes the namespaces, the holder and the way a tool joins it.

Two mechanisms give the scope. The first is chosen when the host allows it.

### Namespace scope

The scope is a holder: a process that is the init of a PID namespace, in a user
and mount namespace that carry the cover mounts. The holder reads a pipe that
the invocation keeps open. When the invocation closes the pipe, or ends, the
holder ends, and the kernel ends every process of the PID namespace.

- A tool joins the user, mount, and PID namespaces of the holder with
  `fyai_cover_join()`. It no longer makes namespaces of its own.
- The init of the namespace reaps the orphans that detached processes leave.
- The holder is started by `unshare()` where the host allows it. Where it does
  not, the holder is the bubblewrap holder that `src/fyai_cover_linux.c` already has.
  Both give the same `holder` process identifier to `fyai_cover_join()`.
- Where neither can start, the tool makes namespaces for each call as it does
  today, and the orphan tracker below gives the lifetime.

The change from today is that the holder is made for an exchange and ends with
it, not for the invocation (`BWRAP`) or for one call (`USERNS`).

### Orphan tracker

The orphan tracker works with no namespace. It is the fallback when no cover is
possible, and the sandbox may be off.

- A tool child sets `PR_SET_CHILD_SUBREAPER`. A process that a program of the
  call detaches then becomes a child of the tool child, whatever it called. The
  child of a sub-agent call does not.
- When the call ends, or the program of a terminal session ends, the tool child
  names its live children, with the start
  time of each, in the `detached` member of its answer. The invocation opens a
  `pidfd` for each, and checks the start time, so a reused number is never
  signalled. A tool child that runs in a namespace of a scope names none: the
  pids mean nothing outside it.
- When the scope ends, the invocation reads the processes below each of them,
  sends `SIGTERM` to all, and sends `SIGKILL` after a grace time with a timer of
  the event loop. The invocation ends the rest when it ends.
- The processes reparent to the init of the host when the tool child ends, so
  the invocation has no zombie to reap.

The tracker is weaker than the namespace. The process stays visible to the user,
and a process that its parent leaves, because the parent ended, is not found by
the walk below its root. It still has a defined end. The tracker also runs when a
namespace scope is in use. It then finds nothing, because the namespace holds
every process.

### Platforms

Both mechanisms are Linux only. Other platforms keep the group signal of today.

## Lifecycle in the code

| Event | Action |
|-------|--------|
| First tool call that needs a sandbox process | Create the scope, or reuse the one that a pin holds. |
| Tool call ends, times out, or is cancelled | Signal the process group of the call. Record the orphans. |
| Exchange ends (`fyai_turn_run_destroy()`) | Release the exchange reference of the scope. |
| Last reference released | End the holder, then sweep the orphans. |
| Invocation ends (`fyai_cleanup()`) | End every scope, whatever its pins. |
| Interrupt of an exchange | End the scope at once. |
| Failure of a turn that keeps its steps | End the scope as for any other end of the exchange. |

The scope is a member of `struct fyai_ctx`, as `struct fyai_cover` is. A forked
child that does not exec drops it in `fyai_ctx_fork_disown()` and does not end
it. A child that executes `fyai` finds the holder of its ancestor through the
environment, as it does today.

## What the user and the model see

- The result of a call that leaves a process running says so, for example:
  `1 daemon still running (bitbake-server); it ends with this exchange`. The model then knows that the process is there and when it ends.
- `list` has the kind `daemons`, which names the processes that the holder of
  the scope holds and the ones that the tracker keeps, each with its pid, its
  name and the `label` that its environment carries in `FYAI_DAEMON`.
- `cancel` and `/kill` stop a daemon by its label or its pid
  (`fyai_scope_stop()`): SIGTERM through a pidfd, and SIGKILL after
  `sandbox_grace_ms`.
- The description of `exec_command` says that the processes of an exchange live
  until it ends, and that a process that must live longer belongs in a named
  terminal session.
- The user guide describes the lifetime and the setting below.

## Configuration

`sandbox_scope` selects the lifetime of the scope.

| Value | Meaning |
|-------|---------|
| `exchange` | The scope ends with the exchange. This is the default. |
| `session` | The scope ends with the interactive session. |
| `call` | The scope ends with the tool call. This is the behavior of a `USERNS` cover today. |

A key that weakens confinement is hidden from the model: `x-fyai-model:
hidden`. `session` keeps a process of a tool alive longer and so is a choice of
the user. The key is configuration only; it has no slash command yet.

## Stopping the processes

A scope ends its processes in two steps. They get SIGTERM, then
`sandbox_grace_ms` (5 seconds by default) to stop, then SIGKILL. A program that
saves its state at SIGTERM, such as a build server, has the time. This holds when
the exchange ends, and also when the invocation ends: the invocation waits for
the processes, up to the grace time, before it exits. It waits for no one when
no process remains. Without this wait the exit of the invocation ended the
namespace at once, and the processes got SIGKILL with no cleanup.

## The mounts of a profile

A profile can mount paths in the namespace of its tools: `mounts`, and the private
home (`home`, in `doc/user-guide.md`), which is a tree mounted over the real home.
The invocation makes the tree and the list of mounts before the first tool call
(`fyai_home_ensure()`), in the temporary directory unless `dir` names one, and
removes it at the end. Each tool child makes the mounts when it joins the scope, and
a mount that exists is kept. A directory of `copy` or `env` is an overlay of the
original, or with `method: copy` a copy that shares blocks with it
(`fyai_cas_clone_state()`). Each path that the sandbox grants under the home is
mounted into the tree first, so it stays visible. There is no fallback: where no
namespace is possible, the tool stops.

A mount can be `late`: it is made after the namespace has mounted its procfs, which
hides what lies below `/proc`. The `userns` exposure uses it for the read-only mask.

The removal runs outside the namespaces, after the holder has ended
(`fyai_cover_drain()`), and never inside a mount, which can show the project. The
`tool` verb joins the namespaces itself, so it forks first and the parent removes the
tree (`fyai_fsview_outer()`).

## Interaction with other parts

- **Landlock.** A detached process keeps the Landlock domain of the call that
  started it. The domain is the same for all calls of an exchange, because a
  change of profile reloads the session.
- **Cover paths.** The scope covers the paths that its first call needs. A later
  call that needs a path the scope does not cover denies it with Landlock, as a
  `BWRAP` holder does today.
- **Views.** A session in a view keeps the carve-out and has no scope of this
  kind. The view already contains the processes of the tools.
- **Timeouts.** A call time limit applies to the call. It does not apply to a
  process that the call left in the scope.
- **Turn failure.** The scope ends on every path that ends the exchange,
  including the paths that keep the finished steps of a failed turn.

## Tests

- A detached process that holds a FIFO open for writing stays alive for the next
  call of the same exchange. The test proves it with a read of the FIFO that
  must not end, and with the process itself.
- The same process is gone when the next exchange starts. A read of the FIFO
  ends at once. The test reads with a limit and no delay, and a live process is
  the control that makes the limit fail the test.
- The same two checks run with `sandbox_cover: none`, which uses the tracker.
- A named session keeps the scope while it is open, and the scope ends when the
  session closes.
- An interrupt ends the scope in the middle of an exchange.
- A host with no user namespaces skips the namespace checks, as the cover cases
  do.

## Open questions

- Whether the default should be the interactive session. This keeps a build
  server alive across prompts, which is what a long build needs. It makes the
  end of a process less predictable. The design starts with `exchange` and
  keeps `sandbox_scope: session` for the user who wants the other.
- Whether a pin by a monitor is needed, or whether a monitor can end with the
  exchange.
