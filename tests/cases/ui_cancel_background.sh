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

FYAI_PTY_INPUT="start work to cancel" FYAI_PTY_NEEDLE="Saw the failure." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

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

mock_stop 4

# The title row of the stored call names what is cancelled.
run_fyai history
assert_status 0
assert_stdout_contains 'cancel w1'
assert_stdout_contains 'cancel bg'
pass
