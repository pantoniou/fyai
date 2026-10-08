#!/bin/bash
# SPDX-License-Identifier: MIT
# A wait that holds the turn for a list of background sub-agents.
#
# `wait` with `for` and `mode: all` returns the reports of every sub-agent as
# its result. The reports are not given again in a turn of their own.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start wait_for_all.json

FYAI_PTY_INPUT="run two and collect" FYAI_PTY_NEEDLE="Collected both." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the wait did not collect both reports"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
results = [m["content"] for m in msgs if m.get("role") == "tool"]
users = [m["content"] for m in msgs if m.get("role") == "user"]

if not any("REPORT-ONE" in r and "REPORT-TWO" in r for r in results):
    raise SystemExit("the result lacks a report: %r" % results)
if any("REPORT-" in u for u in users):
    raise SystemExit("a report came again as a turn: %r" % users)
PYEOF

mock_stop 5
pass
