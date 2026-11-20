#!/bin/bash
# SPDX-License-Identifier: MIT
# A worker that the server keeps for no client, with nothing to do, ends after
# serve/idle_ms: the session stays stored, and a wakeup, also one that comes
# while the worker ends, runs the session again in a new worker. A message
# to a stored member starts it, also when no server runs.
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
printf "serve:\n  idle_ms: 300\n" >"$TEST_DIR/serve.yaml"
"$FYAI_BIN" --config "$TEST_DIR/serve.yaml" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
mock_watch

# member_pid: the pid of the worker of a:fleet/work, or empty.
member_pid() {
	"$FYAI_BIN" fleet list a --output json | "$PYTHON" -c '
import json, sys
for r in json.load(sys.stdin):
    if r["branch"] == "fleet/work" and r.get("pid"):
        print(r["pid"])'
}
log=.fyai/logs/serve.log

# wait_log TEXT: the log of the server has TEXT, which it will have; the
# deadline bounds a failure.
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

# The idle member ends on its own, stored.
"$PYTHON" "$TESTS_DIR/wait_exit.py" "$first" "$((30 * FYAI_TIMEOUT_SCALE))" ||
	fail "the idle worker did not end"
grep -q "idle for 300 ms, ends" "$log" || fail "the server did not end it: $(cat "$log")"
[ -z "$(member_pid)" ] || fail "the ended worker is still listed"

# A wakeup runs it again, in a new worker.
run_fyai fleet start --branch fleet/work a Work
assert_status 0
wait_requests 2
second="$(member_pid)"
[ -n "$second" ] && [ "$second" != "$first" ] || fail "no new worker: '$second'"

# A wakeup while the server asks the worker: the worker cannot answer at
# once, a client message goes to it meanwhile, and the server does not end
# it. The worker answers when it runs again, and then runs the message.
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
grep -q "worker $second (branch 'fleet/work'): idle for 300 ms, ends" "$log" ||
	fail "the second worker did not end by the server"

# With no server, a message to the stored member starts its session again.
"$FYAI_BIN" serve stop >/dev/null 2>&1 || fail "the root server did not stop"
fyai_test_on_exit '(cd a && "$FYAI_BIN" serve stop >/dev/null 2>&1) || true'
run_fyai session send a:fleet/work More
assert_status 0
wait_requests 4
mock_stop 4
pass
