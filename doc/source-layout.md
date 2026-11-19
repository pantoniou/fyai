# Source layout

This document gives the owner of each source file and of each shared component. `CLAUDE.md` gives the directories and the rules.

- `src/core/main.c`: global option parsing and command dispatch.
- `src/cmd/commands.h`: what an invocation runs, and the argument structures
  that backends read.
- `data/commands.yaml`: the definition of every verb and slash command.
- `src/cmd/fyai_cmd.c`: the command registry, parser, dispatch, and presentation.
- `src/core/fyai_image.h`, `tools/fyai_mkimage.c`: the prebuilt images of the
  command definitions and the catalogue (`doc/embedded-images.md`). Load an
  embedded document that is large with `fyai_image_load()` and keep the parse
  of its text as the fallback.
- `src/cmd/fyai_cmd_complete.c`, `src/cmd/fyai_cmd_help.c`: completion and help from
  the definitions.
- `src/cmd/fyai_cmd_*.c`: the handlers of the commands, one file for each group.
- `src/core/fyai_app.c`: `struct fyai_app`, the root that owns the components that
  the whole program shares, in dependency order (`doc/context-components.md`).
  `main()` declares it and everything else borrows from it. Today it holds the
  diagnostics, the event loop, the arena, the transient builder, the
  credential refresh, the model connection, the transport client, the MCP
  client, the waits, the questions, the tool scope, the terminal, the skill
  files, the tool specification, the branch cursor, the usage totals, the turn
  state, the tool jobs, the sub-agent identity, the output, the UI model, the screen and the
  configuration. The context of the invocation is a member of the app
  (`app->ctx`).
- A component finds its context through an accessor, not through a field that
  the callers read. A component that is a member of `struct fyai_app` holds no
  pointer to the context: its accessor uses `container_of()` on the app
  (`fyai_cred_ctx()`). An object of which many stand on one context (a
  connection, a request, a tile) keeps a pointer of its own and gives it
  through an accessor of its own, `NAME_ctx()` for `struct NAME`, defined
  next to the struct. Set the field where the object is made and read it
  nowhere else; `tests/ctx-accessor.sh` fails on a direct read.
- `src/store/fyai_arena.c`: `struct fyai_arena`, the durable and working builders of
  the canonical state. `ctx->arena` points at the one in the app; read
  `ctx->arena->gb` where you need the working builder.
- `src/store/fyai_transient.c`: `struct fyai_transient`, the builder for the values of
  one operation. `ctx->transient` points at the one in the app.
- `src/provider/fyai_link.h`: `struct fyai_link`, the model connection: the curl handle,
  its headers, the outcome of the last transfer, the curl multi state and the
  request cache. `ctx->link` points at the one in the app.
- `src/transport/fyai_transport_state.h`: `struct fyai_transport_state`, the client of the
  credential transport and its control channel. `ctx->transport` points at the
  one in the app.
- `src/mcp/fyai_mcp_state.h`: `struct fyai_mcp`, the connections to the MCP servers
  and their tool catalogue. `ctx->mcp` points at the one in the app.
- `src/loop/fyai_waitq.h`, `src/tools/fyai_askq.h`: `struct fyai_waitq` (named waits, the
  waiters that hold a call, the event queue) and `struct fyai_askq` (the
  background questions and the questions store). `ctx->waitq` and `ctx->askq`
  point at the ones in the app.
- `src/sandbox/fyai_scope.h`, `src/front/fyai_tty.h`, `src/tools/fyai_skills.h`,
  `src/tools/fyai_toolspec.h`: the scope of the tool processes, the real terminal, the
  skill files, and the tool specification. `ctx->scope`, `ctx->tty`,
  `ctx->skills` and `ctx->toolspec` point at the ones in the app.
- `src/store/fyai_branch_state.h`, `src/core/fyai_usage.h`: `struct fyai_branch_state`, the
  branch cursor, and `struct fyai_usage`, the totals of the run. `ctx->bstate`
  and `ctx->usage` point at the ones in the app. A detached view context keeps
  a copy of the cursor (`struct fyai_view_own`).
- `src/core/fyai_turnstate.h`: `struct fyai_turnstate`, what a running turn carries:
  the last message, the observers, the tool gate, the stop reason and the
  attachments that wait. `ctx->turn` points at the one in the app.
- `src/tools/fyai_jobs.h`, `src/tools/fyai_agent_state.h`: `struct fyai_jobs` (tool jobs,
  terminal sessions, monitors, background runs) and `struct fyai_agent_state`
  (the registry and the identity of a sub-agent). `ctx->jobs` and `ctx->astate`
  point at the ones in the app.
- `src/front/fyai_out.h`, `fyai_uistores.h`, `fyai_screen.h`: the display in
  three components. `struct fyai_out` holds the sink and the open transcript
  document, which is all that the engine says to the user. `struct fyai_uistores`
  holds the UI model and its stores, the bands, the live document, the span of a
  command and the renderer. `struct fyai_screen` holds the terminal UI, the work
  pane, the browser, the command in flight and the editor and catalogue update
  requests. `ctx->out`, `ctx->uistores` and `ctx->screen` point at the ones in the
  app. Only `out` is for the engine to use; the other two are the part that the
  TUI split moves.
- `src/auth/fyai_cred.c`: `struct fyai_cred`, the shared refresh of the login and
  the secrets in memory. `ctx->cred` points at the one in the app; it stands
  on the loop of the app for its timer.
- `src/loop/fyai_loop.h`: `struct fyai_loop`, the event loop component: the loop,
  its timers and sources and their pools, the signals, and the interrupt state.
  `ctx->loop` points at the one in the app. `src/loop/fyai_event*.c` implement it.
- `src/core/fyai.c`: engine orchestration.
- `src/front/fyai_sink.c`: the one rendering component and its backends.
- `src/front/fyai_sink_frames.c`: the sink of a command that a client runs with
  `--remote TARGET`. It sends each sink operation as a frame, and the client presents
  the frame with its own sink.
- `src/remote/fyai_remote_server.c`: `fyai serve`, its project servers and workers
  (`doc/remote-sessions-sdd.md`, section 6.1.2).
- `src/remote/fyai_remote.c`: the client of `--remote` and of `remote connect`, and
  what it shares with the server (`src/remote/fyai_remote_int.h`). It reaches a
  server by a Unix socket, by `ssh://` through `remote bridge`, and by
  `http://` and `https://` through `remote relay`.
- `src/remote/fyai_remote_http.c`: the HTTP front of `fyai serve --listen`: the
  bearer-token check, the route of the session, and the bridge of a WebSocket
  to a socket of lines, on `fyai_httpd`. `src/remote/fyai_remote_relay.c` is the
  client end of the relay, on `fyai_ws_client`.
- `src/core/fyai_buf.c`: the growing byte buffer, `struct fyai_buf`. It holds text
  or a queue that is read from the front with `fyai_buf_consume()`. Use it
  for every buffer of bytes and do not write another.
- `src/net/fyai_net.c`: sockets for the network code: a connection that is TCP or
  TLS over TCP, and the listen, connect (blocking, or started without
  blocking) and URL helpers. The URL parse uses the URL API of libcurl. It
  owns no event source.
- `src/net/fyai_httpd.c`: the HTTP/1.1 server on the event loop: listeners with
  optional TLS, the request parser, request bodies, kept connections,
  answers (its own, or bytes that the handler writes), and the server end of a
  WebSocket. It knows no route, no credential and no protocol over the
  WebSocket; a handler gets each request and message
  (`src/net/fyai_httpd.h`). Use it for every listener of HTTP in this program,
  the mock provider included, and do not write a second one.
- `src/net/fyai_ws.c`: RFC 6455 as pure functions: the codec, the frame encoder and
  the text of the handshake. `src/net/fyai_ws_client.c` is the client end, in two
  forms: a blocking connection (`fyai_ws_client`) for a standalone command, and
  a connection on the event loop (`fyai_wsc`) that never blocks. Each connects,
  makes the TLS handshake, checks the pin, upgrades, sends masked frames and
  gets whole messages. Use `fyai_wsc` in code that runs on the loop.
- `src/remote/fyai_remote_tls.c`: the self-signed key of a server, the context of a
  TLS listener, and the pin and chain checks of a client of one.
- `src/front/fyai_uidraw.c`: the renderer of the UI model. It draws the live
  document, the log and the bands from the stores with the sink
  (`doc/ui-model.md`).
- `src/front/fyai_flow.c`: the output separation manager.
- `src/front/fyai_output.c`: transcript document source and fragments.
- `src/front/fyai_session.c`: interactive input and slash commands.
- `src/tools/fyai_agent.c`: sub-agent execution and agent RPC.
- `src/tools/fyai_tools.c`: the dispatch of tool calls, the tool implementations
  and the glue. `src/tools/fyai_tools_int.h` is the interface that the tool files
  share; keep a function there only when another tool file calls it.
- `src/tools/fyai_pending.c`: the background work of a run in the store of the
  branch (`durable`): the table of waits, monitors, commands and sub-agents, and its
  takeover after a loss.
- `src/tools/fyai_tool_job.c`: the lifecycle of a tool job and of a job group:
  spawn, submit, gate, collect and the exchange scope.
- `src/tools/fyai_tool_child.c`: the tool child process: its fork and exec, its
  JSON-RPC serve loop and its dispatch.
- `src/sandbox/fyai_shell_sandbox.c`: the confinement policy of a shell command.
- `src/tools/fyai_shell_session.c`: shell sessions and their tiles, bang commands,
  user programs and mirrors.
- `src/front/fyai_tools_focus.c`: focus, zoom and the keys of tiles and inline
  blocks, and the control of a session by name.
- `src/front/fyai_agent_view.c`: the screen of a sub-agent, its inline terminal and
  the btw panels.
- `src/remote/fyai_claude_sdk.c`: the Claude Agent SDK protocol
  (`doc/claude-sdk-compat.md`).
- `src/loop/fyai_event*.c`: the portable event loop and signal handling.
- `src/loop/fyai_event_dump.c`: SIGUSR2 event-loop diagnostics.
- `src/core/fyai_render.c`: generic-to-Markdown table rendering.
- `src/core/fyai_diag.c`: collected diagnostics.
- `src/core/utils.c`: HTTP buffers, file helpers, and generic serialization.
- `src/tools/fyai_shell_capture.c`: shell capture on the event loop, and the child
  start: its status channel, `fyai_child_exec_prepare()` and
  `fyai_exec_shell_command()`.
- `src/front/fyai_editor.c`: the external editor child, and the editor in a tile of
  the work pane.
- `src/front/fyai_workpane.c`: the one owner of work-pane geometry, focus, and zoom.
- `src/front/fyai_chrome.c`: the one owner of tile chrome: which controls and scroll
  bar a tile carries, and its head.
- `src/sandbox/fyai_fsview.c`: the portable part of the filesystem views and the
  dispatch to the backend of the platform (`src/sandbox/fyai_fsview_backend.h`).
  `src/sandbox/fyai_fsview_linux.c` is OverlayFS and the namespaces,
  `src/sandbox/fyai_fsview_macos.c` is the native cloned directory, and
  `src/sandbox/fyai_fsview_none.c` has no views. Put platform code in a backend.
  The view commands (`src/cmd/fyai_cmd_fsview.c`) use its pipe, socket, boot id,
  sync, prepare and confine members and test no platform.
- `src/store/fyai_project_capture.c`: the portable part of the project capture. Its
  file system primitives are in a backend
  (`src/store/fyai_project_capture_backend.h`): `src/store/fyai_project_capture_linux.c`,
  `src/store/fyai_project_capture_macos.c` and `src/store/fyai_project_capture_none.c`.
- `src/store/fyai_cas.c`: the portable part of the blob store. Its file system
  primitives (sync, kernel copy, clone) are in a backend
  (`src/store/fyai_cas_backend.h`): `src/store/fyai_cas_linux.c`, `src/store/fyai_cas_macos.c`
  and `src/store/fyai_cas_none.c`.
- `src/sandbox/fyai_sandbox.c`: the portable part of tool confinement and the dispatch
  to the backend of the platform (`src/sandbox/fyai_sandbox_backend.h`).
  `src/sandbox/fyai_sandbox_linux.c` is Landlock and the seccomp UDP filter,
  `src/sandbox/fyai_sandbox_macos.c` is Seatbelt, and `src/sandbox/fyai_sandbox_none.c`
  confines nothing. Put platform code in a backend, not in the core.
- `src/host/fyai_host.c`: the portable part of the operating system primitives
  (descriptors, processes, peer identity, entropy, browser launch) and the
  dispatch to the backend of the platform (`src/host/fyai_host_backend.h`).
  `src/host/fyai_host_linux.c`, `src/host/fyai_host_macos.c` and `src/host/fyai_host_none.c`
  hold the platform code. Test a capability (`fyai_host_memfd_available()`),
  never the name of a platform. `src/sandbox/fyai_orphan_linux.c` and
  `src/sandbox/fyai_cover_linux.c` are the Linux code of the scope and the cover; the
  `_none.c` files hold their stubs.
- `src/provider/fyai_grammar.c`: the dispatch to the provider API grammars
  (`src/provider/fyai_grammar.h`). `src/provider/fyai_grammar_responses.c`,
  `src/provider/fyai_grammar_chat.c` and `src/provider/fyai_grammar_messages.c` each build the
  requests and parse the replies of one grammar (`doc/provider-grammars.md`).
  `src/provider/fyai_provider.c` holds what the grammars share and the readers of the
  canonical response. Put grammar code in a grammar file.
- `src/auth/fyai_secret.c`: the `secret` verb and the machine-local secrets.
  `src/auth/fyai_secret_store.h` is the interface of the stores: the kernel keyring
  (`fyai_secret_keyctl.c`), the macOS keychain (`fyai_secret_keychain.c`), the
  Secret Service (`fyai_secret_libsecret.c`) and none. The login uses
  `fyai_secret_store_keyring()`.
- `src/auth/fyai_oauth.c`: provider-independent OAuth browser flow.
- `src/auth/fyai_cred.c`: the credential sources (`env:`, `secret:`, `mem:`,
  `oauth:chatgpt`) and the one refresh of the login for each process: waiters,
  `prepare` before a request, `reject` after a 401 and the scheduled refresh
  (`doc/credential-sources-plan.md`). The agent, the model discovery and the
  transport all use it; do not refresh a login anywhere else.
- `src/provider/fyai_xfer.c`: a model request over curl or the transport. It builds the
  headers for each send and holds back a 401 that a renewal can answer.
- `src/auth/fyai_mcp_oauth.c`: the refresh request of an MCP OAuth server.
- `src/net/fyai_jsonrpc.c`: JSON-RPC over standard I/O and HTTP, and the image
  mode of two fyai ends (`doc/remote-sessions-sdd.md`, section 5.2.1).
- `src/core/fyai_config.c`: layered configuration and config commands.
- `src/store/fyai_branch.c`: branch storage, references, and root selection.
- `src/store/fyai_query.c`: the queries that a client with no arena sends
  (`doc/future-cleanups.md`, The data of the client): a page of rows, a cursor,
  and the `rev` of the data.
- `src/store/fyai_merge.c`: branch merge and rebase.
- `src/remote/fyai_fleet_ops.c`: the operations of a fleet that the `fleet`
  commands and the `fleet` tool share: the rows of the list, the scan, the
  tasks, the search and the read, which a process with an open arena runs in
  a child, and the start and the supervisor requests.
- `src/tools/fyai_fleet_tool.c`: the `fleet` tool of a root session.
- `src/remote/fyai_fleet_watch.c`: `struct fyai_fleet_watch` (`ctx->fleet`),
  the supervisors that a root session keeps on its members, which turn the
  changes of a member into events.
- `src/store/fyai_fleet.c`: the tree of projects: the nearest project at or
  above a directory, the parent of a project and the root, by a walk up to a
  ceiling, and the scan of the projects below a root
  (`doc/fyai-fleet-sdd.md`, sections 5.1 and 6.1).
- `src/provider/fyai_catalog.c`: provider and model catalogue.
- `src/core/fyai_media.c`: attachments that a message carries, their bytes, and
  the clipboard helper (`doc/media.md`). `fyai_image.*` is something else:
  the prebuilt embedded YAML images.
- `src/tools/fyai_skill.c`: branch-stored skills and the `skill` tool.
- `src/tools/fyai_settings.c`: the `settings` tool, which reads, selects and
  exports the configuration by the `x-fyai-model` access of each key, and
  the catalogue. Its exports go to a private `mkdtemp()` directory that
  `fyai_settings_release()` removes at cleanup and
  `fyai_settings_abandon()` forgets in a forked child.
- `src/*/*.h`: shared structures and internal interfaces.
- `data/`: embedded configuration schemas and catalogue data.
- `tests/`: unit tests, functional cases, mock providers, and scenarios.

