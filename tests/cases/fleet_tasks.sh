#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify fleet task tracking and lineage: starting a member from a root session
# creates an open task, records parent/task lineage on the member, allows
# marking the task done, and CLI starts carry no parent session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_tasks.json
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
mkfifo "$SYNC/started" "$SYNC/marked"
mock_watch
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i 8<&- <<PTY &
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - type: /fleet start a Fix the build
  - wait-screen: "started a:fleet/fix-the-build"
    timeout: session
  - touch: "$SYNC/started"
    timeout: session
  - wait-file: "$SYNC/checked"
    equals: ok
    timeout: session
  - type: /fleet task 1 done
  - wait-screen: "task 1: done"
    timeout: session
  - touch: "$SYNC/marked"
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/started" || true
# Wait for the member's first mock request so its initial lineage metadata is
# guaranteed to be committed on its branch.
wait_requests 2

"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" - "$TEST_DIR/tasks.json" <<'PY' || fail "fleet tasks: $(cat "$TEST_DIR/tasks.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['id'], r['session'], r['name'], r['state'], r['text']) for r in rows] == \
    [(1, 'main', 'a:fleet/fix-the-build', 'open', 'Fix the build')], rows
PY
"$FYAI_BIN" fleet list --output json >"$TEST_DIR/fleet.json"
"$PYTHON" - "$TEST_DIR/fleet.json" <<'PY' || fail "fleet list: $(cat "$TEST_DIR/fleet.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
got = [(r.get('task'), r.get('parent')) for r in rows
       if r['project'] == 'a' and r['branch'] == 'fleet/fix-the-build']
assert got == [(1, '.:main')], rows
PY
echo ok >"$SYNC/checked"
read -r _ <"$SYNC/marked" || true

"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" - "$TEST_DIR/tasks.json" <<'PY' || fail "the task was not marked: $(cat "$TEST_DIR/tasks.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['id'], r['state']) for r in rows] == [(1, 'done')], rows
PY
wait "$driver" || fail "the root session failed: $(tail -c 2000 "$TEST_DIR/pty.out")"

# CLI `fleet start` succeeds without a parent root session.
run_fyai fleet start --branch fleet/plain a Plain task
assert_status 0
assert_stdout_contains "started a:fleet/plain"
pass
