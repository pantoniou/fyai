# Events and background work

## Scope

This document records how the model starts work in the background and learns
that the work ended. It lists what is missing compared with Claude Code and
Codex, and it states the open questions for durable events. It is a plan, not
a specification: only the section "Current behavior" describes code.

## Current behavior

An event is a line of text for the model. A producer queues it on the context
(`fyai_event_inject()`). The context lives for one invocation, and the queue
is not stored.

Producers:

- a named `wait` that fires: `[wait 'NAME' fired: REASON]`;
- a background sub-agent (`agent` with `background`) that ends or fails:
  `[agent 'NAME' finished]` or `[agent 'NAME' failed: CAUSE]`;
- a sub-agent or terminal session that stops for input;
- a `monitor`: `[monitor 'NAME'] LINES`, and `[monitor 'NAME' ended]`.

Delivery:

- The turn loop takes queued events before each model request, after the
  active tool calls finish. An idle session starts a turn for the event.
  Each event is echoed as a card and stored as a user message.
- `wait` with `for` holds the turn for named targets, with `mode` any or all
  and an optional time limit. Waits of one response run at the same time. A
  waiter reads an event and leaves it queued, so every waiter that waits for
  it gets it. The turn loop skips an event that a waiter has read, and the
  event leaves the queue when no waiter is active.
- A waiter does not poll. The queue, a change of state, or the earliest
  deadline wakes it.
- `cancel` ends a wait, a sub-agent, a monitor, or a session by name.
- `/clear`, `/resume`, `/checkout` and `/reset` end the background sub-agents,
  monitors and named waits and drop the queued events. Nothing reports into
  another conversation. Terminal sessions stay.

The registry (`doc/tools-yaml.md`) holds the properties of each tool.

## Gaps

### Compared with Claude Code

| Gap | Notes |
|---|---|
| Recurring and scheduled events | Waits are one-shot. A repeating timer for the life of a session is possible. A durable schedule needs the section "Durable events". |
| Background command with one completion event | `monitor` sends every line. A plain command with `background` should send one end event, and a call should read the output so far. |
| Other event sources | No WebSocket, file watch, or webhook source. |
| Push to the user | No terminal bell, desktop notification, or other channel outside the transcript. |
| Lifecycle hooks | No user commands on tool use, turn end, or event. |
| Message to a running agent | `agent_input` types on a terminal. No structured message arrives as a turn. |
| Structured events | An event is bracketed text. It has no identity, kind, or origin, and it cannot be read again. |

### Compared with Codex

| Gap | Notes |
|---|---|
| Typed event stream for a controller | `agent --rpc` and `--output json` give results. No feed of the events of a live session. |
| Notify command on turn end | A configured program should run when a turn ends or an event arrives while the user is away. |
| Poll with a budget on a process | `shell_output` has no "wait up to N ms for new output". `wait` with `for` on a session covers most of it. |

## Next steps

In this order:

1. A `notify` command and a terminal bell for an event or a turn end.
2. `exec_command` with `background`: one end event and a read of the output
   so far.
3. A repeating timer that lives with the session.
4. Event records with an identity and a kind, stored apart from typed input.

## Durable events

An event that must survive the process cannot use the queue above. No daemon
runs between invocations, so nothing can receive such an event. These
questions need an answer before any design.

### Where does a durable event live

The architecture keeps structured state in the arena. A candidate is an inbox
for each branch in the branch store. Any process posts a record with a lost-CAS
retry, as a publish does. A session reads the inbox when it starts, and a live
session finds new records when the arena generation changes. This needs
checks of the paths that build, decode, validate, truncate the reflog, and
merge CAS conflicts.

### Who posts

- A scheduler (cron, a systemd timer) that runs `fyai event post` or starts
  an invocation with `--branch`. The second works today with no new code.
- A hook or a notify command that posts when a job ends.
- An external system, such as a webhook receiver. This needs a program that
  runs all the time, so fyai does not provide it. It can call the post verb.

### Rules to decide

- **Identity and delivery.** Give each record an identity so that a retry does
  not deliver it twice. Decide when a record is consumed: at delivery, or when
  the turn that holds it is published.
- **Scope.** A record names one branch. Decide what a record does when that
  branch moved on: `/clear`, `/reset`, or a merge.
- **Order.** A conversation is append-only. Decide where an event turn sits in
  a merge or a rebase, as a `command` turn does now.
- **Trust.** The text of a durable event can come from a network. It is data,
  never an instruction. The record should keep its origin, and the model
  should see it marked as external. No event grants a permission.
- **Bounds.** Limit the size and the number of records in an inbox. Garbage
  collection limits the chains, and an inbox must follow the same rule.
- **Time.** A scheduled event has a due time. Decide whether a missed one is
  delivered late, once, or dropped.
- **Cancel.** `cancel` must reach a durable event by name, from any process.
- **Interaction with the live queue.** One path should serve a live and a
  durable event, so a waiter, a card, and the stored turn behave the same.

### Not in scope

- A resident process that watches the arena.
- Delivery of events to a session that is not running.
