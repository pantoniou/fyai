#!/bin/bash
# SPDX-License-Identifier: MIT
# The todo list of the branch stands above the prompt of the page, one row
# for each item with its state mark, and follows a change of the list.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai tool todo_write '{"todos":[
	{"content":"First page task","status":"completed","priority":"high"},
	{"content":"Second page task","status":"in_progress","priority":"medium"},
	{"content":"Third page task","status":"pending","priority":"low"}]}'
assert_status 0

driver=0
FYAI_TRACE="$TEST_DIR/trace.log" pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key -b main --set display/markdown=true \
	-m mock-model -i <<'PTY' || driver=$?
cols: 100
steps:
  - wait-screen: "✔ First page task"
  - wait-screen: "[>] Second page task"
  - wait-screen: "☐ Third page task"
  - type: /todo clear
  - wait-gone: Second page task
  - type: /quit
PTY
if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out" "$TEST_DIR/trace.log"; then
	skip "this build has no page support"
fi
[ "$driver" -eq 0 ] || fail "the page did not show the todo list"
pass
