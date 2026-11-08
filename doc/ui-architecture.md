# The architecture of the display and of remote sessions

This document gives the parts of the display, how data moves between them, and
how a remote client shows a session that runs in another process. The
interfaces are in `doc/ui-model.md` and `doc/remote-sessions-sdd.md`.

## Parts

The display has three layers:

1. The UI model (`src/fyai_uimodel.[ch]`). It holds the state that a renderer
   reads, as stores.
2. The producers. They write the stores. Most of them write at the input of
   the display, not in the engine.
3. The renderer (`src/fyai_uidraw.c`). It draws the stores with the sink of
   the process. A local session and a remote client use the same renderer;
   the client draws a copy of the stores of a worker.

## The UI model

The model has six stores. Each store is one generic root with a sequence
number.

| Store        | Holds                                                         |
| ------------ | ------------------------------------------------------------- |
| `session`    | the values of the header and the state of the turn            |
| `panels`     | the rows of the todo list                                     |
| `questions`  | the open sets of questions to the user                        |
| `log`        | the output of each command                                    |
| `work`       | the live work (shells, sub-agents, terminals) and the bands   |
| `transcript` | the live document of the conversation                         |

These rules apply to every store:

- A store holds data and Markdown source. It holds no SGR, no rows and no
  value that depends on a width or a theme. Each renderer formats, wraps and
  colours what it reads.
- View state is not in the model. Focus, zoom, scroll positions, the layout,
  the theme, the prompt text and the cursor of a question belong to each
  renderer.
- A write replaces the root, or one member of the root. A root equal to the
  current root changes nothing, so the sequence number moves only on a change.
- Content that grows is an append-only text. A store names a text by its id
  and its length. A consumer of a text gets each append as a delta: the id,
  the offset and the bytes. A store root never holds the bytes of a text.

### Memory

A writer builds a root between `fyai_uimodel_begin()` and
`fyai_uimodel_end()`, in a scratch builder. Only the outermost begin resets
that builder. Each store has two builders and uses them in turn, so a store
holds two roots at most, whatever the number of writes. A JSON-RPC connection
encodes each frame in a builder of its own, which it resets for each frame.

### Consumers

A renderer that draws in frames compares the sequence number of a store with
the number that it drew, and reads the root again when the number changed. A
consumer that must see each change, such as the encoder of a remote worker,
subscribes to the stores and to the texts. `fyai_uimodel_consumed()` tells a
writer that a consumer reads the model. A writer that only a display enables,
such as the header, then writes also in a worker that has no display.

## Producers

The engine does not write most stores itself. Each store is written where its
data enters the display, and the renderer draws the display from the store:

```
engine --> fyai_sink_markdown(), fyai_sink_write() --> log store ---+
       --> sink band open, paint and end ------------> work store --+
       --> fenced stream push -----------------------> band text ---+--> renderer
       --> fyai_output_begin(), append(), end() -----> transcript --+      |
       --> bytes of a session or agent terminal -----> terminal text       v
                                                                    sink backend
```

The model gives the renderer each change when the change is made, so the
renderer draws in the order of the writes. It calls the sink with `drawing`
set: its bands are bands of the display, and the store does not take them
again.

- The log store records what the sink gets during the span of a command,
  from `fyai_ui_pane_begin()` to `fyai_ui_pane_end()`: Markdown source, bytes
  in their final form, and each diagnostic that the drain reports. One entry
  is made for each command that gave output. The store keeps the last 200
  entries.
- A band of the work store has a `format` that tells a renderer how to draw
  it again:
  - `markdown` and `code` are source. A renderer renders them at its width.
  - `terminal` is the bytes a program wrote on a pipe. A renderer replays them
    through a terminal model, because they can move the cursor.
  - `tool-view` is the Markdown of the view of a tool call, with its code
    blocks and the preview rows of each.
  - `rows` is a body that its producer painted in its final form.

  A band that ends stays in the store for one write with its `state`, so a
  consumer ends its own band in the same way.
- A terminal text starts with a snapshot of the screen: the bytes that clear
  it, draw its rows and place the cursor. The program bytes follow, as the
  cast protocol records output. When the text grows past 256 KiB, a new text
  starts with a new snapshot.
- The transcript store holds the source of the live document. Its `recorded`
  ranges are the source that a band presented already, such as a tool
  exchange. A renderer that draws the bands does not draw these ranges again.

## Parallel construction

A part of the screen moves to the model in steps. While it moves, the legacy
path that made it stays as its oracle. The renderer shows what it made from
the model, and `fyai_uimodel_check()` compares that with what the legacy path
made. A difference goes to the trace and to the file that
`$FYAI_UI_MODEL_CHECK` names. The test harness fails a case when that file is
not empty, so each PTY case also checks the model against the legacy path.

The legacy path of a part that the renderer draws presents nothing:

- `src/fyai_output.c` records the operations it gave the sink, and the
  renderer compares them with its own when the document ends.
- The span of a command still captures what the sink presents. The capture is
  the oracle of the rows that the renderer makes from the entry of the log.
- The band of a producer is a shadow. It keeps its last paint, and it renders
  at the width of the band that the renderer draws. The renderer compares the
  two when the band ends.

## Remote sessions

```
 fyai remote connect          fyai serve (front)            worker
 +--------------------+  Unix  +-------------------+  fds   +----------------------+
 | model is a mirror  |<------>| admission, token, |<------>| claude-sdk           |
 | terminal UI        | socket | one worker for    | 3/4/5  |   --remote-worker    |
 | src/fyai_remote_   |  SDK + | each branch       |  SDK + | engine and arena     |
 |   ui.c             |  fyai  |                   |  fyai_ | bands with no display|
 |                    | frames |                   | worker | encoder of the model |
 +--------------------+        +-------------------+        +----------------------+
```

### Front

`src/fyai_remote_server.c` listens on a Unix socket. It admits a client by its token
or by the uid of the peer. It starts one worker for each branch, or attaches
the client to the worker that runs. It forwards the frames and does not read
the model. A worker reports its branch in a `fyai_worker` frame, so a
reconnect follows a checkout. A session with no name ends with its client.

### Worker

The worker is the engine with no display. A client that sends `fyai: {model:
1}` in its initialize gets each open text whole, then the root of each store.
Then the worker sends each change:

- `{type: fyai, subtype: store, store, seq, root}` replaces the root of a
  store.
- `{type: fyai, subtype: text, id, offset, data}` appends to a text, and
  `closed: true` ends it.

The texts go first, so a store never names a text that the client does not
have. The worker accepts these requests from a client:

| Request                | Action                                                 |
| ---------------------- | ------------------------------------------------------ |
| `fyai_answer`          | settles a set of questions                             |
| `fyai_command`         | runs a slash command, queued as a typed command        |
| `fyai_terminal_input`  | writes keys to the program of a terminal tile          |
| `fyai_bang`            | starts a command of the user in a terminal session     |
| `fyai_sync`            | returns the stored turns that the client lacks         |

The worker runs with Markdown on, so the client gets Markdown source and not
rendered text.

### Client

The client is a context with a terminal UI. Its model is a mirror: the frames
of the worker write it, and the writers of the client write nothing. No
oracle runs in a mirror, because the oracle of the data is in the worker.

The client draws the header, the turn state, the todo panel, the questions
and the counts of the work from the mirror, as a local session draws them. The
renderer draws the bands, the live document and the log of the mirror, as it
draws them in a local session. The client shows the terminals of the worker in
tiles of its own. When the `head` member of the session store changes, the
client asks for the last stored turns again, and the stored turns replace the
live rows, as a published turn replaces them in a local session. A typed line goes to the worker. A command with
`scope: view`, or a setting of a `display/` key, runs in the client. The keys
of a mirrored tile go to the worker through the hooks of
`fyai_uimodel_mirror_tiles()`.

### Commands

A command that a client runs with `--remote` does not use the UI model. It has
no display, so it has no bands and no live tiles: its output is what the sink
gets. The worker runs with the frame sink (`src/fyai_sink_frames.c`), which
sends each sink operation to the client, and the client gives each to its own
sink. The client takes the `display` settings of the worker first, so it
renders the output as a local run of the command does, at its own width. The
standard error of the worker and its exit status go to the client too
(`doc/remote-sessions-sdd.md`, section 6.1.2).

## Limits

1. The producers still render what the oracles compare: a shadow band renders
   its rows, and the span of a command captures its output. When the oracles
   go, a producer writes only the model.
2. A change of one member sends the whole root of its store. A change of one
   band thus sends all the bands of the work store. The texts keep growing
   content small, but many bands send many roots.
3. The terminal sessions of a local session draw their tiles from their own
   views. The client draws them from the terminal texts.
4. The stored exchanges are not in the model. The client holds a replica of
   them and asks for the turns it lacks with `fyai_sync` each time the head
   moves (`doc/state-sync-sdd.md`).
5. The units of the flow are not in the model. A mirror states the prose of
   an answer when it starts.
6. A request of a client has a subtype and a generic, with no schema.
7. A worker has one client. A client that attaches again gets a new snapshot.
   It does not use the sequence numbers to find a frame that it did not get.
8. A remote command cannot ask the user: a question or an approval takes the
   answer that a run without a terminal takes. A command that needs the
   terminal, such as `sandbox enter` or an editor, does not run remotely.
