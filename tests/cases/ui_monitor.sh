#!/bin/bash
# SPDX-License-Identifier: MIT
# A monitor turns the lines of a command into events.
#
# The command prints three lines and ends a second later. A shell may write them
# at once or one at a time, so they reach the model as one event or as several: the
# first wait returns the first event, and the others arrive as messages of the user.
# The second wait returns the end of the command.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start monitor_lines.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: watch the lines
  - wait: Monitor done.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the monitor events did not reach the model"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
# The events that no wait took arrive as messages of the user.
results = "\n".join(m["content"] for m in msgs
                    if m.get("role") in ("tool", "user") and isinstance(m.get("content"), str))

for want in ("[monitor 'feed' started", "[monitor 'feed'] line-a",
             "line-b", "line-c", "[monitor 'feed' ended]"):
    if want not in results:
        raise SystemExit("missing %r in %r" % (want, results))
PYEOF

mock_stop
pass
