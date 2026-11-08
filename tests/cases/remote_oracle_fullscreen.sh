#!/bin/bash
# SPDX-License-Identifier: MIT
# The shipped display - the full-screen page with Markdown, the controls of the
# mouse and the screens of calls in the transcript - shows a remote session as
# it shows a local one: the cards, a terminal of the model, and a command of
# the user.
# A program that waits for input does not wake the model: the worker of a remote
# session runs no turn for such an event, and a local session would.
set -eu
. "$(dirname "$0")/../oracle-harness.sh"

oracle_setup remote_terminal.json
"$FYAI_BIN" --set tools=true --set display/screen=fullscreen \
	--set display/markdown=true --set display/work_controls=full \
	--set display/tool_display=inline \
	--set display/command_output=transcript --set shell/input_poll_ms=0 \
	>/dev/null 2>&1
"$FYAI_BIN" branch create oracle/fullscreen >/dev/null 2>&1
oracle_compare --branch oracle/fullscreen <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: mock-model
  - checkpoint: start
  - type: Open a terminal.
  - wait-screen: The session runs.
  - wait-row: term-screen-row
  - write: "\x14"
  - type: hello
  - wait-screen: got-hello
  - type: bye
  - wait-gone: Ctrl-] returns to the prompt
  - checkpoint: ended
  - type: "!printf 'bang-row\\nbang-done'"
  - wait-screen: bang-done
  - wait-screen: Esc closes
  - checkpoint: bang
  - write: "\e"
  - wait-gone: Esc closes
  - checkpoint: closed
  - type: /quit
PTY
pass
