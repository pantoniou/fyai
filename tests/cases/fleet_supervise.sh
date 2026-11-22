#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify CLI supervision of fleet sessions by PROJECT:BRANCH: start with a
# task, send messages, interrupt an in-flight turn, answer a pending question
# on a user-driven session, and kill the session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_supervise.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
fyai_test_on_exit 'touch "$TEST_DIR/release-long"'

# Fail cleanly when no server is running.
run_fyai fleet start a "Fix the build"
assert_status 1
assert_stderr_contains "no server runs the sessions of"

"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
mock_watch

# Start a headless member session with an initial task.
run_fyai fleet start a "Fix the build"
assert_status 0
assert_stdout_contains "started a:fleet/fix-the-build"
NAME=a:fleet/fix-the-build
wait_requests 1

# Send a message and confirm delivery as an attributed event.
run_fyai session send "$NAME" Also run the tests
assert_status 0
wait_requests 2
assert_request 1 '"[message '"'"'user'"'"']" in json.dumps(r["body"]) and "Also run the tests" in json.dumps(r["body"])'

# Interrupt a blocked model request so the next message can execute.
run_fyai session send "$NAME" Take long
assert_status 0
wait_requests 3
run_fyai session interrupt "$NAME"
assert_status 0
run_fyai session send --from root:main "$NAME" After the stop
assert_status 0
wait_requests 4
assert_request 3 '"[message '"'"'root:main'"'"']" in json.dumps(r["body"]) and "After the stop" in json.dumps(r["body"])'

# Sending to a non-existent live session fails.
run_fyai session send a:fleet/none Hello
assert_status 1
assert_stderr_contains "no live session on the branch"

# Answer an open model question on a user-driven session via `session answer`.
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/asked"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - type: Ask me
  - wait-screen: "WHICH-COLOUR?"
    timeout: session
  - touch: "$SYNC/asked"
    timeout: session
  - wait-screen: "You chose teal."
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/asked" || true
run_fyai session answer main teal-xyz
assert_status 0
wait "$driver" || fail "the answer did not reach the question: $(tail -c 2000 "$TEST_DIR/pty.out")"

# Terminate the supervised session by PROJECT:BRANCH name.
run_fyai session kill "$NAME"
assert_status 0
assert_stdout_contains "ended $NAME"
pass
