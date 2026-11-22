#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify fleet/approve and fleet/max_live: user approval gates member starts,
# starts above max_live are approved then queued, and non-interactive runs
# refuse starts when approval is required.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_policy.json
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
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

# Reject task 1, approve task 2, approve task 3 (queued due to max_live=1).
# In the scenario, non-interactive prompt steps precede the interactive replay,
# and the tool result plus member completion event may coalesce into one
# request; both mock branches return the same reply.
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Plan the work
  - wait-screen: "Task one. Run it?"
    timeout: session
  - wait-screen: "1. Reject"
  - write: "\r"
  - wait-screen: "Task two. Run it?"
    timeout: session
  - wait-screen: "1. Reject"
  - write: "\e[B\r"
  - wait-screen: "Task three. Run it?"
    timeout: session
  - wait-screen: "1. Reject"
  - write: "\e[B\r"
  - wait-screen: "Heard of two."
    timeout: session
  - wait-screen: "a:fleet/task-two · idle — Two done."
    timeout: session
PTY
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the policy did not hold"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
def users(r):
    return [m["content"] for m in r["body"]["messages"] if m["role"] == "user"]
started = [users(r)[0] for r in reqs if users(r) and users(r)[0].startswith("Task ")]
assert started == ["Task two"], started
tools = [m["content"] for r in reqs for m in r["body"]["messages"] if m["role"] == "tool"]
assert any("the person rejected the call" in t for t in tools), tools
assert any('"queued": true' in t or '"queued":true' in t for t in tools), tools
PY

# Non-interactive run has no user to approve: member start must be refused.
"$FYAI_BIN" "Plan alone" >"$TEST_DIR/alone.out" 2>&1 ||
	fail "the run failed: $(cat "$TEST_DIR/alone.out")"
grep -q "Could not start." "$TEST_DIR/alone.out" ||
	fail "the run did not end: $(cat "$TEST_DIR/alone.out")"
pass
