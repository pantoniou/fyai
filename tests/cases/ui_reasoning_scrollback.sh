#!/bin/bash
# SPDX-License-Identifier: MIT
# A long reasoning trace leaves the page for the scrollback while it streams:
# the first section scrolls off before the answer starts.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/reason.json" <<'PY'
import json
import sys


def delta(**d):
    return {"choices": [{"index": 0, "delta": d}]}


events = [delta(role="assistant")]
for i in range(60):
    events.append(delta(reasoning_content="**Section %d**\n\nThinking about "
                        "step %d of the work.\n\n" % (i, i)))
held = len(events) - 1
events += [delta(content="The answer."),
           {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
step = {"sse": events, "done": True, "wait_after_event": held,
        "wait_for": "release-answer"}
json.dump({"steps": [step, step, step]},
          open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/reason.json"

for screen in primary inline fullscreen; do
    pty_run "$TEST_DIR/$screen.out" \
        "$FYAI_BIN" -k test-key --theme dark -m mock-model \
        --set display/markdown=true --set display/stream=true \
        --set display/thinking=true --set display/screen=$screen \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -i <<PTY
rows: 20
steps:
  - type: think
  - wait-screen: Section 59
  - checkpoint: "$TEST_DIR/$screen.held"
  - touch: "$TEST_DIR/release-answer"
  - wait-screen: The answer.
  - wait-screen: "\u276f"
  - checkpoint: "$TEST_DIR/$screen.done"
PTY
    rm -f "$TEST_DIR/release-answer"
    # The fullscreen page has no scrollback: its rows stay in the view.
    [ "$screen" = fullscreen ] && continue
    grep -q '^B|.*Section 0\b' "$TEST_DIR/$screen.held" ||
        fail "$screen: the reasoning did not reach the scrollback:
$(cat "$TEST_DIR/$screen.held")"
    # Each row reaches the scrollback once: none doubles or goes missing
    # when the frozen rows join the finished answer.
    "$PYTHON" - "$TEST_DIR/$screen.done" <<'PY' ||
import re
import sys

rows = [l[2:] for l in open(sys.argv[1]).read().split("\n")
        if l.startswith(("B|", "R|"))]
text = "\n".join(rows)
for i in range(60):
    n = len(re.findall(r"\bSection %d\b" % i, text))
    if n != 1:
        raise SystemExit("Section %d is there %d times:\n%s" % (i, n, text))
if text.count("The answer.") != 1:
    raise SystemExit("the answer is not there once:\n" + text)
PY
        fail "$screen: the transcript is not whole"
done

mock_stop
pass
