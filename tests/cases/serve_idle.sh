#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify serve/idle_ms shutdown and wakeup: unattached idle workers exit and
# stay stored, wakeups spawn a new worker, messages racing the idle check keep
# the worker alive, and messaging a stored member starts it even without a server.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start serve_idle.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
# Long enough that a slow `fleet list` does not find the worker gone already.
idle_ms=1500
printf "serve:\n  idle_ms: %s\n" "$idle_ms" >"$TEST_DIR/serve.yaml"
"$FYAI_BIN" --config "$TEST_DIR/serve.yaml" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
mock_watch

# member_pid: print the worker PID for a:fleet/work, or empty if not running.
member_pid() {
	"$FYAI_BIN" fleet list a --output json | "$PYTHON" -c '
import json, sys
for r in json.load(sys.stdin):
    if r["branch"] == "fleet/work" and r.get("pid"):
        print(r["pid"])'
}
log=.fyai/logs/serve.log

# wait_log TEXT: poll server log until TEXT appears or timeout expires.
wait_log() {
	"$PYTHON" -I - "$log" "$1" "$((30 * FYAI_TIMEOUT_SCALE))" <<'PY'
import sys, time

path, text, deadline = sys.argv[1], sys.argv[2], time.monotonic() + float(sys.argv[3])
while time.monotonic() < deadline:
    try:
        if text in open(path).read():
            sys.exit(0)
    except FileNotFoundError:
        pass
    time.sleep(0.01)
sys.exit(1)
PY
}

run_fyai fleet start --branch fleet/work a Work
assert_status 0
wait_requests 1
first="$(member_pid)"
[ -n "$first" ] || fail "no worker for the member"

# Idle member exits automatically after serve/idle_ms and remains stored.
"$PYTHON" "$TESTS_DIR/wait_exit.py" "$first" "$((30 * FYAI_TIMEOUT_SCALE))" ||
	fail "the idle worker did not end"
grep -q "idle for $idle_ms ms, ends" "$log" || fail "the server did not end it: $(cat "$log")"
[ -z "$(member_pid)" ] || fail "the ended worker is still listed"

# Waking the stored member spawns a new worker PID.
run_fyai fleet start --branch fleet/work a Work
assert_status 0
wait_requests 2
second="$(member_pid)"
[ -n "$second" ] && [ "$second" != "$first" ] || fail "no new worker: '$second'"

# Pause worker during the server's idle probe so a client message arrives mid-
# check; on SIGCONT the worker must stay alive, handle the message, then idle out.
kill -STOP "$second"
wait_log "worker $second (branch 'fleet/work'): idle, asks if it is settled" ||
	fail "the server did not ask the second worker"
"$FYAI_BIN" session send a:fleet/work Again >"$TEST_DIR/send.out" 2>&1 &
sender=$!
wait_log "send to the worker $second" || fail "the message did not reach the server"
kill -CONT "$second"
wait "$sender" || fail "the message was not sent: $(cat "$TEST_DIR/send.out")"
wait_requests 3
"$PYTHON" "$TESTS_DIR/wait_exit.py" "$second" "$((30 * FYAI_TIMEOUT_SCALE))" ||
	fail "the worker did not end after the message"
grep -q "worker $second (branch 'fleet/work'): idle for $idle_ms ms, ends" "$log" ||
	fail "the second worker did not end by the server"

# With no server running, `session send` wakes the stored member on demand.
"$FYAI_BIN" serve stop >/dev/null 2>&1 || fail "the root server did not stop"
fyai_test_on_exit '(cd a && "$FYAI_BIN" serve stop >/dev/null 2>&1) || true'
run_fyai session send a:fleet/work More
assert_status 0
wait_requests 4
mock_stop 4
pass
