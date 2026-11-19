#!/bin/bash
# SPDX-License-Identifier: MIT
# A root session adopts a live session that a person started: the session gets
# the lineage of the root and a task, and the root hears of what it does. The
# member leaves after the root. The root session that stays at the server
# hears of that, and a step answers it.
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
# The member: a person drives it; a turn registers it with the server.
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

# The root adopts it, and hears of its next turn.
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
# The member leaves after the root, so that the root does not start a turn for
# its end while it leaves.
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
