#!/bin/bash
# SPDX-License-Identifier: MIT
# The model cancels what it started in the background.
#
# `cancel` ends a pending named wait and a running background sub-agent by
# name. The sub-agent reports its end as a turn, and the wait never fires.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start cancel_background.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: start work to cancel
  - wait: Saw the failure.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the work was not cancelled"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
results = [m["content"] for m in msgs if m.get("role") == "tool"]
users = [m["content"] for m in msgs if m.get("role") == "user"]

if not any("wait 'w1' cancelled" in r for r in results):
    raise SystemExit("the wait was not cancelled: %r" % results)
if not any("agent 'bg' was asked to stop" in r for r in results):
    raise SystemExit("the sub-agent was not stopped: %r" % results)
if not any("agent 'bg' failed" in u for u in users):
    raise SystemExit("the end was not reported: %r" % users)
if any("wait 'w1' fired" in u for u in users):
    raise SystemExit("a cancelled wait fired: %r" % users)
PYEOF

# The end of the sub-agent reaches the model with the results of the cancel, or
# after them as one more request; both are the work cancelled.
mock_stop_quiet
served="$(cat "$TEST_DIR/served" 2>/dev/null || echo 0)"
[ "$served" -eq 4 ] || [ "$served" -eq 5 ] ||
    fail "expected 4 or 5 requests, served $served"

# The title row of the stored call names what is cancelled.
run_fyai history
assert_status 0
assert_stdout_contains 'cancel w1'
assert_stdout_contains 'cancel bg'
pass
