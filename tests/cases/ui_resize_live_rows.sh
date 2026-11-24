#!/bin/bash
# SPDX-License-Identifier: MIT
# The rows a turn presented before a call render again at the new width when
# the terminal narrows while the call runs: the prompt and the reasoning wrap,
# and no row is cut at the old width.
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
    events.append(delta(reasoning_content="**Step %d**\n\nThe reasoning of "
                        "this step is long enough to wrap at fifty columns "
                        "END%d\n\n" % (i, i)))
events += [delta(tool_calls=[call]),
           {"choices": [{"index": 0, "delta": {},
                         "finish_reason": "tool_calls"}]}]
answer = [delta(role="assistant"), delta(content="All done."),
          {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
steps = []
for screen in range(2):
    steps += [{"sse": events, "done": True},
              {"sse": answer, "done": True}]
json.dump({"steps": steps}, open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/reason.json"

export TMPDIR
for screen in primary fullscreen; do
    pty_run "$TEST_DIR/$screen.out" \
        "$FYAI_BIN" -k test-key --theme dark -m mock-model \
        --set display/markdown=true --set display/stream=true \
        --set display/thinking=true --set display/screen=$screen \
        --set display/tool_display=inline \
        --set display/tool_update_interval_ms=0 \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -i <<PTY
rows: 50
cols: 100
steps:
  - type: think about this request until the prompt reaches PROMPTEND
  - wait-screen: live-shell-row
  - resize: 50x50
  - wait-screen: END3
  - wait-screen: PROMPTEND
  - checkpoint: "$TEST_DIR/$screen.held"
  - touch: "$TMPDIR/shell-release"
  - wait-screen: All done.
PTY
    rm -f "$TMPDIR/shell-release"
    "$PYTHON" - "$TEST_DIR/$screen.held" <<'PY' ||
import re
import sys

rows = [l[2:] for l in open(sys.argv[1]).read().split("\n")
        if l.startswith(("B|", "R|"))]
text = "\n".join(rows)
ends = [[i for i, r in enumerate(rows) if re.search(r"\bEND%d\b" % n, r)]
        for n in range(4)]
out = [i for i, r in enumerate(rows)
       if "live-shell-row" in r and "echo" not in r]
if any(len(e) != 1 for e in ends) or len(out) != 1:
    raise SystemExit("a row is missing or doubled:\n" + text)
if not ends[3][0] < out[0]:
    raise SystemExit("the shell is above the reasoning:\n" + text)
PY
        fail "$screen: the rows of the turn are not at the new width"
done

mock_stop
pass
