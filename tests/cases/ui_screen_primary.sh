#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify display/screen=primary: the page takes the whole normal screen, a
# row that leaves the top of the page goes to the scrollback once and in
# order, the prompt keeps the bottom, and the transcript stays on the screen
# when the session ends.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/primary.json" <<'PY'
import json
import sys


def answer(tag):
    text = "\n\n".join("## %s section %d\n\nRow of section %d." % (tag, i, i)
                       for i in range(1, 13))
    return {"response": {"id": "chatcmpl-primary", "object": "chat.completion",
                         "model": "mock-model",
                         "choices": [{"index": 0, "finish_reason": "stop",
                                      "message": {"role": "assistant",
                                                  "content": text}}]}}


steps = [dict(match={"last_contains": "first"}, **answer("FIRST")),
         dict(match={"last_contains": "second"}, **answer("SECOND"))]
json.dump({"steps": steps}, open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/primary.json"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --set display/markdown=true \
    --set display/screen=primary --set display/stream=false \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<PTY
rows: 20
cols: 70
steps:
  - type: first answer
  - wait-screen: "FIRST section 12"
    timeout: session
  - type: second answer
  - wait-screen: "SECOND section 12"
    timeout: session
  - wait-row: "▌❯"
  - checkpoint: "$TEST_DIR/screen.txt"
PTY

"$PYTHON" - "$TEST_DIR/screen.txt" "$TEST_DIR/pty.out" <<'EOF' || \
    fail "the primary screen is wrong"
import re
import sys

sys.path.insert(0, __import__("os").environ["TESTS_DIR"])
import screen

lines = open(sys.argv[1]).read().split("\n")
rows = [line[2:] for line in lines if line[:2] in ("B|", "R|")]
text = "\n".join(rows)
# Every section once, in the order the answers gave them: the scrollback
# and the window are one transcript.
seen = [m.group(0) for r in rows
        for m in [re.search(r"(FIRST|SECOND) section \d+", r)] if m]
want = ["%s section %d" % (t, i) for t in ("FIRST", "SECOND")
        for i in range(1, 13)]
if seen != want:
    raise SystemExit("the sections are not once and in order: %r\n%s"
                     % (seen, text))
# The page is as tall as the window: the status is its last row.
window = [line[2:] for line in lines if line.startswith("R|")]
if len(window) != 20 or "mock-model" not in window[-1]:
    raise SystemExit("the page does not fill the window:\n" +
                     "\n".join(window))
# After the session the transcript is still on the screen, without the
# prompt and the status.
s = screen.Screen(20, 70)
s.feed(open(sys.argv[2], "rb").read())
after = ["".join(r).rstrip() for r in s.grid]
if not any("SECOND section 12" in r for r in after):
    raise SystemExit("the transcript left the screen:\n" + "\n".join(after))
if any("mock-model" in r for r in after):
    raise SystemExit("the status stayed on the screen:\n" + "\n".join(after))
EOF

mock_stop 2
pass
