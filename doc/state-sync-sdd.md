# State synchronization between a worker and its clients

Status: design, with the first phases in progress. Date: 2026-11-01.

This document says how the state that a worker holds reaches a client, so that
the renderer of the client draws what a local session draws. It extends
`doc/remote-sessions-sdd.md` (sections 6.1.4 and 7.4) and
`doc/ui-model.md`.

## 1. The problem

A worker is an invocation. It holds state in several places. A client shows
that state and must give the user what a local session gives: the screen, the
completion of a branch name, the branch browser, the resume picker, `/history`
and a scroll back through the whole conversation.

The first design sent each answer: a rendered table, the last 64 turns, a
stream of draw frames. Each feature that needs a new part of the state needs a
new request. This does not scale. The client must instead hold the state that
the local code reads, and the protocol must keep that state up to date at a
small cost.

Thus the problem is state transfer: keep a replica on the client up to date
from the worker, with these limits:

- the cost of an update depends on what changed, not on the size of the state;
- the worker keeps a bounded amount of data for each client, however long the
  client stays;
- a client that was away, or is new, reaches the current state;
- the canonical state stays in the arena of the worker. The replica is a
  copy, never a second source.

## 2. The kinds of state

Each depot of state is one of four kinds. The kind decides how it is
synchronized.

| Kind | Examples | Identity of "what the client has" | Update |
| --- | --- | --- | --- |
| Immutable objects in a graph | stored turns and their display documents, blobs, a stored configuration | content identity (section 3) | negotiation of the objects that the client lacks |
| Versioned values | the branch table, the configuration layers, the `state`, `session`, `panels` and `questions` stores | a generation number | the whole value, when the generation differs |
| Append-only logs | the `log` store, the live document, the source of a band | an offset in an epoch | the bytes after the offset; a snapshot when the offset is gone |
| Client state | zoom, scroll, layout, typed text, **terminals** | n/a | not synchronized |

A terminal belongs to the client that shows it. The program runs in the worker,
but its screen, its size and its keys are those of one connection, not state of
the session. It uses a channel of its own and is outside this protocol. A
second client that watches the same program has its own terminal and its own
size.

## 3. Identity of a turn

A turn is an immutable generic mapping that holds the turn before it in
`previous`. The arena deduplicates generics, so two turns with the same
content and the same history are the same word, and the word does not change
while the arena stays as it is. The arena is address-stable between
processes (`CLAUDE.md`, architecture rules). Thus the word of a turn is its
identity: no digest is needed.

The **turn id** on the wire is that word, as an opaque 64-bit number. A client
does not read it. It keeps the ids, compares them and sends them back, and the
worker finds the turn again from its id.

The word has a meaning only for the arena that gave it. Garbage collection
relocates objects, and the arena of another worker is another namespace. Thus
every id belongs to an **epoch** (section 4.1), and a client that sees another
epoch drops every id it holds.

The worker never trusts an id from a client. It looks the id up among the turns
of the chain it walks, and an id that it does not find in that chain is "not
held": it does not follow it as an arena address. This keeps a damaged or
hostile client from reaching an object outside the conversation.

The provider stream stays at the worker, as it did before: it holds request
identifiers and time stamps that the client does not need, and it can hold
data that must not leave. A turn goes to the client without it.

## 4. The protocol

### 4.1 Epoch and sequence

A worker has an **epoch**: a random 64-bit number chosen when the worker
starts, and again when it reopens its arena, which is when an arena
collection can have moved its objects. Every frame of the model stream carries the epoch of the worker, and
each store frame carries the sequence number of its store (it exists now).
A sequence number has a meaning only inside its epoch. A worker that restarts
has a new epoch, and a client that sees a new epoch drops its replica and
starts again.

### 4.2 Snapshot

The policy is snapshot first. When a client initializes its connection, the
worker sends the state as it is, whole, then each change:

1. the open texts, whole;
2. the root of each store;
3. a `fyai synced {epoch, seq}` frame, which ends the snapshot.

A client that reconnects sends the same initialize and gets the same
snapshot. The versioned values and the logs are small, so this costs little,
and the worker does not need to remember what the client held. The client
treats everything before `synced` as one atomic state: it does not draw a
partial snapshot.

### 4.3 The negotiation of turns

The conversation is the one large depot. Its turns are immutable, so a client
keeps those it has. `fyai_sync` asks for the turns the client lacks:

    request:  fyai_sync {have: [ID, ...], depth: N, before: ID?}
    answer:   {epoch, head: ID, turns: [{id, turn}, ...], more: bool}

`have` lists turn ids (section 3) that the client holds: its head first, then ancestors at
exponentially growing distance (1, 2, 4, 8, ... back). The worker walks from
its head to the first id in `have` and sends the turns after that id, oldest
first, at most `depth` of them. If the client holds none of them, the worker
sends the newest `depth` turns and `more` is true.

`before: ID` pages backward: it asks for `depth` turns that end just before
`ID`. This is the scroll back past the turns the client holds. It has no other
meaning, and it needs no sequence number.

A head that moved by `/reset`, `/rewind`, `/checkout`, a rebase or a compaction
has no turn of the old line after the common ancestor, so the negotiation finds
that ancestor and sends the new line. A sequence number per exchange cannot say
this: after such a move the numbers of the two lines overlap.

The client puts the turns in its replica (section 5). A turn the client does
not hold, below the oldest it holds, is a **paged-out** turn: a known id with no
object. The code that walks turns must end a walk there without calling it
corruption (section 5.3).

### 4.4 The acknowledgement and the bound on the server

The client acknowledges each sync answer and the end of each snapshot with
`fyai_ack {epoch, seq, head}`. The worker keeps for each client:

- the epoch and the last sequence acknowledged;
- the head id of the last acknowledged sync.

This is all of it, so the state per client is constant. The worker keeps no
copy of what the client holds, because:

- the versioned values go whole, so no old value is needed;
- the logs have a bounded ring in the worker for each log; and
- the turns are in the arena already.

The one cost is that the arena object of the acknowledged head must stay
reachable while the client may ask for a negotiation that starts there. The
worker therefore **pins** the oldest acknowledged head of the clients that are
attached. Garbage collection keeps the chain below a pinned head. A client that
does not send a request for the idle time (`remote/client_idle_s`) is dropped
from the pins, and its next sync is negotiated again from its `have` list, which
needs no pin. The growth is thus limited by the number of attached clients, and
a client that stalls costs a negotiation and no memory.

Each connection has a bounded queue of frames. A client that does not read
fast enough overflows it. The worker then drops the queued deltas, marks the
client, and sends a new snapshot when the queue drains. Nothing grows without
bound, and a slow client costs one snapshot.

## 5. The replica of the client

### 5.1 What it is

The client has a private arena builder, `replica`. It holds the objects that
the worker sent: the turns, the branch table, and the stored configuration the
client needs. It is not the arena of the worker and it is not shared with any
other process. It is never written back, and it is dropped with the
connection. The garbage collection of the arena does not apply: the builder
lives and ends with the view.

### 5.2 What reads it

The code that reads a conversation, a branch or a configuration from `ctx`
reads the replica when the process is a client:

- the transcript view and the recap, through the stack of turns;
- `/history`, `/help` and the completion of a branch name;
- the branch browser and the resume picker.

Each of them runs unmodified. The client sets up a context whose branch,
`last_message` and branch table are the replica's. This is what "transfer the
state so the local renderer works" means: the client does not ask for an answer,
it holds the data.

### 5.3 A paged-out turn

`fyai_turn_foreach()` and `fyai_turn_stack_init()` treat a missing `previous` as
the end of the chain. A paged-out turn has an id and no object, so a walk of
the replica ends at the oldest turn it holds. The caller that needs the older
turns asks for them with `before`. A chain whose objects cannot be read is
still an error at the worker, where every object is present.

### 5.4 Live and committed state

A turn that runs has no turn id. Its content is in the append-only texts and
in the stores (section 2), not in the graph of turns. The client draws it from
them, and it does not ask for it with `fyai_sync`.

When the turn commits, the worker does these steps in this order:

1. It closes the text of the live document (`closed: true`). This is the last
   delta of that text.
2. It publishes the new head. The `head` member of the `session` store changes.
3. The client sees the head move and sends `fyai_sync`. The answer holds the
   committed turns, and they enter the replica.
4. The stored exchange replaces the live rows, as a published turn replaces
   them in a local session.

A client that attaches in the middle of a turn gets the open texts whole in its
snapshot (section 4.2), then the stores, then the deltas. It needs no
negotiation for the live content. A worker that stops in the middle of a turn
loses the content that was not committed, as a local session does. Its epoch
changes, and the client drops its live texts and its replica.

A delta names its text and its offset. A client that finds an offset that is
not the length it holds must ask for the whole text again. This check is part
of phase 1 (section 7).

### 5.5 Memory bounds

A process that runs for a long time must not grow with the time it runs. The
state that it keeps grows only with the state itself: the turns of the
conversation, the bounded rings of the logs, and the bounded queue of a client.
The following rules keep this true:

- A builder that a process resets to use again is made with
  `fy_generic_builder_create(NULL)`. The builder owns its allocator, so a reset
  gives its memory back. A builder on a borrowed allocator needs
  `FYGBCF_CREATE_TAG` for this. The frames of a connection, the stores of the
  UI model, the diagnostics and the inbox of a client use such a builder. The
  libfyaml that fyai needs resets the memory of an owned allocator.
- The client keeps in `turns` only the turns of its replica. When it drops
  turns, the builder still holds them. The client makes the builder again from
  the turns that remain when the dropped turns outnumber them (more than 64).
  The cost of this is constant for each dropped turn.
- `fyai_host_malloc_tune()` fixes the limit above which a block is mapped. A C
  library that adapts the limit keeps each later large block, such as the body
  of a request to the provider, in its heap. The resident set then follows the
  peak and not the use.

The benches `tests/bench/remote_soak.sh` and `tests/bench/remote_client_soak.sh`
measure this. The first runs one worker through the SDK protocol and reports the
resident set of the server and the worker, the CPU time of each turn and the
time of `fyai_sync`. The second types the turns into the full-screen client and
reports the resident set of the client, the server and the worker. Set
`SOAK_CHURN=1` to move the head back before each question: the conversation
then keeps its length and the memory must stay flat.

The measured result, with a one-kilobyte reply for each turn:

| Process | Growth with the turns | Remark |
| --- | --- | --- |
| server | none | 3.2 MB from the first turn to the 1000th |
| worker, anonymous | none | the arena pages that the file maps grow with the conversation, about 4 KB for each turn |
| client | about 4.6 KB for each turn | the replica holds the turns and the display source; this stops only when the client keeps a window of the turns (open question) |
| client, head moves | none | the conversation keeps its length |

The CPU time of a turn of the worker used to grow with the conversation:
each request made the text of the whole history again, and the emitter and
the copy of the turns into a builder cost more than all the rest. The request
cache (`doc/provider-grammars.md`) keeps the text of the turns that the last
request sent. The CPU time of a turn is then 3 to 5 ms from the first turns to
the 400th, and the worker keeps the last body, which is about the size of the
history as text. The request still carries the whole history to the provider.
The time of a sync depends on the number of turns it sends (about 0.15 ms for
each turn) and not on the length of the conversation. The server uses about
1.5 ms of each turn.

## 6. What does not move

- The provider stream, the credentials and the configuration that the worker
  keeps for the provider stay at the worker.
- Files of the project stay at the worker. Completion of a path and `@PATH`
  read the file system of the client, or the worker through the protocol, as
  `doc/remote-sessions-sdd.md` decides.
- Terminals, as section 2 says.

## 7. Phases

1. **Negotiation.** `fyai_sync` with the epoch, the client replica and the
   replacement of `fyai_history`. Done, for the turns as a client shows them
   (display records, user message, calls and outputs). Open: `before` for
   paging back, the whole turn for the readers of the branch browser,
   `fyai_ack` and `synced`.
2. **Branch data.** The branch table and the stored configuration as versioned
   values, and the client code that reads them (completion, browser, picker).
3. **Bounds.** The pin of the oldest acknowledged head, the idle time, the
   bounded queue and the snapshot after an overflow.
4. **Terminals** on a channel of their own, with a screen snapshot when a client
   attaches.
5. **Objects as state of the server.** Attachments and the clipboard go from the
   client to the worker by digest, and become state of the worker.

## 8. Open questions

- The size of a turn with an image. The first version sends the image bytes
  with the turn. A later version sends a stub with the digest and loads the
  bytes when the view needs them (phase 5).
- A window for the replica. The client holds every turn that it synchronized,
  so its memory grows with the conversation (section 5.5). A window of the
  newest turns, with `before` (section 4.3) for a scroll back, bounds it. The
  transcript view must then ask for the turns that it lacks.
- The ring size of the logs, and the idle time. Both are configuration, with a
  default that the first measurements will set.
- Several clients on one session: each has its cursor and its pin. The turn ids
  are the same for all of them, so a turn is serialized once.
