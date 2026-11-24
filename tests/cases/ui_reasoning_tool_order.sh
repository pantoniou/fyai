#!/bin/bash
# SPDX-License-Identifier: MIT
# A shell call that starts while the reasoning streams draws its live output
# under the reasoning, not above it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/reason.json" <<'PY'
import json
import sys


def delta(**d):
    return {"choices": [{"index": 0, "delta": d}]}


cmd = ("echo live-shell-row; "
       "while [ ! -e \"$TMPDIR/shell-release\" ]; do sleep 0.05; done")
call = {"index": 0, "id": "call_1", "type": "function",
        "function": {"name": "shell",
                     "arguments": json.dumps({"command": cmd})}}
events = [delta(role="assistant")]
for i in range(4):
    events.append(delta(reasoning_content="**Step %d**\n\nThinking about "
                        "step %d.\n\n" % (i, i)))
events += [delta(tool_calls=[call]),
           {"choices": [{"index": 0, "delta": {},
                         "finish_reason": "tool_calls"}]}]
answer = [delta(role="assistant"), delta(content="All done."),
          {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
steps = []
for screen in range(3):
    steps += [{"sse": events, "done": True},
              {"sse": answer, "done": True}]
json.dump({"steps": steps}, open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/reason.json"

export TMPDIR
for screen in primary inline fullscreen; do
    pty_run "$TEST_DIR/$screen.out" \
        "$FYAI_BIN" -k test-key --theme dark -m mock-model \
        --set display/markdown=true --set display/stream=true \
        --set display/thinking=true --set display/screen=$screen \
        --set display/tool_display=inline \
        --set display/tool_update_interval_ms=0 \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -i <<PTY
rows: 40
steps:
  - type: think
  - wait-screen: live-shell-row
  - checkpoint: "$TEST_DIR/$screen.held"
  - touch: "$TMPDIR/shell-release"
  - wait-screen: All done.
  - wait-screen: "\u276f"
  - checkpoint: "$TEST_DIR/$screen.done"
PTY
    rm -f "$TMPDIR/shell-release"
    # While the shell runs and after the turn, the reasoning is above the
    # shell, once.
    "$PYTHON" - "$TEST_DIR/$screen.held" "$TEST_DIR/$screen.done" <<'PY' ||
import re
import sys

for path in sys.argv[1:]:
    rows = [l[2:] for l in open(path).read().split("\n")
            if l.startswith(("B|", "R|"))]
    text = "\n".join(rows)
    # The output row of the shell, not the command that prints it.
    out = [i for i, r in enumerate(rows)
           if "live-shell-row" in r and "echo" not in r]
    steps = [[i for i, r in enumerate(rows) if re.search(r"\bStep %d\b" % n, r)]
             for n in range(4)]
    if len(out) != 1 or any(len(s) != 1 for s in steps):
        raise SystemExit("%s: a row is missing or doubled:\n%s" % (path, text))
    if not steps[3][0] < out[0]:
        raise SystemExit("%s: the shell is above the reasoning:\n%s" %
                         (path, text))
    if path.endswith(".done") and not out[0] < text.split("\n").index(
            next(r for r in rows if "All done." in r)):
        raise SystemExit("%s: the answer is above the shell:\n%s" %
                         (path, text))
PY
        fail "$screen: the live shell is not under the reasoning"
done

mock_stop
pass
