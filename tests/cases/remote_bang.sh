#!/bin/bash
# SPDX-License-Identifier: MIT
# A command of the user in the full-screen client runs in a terminal of the
# worker: its output reaches a tile of the client, which stays after the
# program ends until Escape dismisses it. The keys after Escape go to the
# prompt at once: they do not wait for the worker to drop the session.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup claude_sdk_branch.json
pty_run "$TEST_DIR/pty.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" -b remote/bang remote connect <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: "!printf 'bang-out-row\\n'"
  - wait-screen: bang-out-row
  - wait-screen: Esc closes
  - write: "\e"
  - type: /quit
PTY
remote_test_stop
mock_stop 0
pass
