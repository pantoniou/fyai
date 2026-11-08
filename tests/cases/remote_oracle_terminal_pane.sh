#!/bin/bash
# SPDX-License-Identifier: MIT
# A terminal session that the model opens in a remote session is shown as a
# local session shows it under display/tool_display: pane. It starts at the
# size that the client gives, takes the keys with Ctrl-T, and ends as a local
# one ends.
# A program that waits for input does not wake the model: the worker of a remote
# session runs no turn for such an event, and a local session would.
set -eu
. "$(dirname "$0")/../oracle-harness.sh"

oracle_setup remote_terminal.json
"$FYAI_BIN" --set tools=true --set display/tool_display=pane \
	--set shell/input_poll_ms=0 >/dev/null 2>&1
"$FYAI_BIN" branch create oracle/terminal >/dev/null 2>&1
oracle_compare --branch oracle/terminal <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: "❯"
  - type: Open a terminal.
  - wait-screen: The session runs.
  - wait-row: term-screen-row
  - wait-row: "❯"
  - checkpoint: running
  - write: "\x14"
  - type: hello
  - wait-row: got-hello
  - checkpoint: answered
  - type: bye
  - wait-gone: Ctrl-] returns to the prompt
  - checkpoint: ended
  - type: /quit
PTY
pass
