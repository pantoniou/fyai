#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a pending named wait due after serve/idle_ms keeps a parked
# worker alive so the same worker executes the wait-triggered turn without a
# client attached.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start serve_idle_wait.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false --set tools=true
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

member_pid() {
	"$FYAI_BIN" fleet list a --output json | "$PYTHON" -c '
import json, sys
for r in json.load(sys.stdin):
    if r["branch"] == "fleet/work" and r.get("pid"):
        print(r["pid"])'
}

run_fyai fleet start --branch fleet/work a Wait for a while
assert_status 0
wait_requests 2
first="$(member_pid)"
[ -n "$first" ] || fail "no worker for the member"

# Wait fires past serve/idle_ms; verify the original worker ran the turn.
wait_requests 3
# The wait lives in the worker that set it: a second start would mean that the
# first worker had ended, and the turn is of another one.
[ "$(grep -c "started the worker .* on branch 'fleet/work'" .fyai/logs/serve.log)" -eq 1 ] ||
	fail "the worker that set the wait did not run it: $(cat .fyai/logs/serve.log)"
pass
