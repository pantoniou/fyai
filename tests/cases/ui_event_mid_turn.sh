#!/bin/bash
# SPDX-License-Identifier: MIT
# An event enters a turn at the next tool boundary.
#
# A named wait fires while the model still works. The report joins the same
# turn before the next model request, so no turn of its own follows.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start event_mid_turn.json

FYAI_PTY_INPUT="arm and keep working" FYAI_PTY_NEEDLE="Event seen mid turn." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the event did not join the running turn"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
if len(reqs) != 2:
    raise SystemExit("expected two requests, got %d" % len(reqs))
msgs = reqs[-1]["body"]["messages"]
roles = [m.get("role") for m in msgs]
fired = [i for i, m in enumerate(msgs) if m.get("role") == "user"
         and "wait 'later' fired" in m.get("content", "")]
tools = [i for i, r in enumerate(roles) if r == "tool"]
if not fired:
    raise SystemExit("the event never reached the model: %r" % roles)
if min(fired) < max(tools):
    raise SystemExit("the event came before the tool results: %r" % roles)
PYEOF

mock_stop 2
pass
