#!/bin/bash
# SPDX-License-Identifier: MIT
# A supervisor reaches the sessions of the fleet by PROJECT:BRANCH: it starts
# one with a task, sends it messages, interrupts its turn and ends it, and it
# answers the question of a session that a person drives.
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

# With no server there is nothing to reach.
run_fyai fleet start a "Fix the build"
assert_status 1
assert_stderr_contains "no server runs the sessions of"

"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
mock_watch

# A session starts in the project with its task, and runs with no client.
run_fyai fleet start a "Fix the build"
assert_status 0
assert_stdout_contains "started a:fleet/fix-the-build"
NAME=a:fleet/fix-the-build
wait_requests 1

# A message reaches it as an event from its sender.
run_fyai session send "$NAME" Also run the tests
assert_status 0
wait_requests 2
assert_request 1 '"[message '"'"'user'"'"']" in json.dumps(r["body"]) and "Also run the tests" in json.dumps(r["body"])'

# An interrupt ends a turn that waits on the model: the next message runs.
run_fyai session send "$NAME" Take long
assert_status 0
wait_requests 3
run_fyai session interrupt "$NAME"
assert_status 0
run_fyai session send --from root:main "$NAME" After the stop
assert_status 0
wait_requests 4
assert_request 3 '"[message '"'"'root:main'"'"']" in json.dumps(r["body"]) and "After the stop" in json.dumps(r["body"])'

# A name of no live session is refused.
run_fyai session send a:fleet/none Hello
assert_status 1
assert_stderr_contains "no live session on the branch"

# The question of a session that a person drives is answered from outside.
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

# The session ends by its name.
run_fyai session kill "$NAME"
assert_status 0
assert_stdout_contains "ended $NAME"
pass
