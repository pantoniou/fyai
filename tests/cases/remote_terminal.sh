#!/bin/bash
# SPDX-License-Identifier: MIT
# The full-screen client shows a terminal session of the worker: the bytes of
# its program reach a tile of the client, which replays them, and the keys
# typed into the tile reach the program.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup remote_terminal.json
"$FYAI_BIN" --set tools=true >/dev/null 2>&1
pty_run "$TEST_DIR/pty.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" -b remote/term remote connect <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: Open a terminal.
  - wait-screen: The session runs.
  - wait-screen: shell [watch]
  - wait-row: term-screen-row
  - write: "\x14"
  - type: hello
  - wait-screen: got-hello
  - write: "\x1d"
  - type: /quit
PTY
remote_test_stop
mock_stop 2
pass
