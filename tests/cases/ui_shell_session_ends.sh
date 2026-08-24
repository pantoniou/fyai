#!/bin/bash
# SPDX-License-Identifier: MIT
# A session that opened stays opened, whatever its program does next.
#
# The child that serves a session leaves when its program does. Its exit
# status says nothing about the call: the session opened before the program
# ran, and the model was told so. Taking that status as the answer turned every
# short-lived session into one that "could not be opened", which the user only
# saw on a terminal, where the parent reaps the child before it collects the
# job.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session_ends.json

FYAI_PTY_INPUT="open a shell that ends" FYAI_PTY_NEEDLE="done." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the session was reported as unopened"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])["body"]
results = [m["content"] for m in last["messages"] if m.get("role") == "tool"]
if len(results) != 2:
    sys.stderr.write("expected two tool results, got %d\n" % len(results))
    sys.exit(1)
opened, read = results

if "could not be opened" in opened:
    sys.stderr.write("a session that opened was reported as failed: %r\n"
                     % opened)
    sys.exit(1)
if "started" not in opened:
    sys.stderr.write("the session did not report as started: %r\n" % opened)
    sys.exit(1)
# What the program wrote before it left is still there to read.
if "SESSION-RAN" not in read:
    sys.stderr.write("the session lost what its program wrote: %r\n" % read)
    sys.exit(1)
PYEOF

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<"PYEOF" || fail "the session screen is wrong"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 100)
s.feed(open(sys.argv[1], "rb").read())
seen = s.scrollback + s.lines()
titles = [l for l in seen if "a program that ends at once" in l]
# One block on the screen, not one for each repaint.
if len(titles) != 1:
    raise SystemExit("the session drew %d blocks: %r" % (len(titles), titles))
# The status of the program belongs on that block.
if "exit 3" not in titles[0]:
    raise SystemExit("the block does not carry the exit: %r" % titles[0])
PYEOF

mock_stop 3
pass
