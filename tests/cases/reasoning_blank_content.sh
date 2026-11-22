#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that blank content between the chunks of a reasoning trace does not
# split the trace into two sections, and that the answer keeps it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/blank.json" <<'PY'
import json
import sys


def delta(**d):
    return {"choices": [{"index": 0, "delta": d}]}


events = [delta(role="assistant"),
          delta(reasoning_content="FIRST-PART of the trace."),
          delta(content="\n\n"),
          delta(reasoning_content="\n\nSECOND-PART of the trace."),
          delta(content="The answer."),
          {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
json.dump({"steps": [{"sse": events, "done": True}]}, open(sys.argv[1], "w"))
PY
mock_start "$TEST_DIR/blank.json"

run_fyai --set display/markdown=true --set display/thinking=true \
	 --set api=chat-completions --set display/stream=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "think twice"
assert_status 0

"$FYAI_BIN" --color off --set display/thinking=true history --last 1 \
	> "$TEST_DIR/replay.txt"

"$PYTHON" - "$TEST_DIR/replay.txt" <<'PY' || fail "the reasoning is split"
import sys

rows = open(sys.argv[1]).read().split("\n")
text = "\n".join(rows)
first = [i for i, r in enumerate(rows) if "FIRST-PART" in r]
second = [i for i, r in enumerate(rows) if "SECOND-PART" in r]
if len(first) != 1 or len(second) != 1:
    raise SystemExit("the trace is not in the replay:\n" + text)
# Every row from the first part to the second carries the quote gutter: a
# blank row would end the section.
gutter = rows[first[0]].lstrip()[0]
for r in rows[first[0]:second[0] + 1]:
    if not r.strip().startswith(gutter):
        raise SystemExit("a row without the gutter splits the trace:\n" +
                         text)
if text.count("reasoning") > 1:
    raise SystemExit("two reasoning sections:\n" + text)
if "The answer." not in text:
    raise SystemExit("the answer is missing:\n" + text)
PY

mock_stop 1
pass
