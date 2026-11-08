#!/bin/bash
# SPDX-License-Identifier: MIT
# The full-screen client draws a remote session from the mirror of its UI
# model: the todo panel and the header of the worker, a streamed answer,
# commands of the view and of the worker, and the stored turns when it
# attaches again.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup claude_sdk_branch.json
"$FYAI_BIN" branch create remote/ui >"$TEST_DIR/branch.out"
"$FYAI_BIN" --branch remote/ui tool todo_write \
	'{"todos":[{"content":"Remote panel task","status":"pending","priority":"high"}]}' \
	>"$TEST_DIR/todo.out"
pty_run "$TEST_DIR/first.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" -b remote/ui remote connect <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: Remote panel task
  - wait-screen: mock-model
  - type: First remote question.
  - wait-screen: First answer.
  - type: /theme dark
  - wait-screen: "theme: dark"
  - type: /status
  - wait-screen: Metric
  - type: /todo clear
  - wait-gone: Remote panel task
  - type: /checkout -b remote/moved remote/ui
  - wait-screen: "fyai: remote/moved"
  - type: /quit
PTY
# The worker reported the branch it moved to: the new name finds it.
pty_run "$TEST_DIR/second.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" -b remote/moved remote connect <<'PTY'
rows: 40
cols: 80
steps:
  - wait-screen: First answer.
  - type: Continue the remote conversation.
  - wait-screen: Second answer.
  - resize: "45x100"
  - wait-screen: Second answer.
  - type: /quit
PTY
remote_test_stop
mock_stop 2
assert_request 0 'any("First remote question." in str(m.get("content", "")) for m in r["body"]["messages"])'
pass
