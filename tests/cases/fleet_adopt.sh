#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a root session can adopt an existing user-driven session,
# assigning it root lineage and a task, and receiving events from its turns
# (including exit after the root detaches).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_adopt.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/member" "$SYNC/root"
# Start a user-driven member; completing one turn registers it with the server.
(cd a && pty_run "$TEST_DIR/member.out" "$FYAI_BIN" -b work/hand -i 8<&- <<PTY
steps:
  - type: Hello
  - wait-screen: "Hi there."
    timeout: session
  - touch: "$SYNC/member"
    timeout: session
  - wait-file: "$SYNC/adopted"
    contains: go
    timeout: session
  - type: Next
  - wait-screen: "Next done."
    timeout: session
  - wait-file: "$SYNC/finish"
    contains: go
    timeout: session
PTY
) &
member=$!
read -r _ <"$SYNC/member" || true

# Adopt the member from the root session and verify it receives the next turn.
pty_run "$TEST_DIR/root.out" "$FYAI_BIN" -b main -i 8<&- <<PTY &
steps:
  - type: /session adopt a:work/hand Check the hand job
  - wait-screen: "adopted a:work/hand as task 1"
    timeout: session
  - touch: "$SYNC/root"
    timeout: session
  - wait-screen: "Heard hand."
    timeout: session
PTY
root=$!
read -r _ <"$SYNC/root" || true
echo go >"$SYNC/adopted"
wait "$root" || fail "the root did not hear of the member: $(tail -c 2000 "$TEST_DIR/root.out")"
# Let the member exit after the root detaches so its exit event does not race
# with root shutdown.
echo go >"$SYNC/finish"
wait "$member" || fail "the member failed: $(tail -c 2000 "$TEST_DIR/member.out")"

"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$FYAI_BIN" fleet list --output json >"$TEST_DIR/list.json"
"$PYTHON" -I - "$TEST_DIR/tasks.json" "$TEST_DIR/list.json" <<'PY' || fail "the adoption: $(cat "$TEST_DIR/tasks.json" "$TEST_DIR/list.json")"
import json, sys
tasks = json.load(open(sys.argv[1]))
assert [(t['name'], t['state'], t['text']) for t in tasks] == \
    [('a:work/hand', 'open', 'Check the hand job')], tasks
row = [r for r in json.load(open(sys.argv[2])) if r['branch'] == 'work/hand'][0]
assert row['task'] == 1 and row['parent'] == '.:main', row
PY
pass
