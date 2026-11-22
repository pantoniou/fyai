#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that when a member worker crashes while its task is open, the root
# session automatically restarts it and receives a crash-restart event
# (whereas explicit user kills are not restarted).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_restart.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
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
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - type: Start the work
  - wait-screen: "1. Reject"
    timeout: session
  - write: "\e[B\r"
  - wait-screen: "a:fleet/do-the-work · idle — Work done."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-screen: "Member restarted."
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true

# Crash the member worker with SIGABRT while its task is open.
"$FYAI_BIN" fleet list --output json >"$TEST_DIR/fleet.json"
pid=$("$PYTHON" -I - "$TEST_DIR/fleet.json" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
print([r for r in rows if r['branch'] == 'fleet/do-the-work'][0]['pid'])
PY
)
kill -ABRT "$pid"
wait "$driver" || fail "the root did not restart the member: $(tail -c 2000 "$TEST_DIR/pty.out")"

# Confirm the member is live again under a new PID and the model was notified.
"$FYAI_BIN" fleet list --output json >"$TEST_DIR/fleet2.json"
"$PYTHON" -I - "$TEST_DIR/fleet2.json" "$pid" <<'PY' || fail "the member is not live again: $(cat "$TEST_DIR/fleet2.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
row = [r for r in rows if r['branch'] == 'fleet/do-the-work'][0]
assert row['state'] in ('idle', 'running', 'waiting'), row
assert row['pid'] != int(sys.argv[2]), row
PY
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the model was not told of the restart"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
assert any("restarted: its process crashed" in str(m.get("content", ""))
           for r in reqs for m in r["body"]["messages"])
PY
pass
