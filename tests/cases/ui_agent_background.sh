#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent that does not hold the turn.
#
# An agent call with `background` returns at once and says so. When the
# sub-agent ends, its final report reaches the model as a turn of its own,
# without the user typing anything.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_background.json

FYAI_PTY_INPUT="start background work" FYAI_PTY_NEEDLE="Picked up the report." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the report did not reach the model"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
results = [m["content"] for m in msgs if m.get("role") == "tool"]
users = [m["content"] for m in msgs if m.get("role") == "user"]

if not any("started in the background" in r for r in results):
    raise SystemExit("the call held the turn: %r" % results)
if not any("agent 'bg' finished" in u and "BGREPORT-ONE" in u for u in users):
    raise SystemExit("the report never arrived as a turn: %r" % users)
PYEOF

mock_stop 4
pass
