#!/bin/bash
# SPDX-License-Identifier: MIT
# The full-screen client draws the work of the worker: a shell call runs as
# in a local session, with no question, and its output streams into a band of
# the client. The view of a file that the model writes is rendered by the client
# from its source. When the turn is stored, the stored exchange replaces the
# live rows, as it does in a local session.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup remote_band.json
"$FYAI_BIN" --set tools=true --set display/tool_detail=full >/dev/null 2>&1
pty_run "$TEST_DIR/pty.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" -b remote/band remote connect <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: Run the shell.
  - wait-row: band-row-2
  - touch: release
  - wait-row: view-line-2
  - touch: viewed
  - wait-screen: Shell finished.
  - wait-row: band-row-1
  - type: /quit
PTY
remote_test_stop
mock_stop 3
assert_request 1 '"band-row-1" in json.dumps(r["body"]["messages"])'
pass
