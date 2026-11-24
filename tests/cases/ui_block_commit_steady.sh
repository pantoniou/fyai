#!/bin/bash
# SPDX-License-Identifier: MIT
# A call that ends while another runs keeps its block until the rows that take
# its place are presented: no frame shows the transcript without it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/calls.json" <<'PY'
import json
import sys


def delta(**d):
    return {"choices": [{"index": 0, "delta": d}]}


def wait(name):
    return "while [ ! -e \"$TMPDIR/%s\" ]; do sleep 0.05; done" % name


quick = ("for i in $(seq 1 30); do echo quick-$i; done; %s; echo quick-end"
         % wait("quick-go"))
held = "echo held-call-row; %s" % wait("held-go")
calls = [{"index": i, "id": "call_%d" % i, "type": "function",
          "function": {"name": "shell",
                       "arguments": json.dumps({"command": c})}}
         for i, c in enumerate([quick, held])]
events = [delta(role="assistant"), delta(content="Running both.\n\n"),
          delta(tool_calls=calls),
          {"choices": [{"index": 0, "delta": {},
                        "finish_reason": "tool_calls"}]}]
answer = [delta(role="assistant"), delta(content="All done."),
          {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
steps = []
for screen in range(2):
    steps += [{"sse": events, "done": True}, {"sse": answer, "done": True}]
json.dump({"steps": steps}, open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/calls.json"

export TMPDIR
for screen in fullscreen primary; do
    : > "$TEST_DIR/$screen.trace"
    FYAI_TRACE="$TEST_DIR/$screen.trace" pty_run "$TEST_DIR/$screen.out" \
        "$FYAI_BIN" -k test-key --theme dark -m mock-model \
        --set display/markdown=true --set display/stream=true \
        --set display/screen=$screen --set display/tool_display=inline \
        --set display/tool_update_interval_ms=0 \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -i <<PTY
rows: 30
cols: 100
steps:
  - type: run
  - wait-screen: held-call-row
  - wait-screen: quick-30
  - touch: "$TMPDIR/quick-go"
  - wait-file: "$TEST_DIR/$screen.trace"
    contains: "ui: block ends"
  - mark: true
  - frame: 2
  - checkpoint: "$TEST_DIR/$screen.held"
  - touch: "$TMPDIR/held-go"
  - wait-screen: All done.
PTY
    rm -f "$TMPDIR/quick-go" "$TMPDIR/held-go"
    "$PYTHON" - "$TEST_DIR/$screen.held" <<'PY' ||
import sys

rows = [l[2:] for l in open(sys.argv[1]).read().split("\n")
        if l.startswith(("B|", "R|"))]
text = "\n".join(rows)
for what in ("quick-30", "held-call-row"):
    if not any(what in r and "echo" not in r for r in rows):
        raise SystemExit("%s is not on the screen:\n%s" % (what, text))
PY
        fail "$screen: a call that ended left the screen before its rows"
done

mock_stop
pass
