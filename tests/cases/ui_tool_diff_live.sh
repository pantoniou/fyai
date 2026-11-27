#!/bin/bash
# SPDX-License-Identifier: MIT
# The diff of a call that changed a file is presented with that call, before the
# next call runs: it does not wait for the end of the turn.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unset FYAI_TEST_NO_HOST_WARNINGS
"$PYTHON" - "$TEST_DIR/diff.json" <<'PY'
import json
import sys


def delta(**d):
    return {"choices": [{"index": 0, "delta": d}]}


def call(cmd):
    c = {"index": 0, "id": "call_1", "type": "function",
         "function": {"name": "shell",
                      "arguments": json.dumps({"command": cmd})}}
    return [delta(role="assistant"), delta(tool_calls=[c]),
            {"choices": [{"index": 0, "delta": {},
                          "finish_reason": "tool_calls"}]}]


answer = [delta(role="assistant"), delta(content="All done."),
          {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
wait = ("echo second-call-row; "
        "while [ ! -e \"$TMPDIR/shell-release\" ]; do sleep 0.05; done")
steps = []
for screen in range(2):
    steps += [{"sse": call("seq 1 3000 > big.txt; echo made-line > made.txt"), "done": True},
              {"sse": call(wait), "done": True},
              {"sse": answer, "done": True}]
json.dump({"steps": steps}, open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/diff.json"

export TMPDIR
IGNORE='view/ignore=["port","served","mock.log","requests.jsonl","stdout","stderr","out","err","tmp","config.yaml","*.out","*.held","*.json"]'
for screen in primary fullscreen; do
    rm -f made.txt big.txt
    pty_run "$TEST_DIR/$screen.out" \
        "$FYAI_BIN" -k test-key --theme dark -m mock-model \
        --set display/markdown=true --set display/stream=true \
        --set display/screen=$screen --set display/tool_display=inline \
        --set display/tool_update_interval_ms=0 --set tools=true --set sandbox_profile=dev \
        --set view/tool_diff=true --set "$IGNORE" \
        --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -i <<PTY
rows: 50
cols: 100
steps:
  - type: make a file and wait
  - wait-screen: second-call-row
  - wait-screen: "+made-line"
  - touch: "$TMPDIR/shell-release"
  - wait-screen: All done.
PTY
    rm -f "$TMPDIR/shell-release"
done

mock_stop
pass
