#!/bin/bash
# SPDX-License-Identifier: MIT
# A wait for an event returns at its time limit.
#
# `wait` with `for` and `seconds` gives up when the limit passes, names what
# still runs, and leaves it running.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start wait_for_timeout.json

FYAI_PTY_INPUT="start the slow one" FYAI_PTY_NEEDLE="Saw the end." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the wait did not give up at its limit"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
results = [m["content"] for m in msgs if m.get("role") == "tool"]

if not any("still waiting for 'slow'" in r for r in results):
    raise SystemExit("the wait did not report the limit: %r" % results)
PYEOF

# The sub-agent can be stopped before it sends its request: a slow runner needs
# more than the limit of the wait to start one, so the count is not fixed.
mock_stop
pass
