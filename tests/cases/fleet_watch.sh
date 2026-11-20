#!/bin/bash
# SPDX-License-Identifier: MIT
# A root session hears of the members that it started: the answer of a member
# reaches its model as an event. The open tasks are the subscriptions, so a
# root session that starts again on the branch hears of the member again.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_watch.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

# The model starts a member; its answer comes back as an event of the root,
# and the fleet panel of the page shows the member with it.
pty_run "$TEST_DIR/pty1.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Start the work
  - wait-screen: "1. Reject"
    timeout: session
  - write: "\e[B\r"
  - wait-screen: "The member finished."
    timeout: session
  - wait-screen: "a:fleet/do-the-work · idle — Work done."
    timeout: session
PTY
assert_request 3 '"[session '"'"'a:fleet/do-the-work'"'"' idle]\nWork done." in r["body"]["messages"][-1]["content"]'

# The title of a member is the description of its branch: the list says it,
# and the panel of a root session that takes the task up shows it.
(cd a && "$FYAI_BIN" branch describe fleet/do-the-work "Do the work" >/dev/null 2>&1) ||
	fail "cannot title the member"
run_fyai fleet list --output json
assert_status 0
assert_stdout_contains '"title": "Do the work"'

# A new root session on the branch takes the open task up again.
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - wait-screen: "The member finished."
    timeout: session
  - wait-screen: "Do the work (a:fleet/do-the-work)"
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-screen: "Heard again."
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true
run_fyai session send a:fleet/do-the-work More work
assert_status 0
wait "$driver" || fail "the new root did not hear of the member: $(tail -c 2000 "$TEST_DIR/pty2.out")"
pass
