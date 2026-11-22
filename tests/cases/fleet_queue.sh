#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a start above fleet/max_live is queued and automatically launched
# once an active member exits. Waits on the Fleet panel rather than replies
# because completion events may coalesce into a single model request.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_queue.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
"$FYAI_BIN" config set fleet/max_live 1 >/dev/null 2>&1 || fail "cannot set fleet/max_live"
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - type: Plan the work
  - wait-screen: "Alpha job. Run it?"
    timeout: session
  - wait-screen: "1. Reject"
  - write: "\e[B\r"
  - wait-screen: "Beta job. Run it?"
    timeout: session
  - wait-screen: "1. Reject"
  - write: "\e[B\r"
  - wait-screen: "a:fleet/alpha-job · idle — Alpha done."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-screen: "a:fleet/beta-job · idle — Beta done."
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true

# Beta stays queued while Alpha occupies max_live=1; killing Alpha starts Beta.
"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" -I - "$TEST_DIR/tasks.json" <<'PY' || fail "the tasks: $(cat "$TEST_DIR/tasks.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
states = {r['name']: r['state'] for r in rows}
assert states == {'a:fleet/alpha-job': 'open', 'a:fleet/beta-job': 'queued'}, states
PY
run_fyai session kill a:fleet/alpha-job
assert_status 0
wait "$driver" || fail "the root did not start the queued task: $(tail -c 2000 "$TEST_DIR/pty.out")"
"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks2.json"
"$PYTHON" -I - "$TEST_DIR/tasks2.json" <<'PY' || fail "the tasks after: $(cat "$TEST_DIR/tasks2.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
states = {r['name']: r['state'] for r in rows}
assert states['a:fleet/beta-job'] == 'open', states
PY
pass
