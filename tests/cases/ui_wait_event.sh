#!/bin/bash
# SPDX-License-Identifier: MIT
# A wait that does not hold the turn.
#
# `time` says what the clock says, because the system turn is frozen when a
# conversation starts and cannot. A named `wait` returns at once and fires
# later, and the firing reaches the model as a turn of its own: the model is
# asked again without the user typing anything.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start wait_event.json

FYAI_PTY_INPUT="start a wait" FYAI_PTY_NEEDLE="picking it up." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail "the wait did not fire into a turn"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
results = [m["content"] for m in msgs if m.get("role") == "tool"]

# The clock answered.
if not any("epoch" in r for r in results):
    raise SystemExit("time did not report the clock: %r" % results)
# The named wait returned at once and said so.
if not any("does not hold your turn" in r for r in results):
    raise SystemExit("the wait held the turn: %r" % results)

# The firing reached the model as a turn of its own, with nobody typing it.
users = [m["content"] for m in msgs if m.get("role") == "user"]
if not any("wait 'later' fired" in u for u in users):
    raise SystemExit("the wait never fired into a turn: %r" % users)
if not any("check the thing" in u for u in users):
    raise SystemExit("the reason was not given back: %r" % users)
EOF

mock_stop 4
pass
