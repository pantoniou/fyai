#!/bin/bash
# SPDX-License-Identifier: MIT
# A session that is waiting to be answered says so.
#
# A program stopped in a read of its input can do nothing until something
# answers it, and nobody is at that terminal. The model is told, in a turn of
# its own, so that it can answer instead of leaving the program there.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_input_wanted.json

FYAI_PTY_INPUT="open a shell that asks" FYAI_PTY_NEEDLE="It answered." \
FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/stream=false --set tools=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the session never asked for input"
import json
import sys

reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
# The last request carries the whole conversation once, which is where a
# turn is counted: every request repeats what came before it.
last = reqs[-1]
users = [m["content"] for m in last["messages"] if m.get("role") == "user"]
asked = [u for u in users if "waiting for input" in u]
if not asked:
    raise SystemExit("the model was never told the session wants input")
# What it asked goes with it: a question with no question in it says nothing.
if "WHO-ARE-YOU?" not in asked[0]:
    raise SystemExit("the event does not say what was asked: %r" % asked[0])
if "'asker'" not in asked[0]:
    raise SystemExit("the event does not say which session: %r" % asked[0])
# Told once for one wait, not on every look.
if len(asked) != len(set(asked)):
    raise SystemExit("the same wait was reported more than once")

# The answer reached the program, so it was really waiting for one.
results = [m["content"] for m in last["messages"]
           if m.get("role") == "tool"]
if not any("HELLO-ADA" in r for r in results):
    raise SystemExit("the program never got its answer")
PYEOF

mock_stop 4
pass
