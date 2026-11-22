#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify crash recovery when a root worker dies after persisting a fleet task
# but before starting its member: restarting on the root branch launches the
# member exactly once.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_tasks.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set durable=true
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

# Inject a root worker exit right after persisting the task: the test hook
# opens the FIFO, unblocks our read, and aborts the root worker.
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/lost" "$SYNC/started"
pty_run "$TEST_DIR/pty1.out" "$(command -v env)" "FYAI_TEST_FLEET_LOSE_ROOT=$SYNC/lost" "$FYAI_BIN" -b main -i 8<&- <<'PTY' >/dev/null 2>&1 &
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - type: /fleet start a Fix the build
  - wait-screen: NEVER-ON-SCREEN
    timeout: session
PTY
driver1=$!
read -r _ <"$SYNC/lost" || true
# Stop the orphaned client driver waiting on the dead root worker.
kill "$driver1" 2>/dev/null || true
wait "$driver1" 2>/dev/null || true

"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" -I - "$TEST_DIR/tasks.json" <<'PY' || fail "the task was not stored: $(cat "$TEST_DIR/tasks.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['id'], r['state'], r['text']) for r in rows] == [(1, 'open', 'Fix the build')], rows
PY
[ "$(wc -l <"$TEST_DIR/requests.jsonl")" -eq 1 ] || fail "a member started before the root was lost"

# Restarting on the root branch picks up the open task and starts one member.
mock_watch
pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" -b main -i 8<&- <<PTY &
steps:
  - touch: "$SYNC/started"
    timeout: session
  - wait-file: "$SYNC/checked"
    equals: ok
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/started" || true
# Wait for the member's turn request (request 2 overall) to reach the mock.
wait_requests 2

"$FYAI_BIN" fleet list --output json >"$TEST_DIR/fleet.json"
"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" -I - "$TEST_DIR/fleet.json" "$TEST_DIR/tasks.json" "$TEST_DIR/requests.jsonl" <<'PY' ||
import json, sys
members = [r for r in json.load(open(sys.argv[1]))
           if r['project'] == 'a' and r['branch'] == 'fleet/fix-the-build']
assert len(members) == 1, members
tasks = json.load(open(sys.argv[2]))
assert [(r['id'], r['state']) for r in tasks] == [(1, 'open')], tasks
reqs = [json.loads(l) for l in open(sys.argv[3])]
started = [r for r in reqs if "Fix the build" in json.dumps(r["body"]["messages"])
           and "Say done" not in json.dumps(r["body"]["messages"])]
assert len(started) == 1, len(started)
PY
	fail "the member of the task did not run once: $(cat "$TEST_DIR/fleet.json" "$TEST_DIR/tasks.json")"
echo ok >"$SYNC/checked"
wait "$driver" || fail "the second root failed: $(tail -c 2000 "$TEST_DIR/pty2.out")"
pass
