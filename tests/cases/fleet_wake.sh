#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that sending to an idle-exited (stored) member from a root session
# wakes it in a new worker and delivers its reply back to the root as an event.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_wake.json
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
printf "serve:\n  idle_ms: 300\n" >"$TEST_DIR/serve.yaml"
"$FYAI_BIN" --config "$TEST_DIR/serve.yaml" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
mock_watch

# Start a member and wait for serve/idle_ms to expire so it transitions to stored.
run_fyai fleet start --branch fleet/work a Do the work
assert_status 0
wait_requests 1
"$PYTHON" -I - .fyai/logs/serve.log "$((30 * FYAI_TIMEOUT_SCALE))" <<'PY' ||
import sys, time

path, deadline = sys.argv[1], time.monotonic() + float(sys.argv[2])
while time.monotonic() < deadline:
    try:
        if "(branch 'fleet/work') exited with status 4" in open(path).read():
            sys.exit(0)
    except FileNotFoundError:
        pass
    time.sleep(0.01)
sys.exit(1)
PY
	fail "the member did not end"

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Wake it
  - wait-screen: "Heard it."
    timeout: session
PTY
pass
