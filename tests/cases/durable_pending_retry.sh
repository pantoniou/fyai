#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that with `durable`, if restoring pending work fails for one record,
# that record is retained for the next restart while succeeded records are not
# re-run.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start durable_pending_retry.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true --set durable=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mock_watch
"$FYAI_BIN" -b main --color off "Start the jobs" >out.txt 2>err.txt 8<&- &
pid=$!
wait_requests 2
kill -9 "$pid"
wait "$pid" 2>/dev/null || true

start_session() {
	pty_run "$TEST_DIR/pty.out" \
	    "$FYAI_BIN" -b main -k test-key --theme dark \
	    --set display/markdown=true --set display/stream=false \
	    --set tools=true --set durable=true --set api=chat-completions \
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<PTY
timeout: 30
steps:
  - wait: $1
    timeout: session
PTY
}

# Inject a restart failure for `mon`; `later` should restore and clear.
FYAI_TEST_PENDING_FAIL=mon start_session "Picked up the wait."
"$FYAI_BIN" -b main dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "name: mon" "$TEST_DIR/state.out" ||
	fail "the monitor that did not restart was dropped"
grep -qF "name: later" "$TEST_DIR/state.out" &&
	fail "the wait that restarted is stored again"

# Next restart should restore `mon` without re-running `later`.
start_session "Noted the restart."
"$PYTHON" -I - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the second start did not restart the monitor once"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
users = "\n".join(m["content"] for m in msgs
                  if m.get("role") == "user" and isinstance(m.get("content"), str))
assert users.count("monitor 'mon' restarted") == 1, users
assert users.count("wait 'later' fired") == 1, users
PY
touch release-pending
mock_stop
pass
