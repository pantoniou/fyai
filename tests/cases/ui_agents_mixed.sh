#!/bin/bash
# SPDX-License-Identifier: MIT
# One sub-agent of a parallel group runs past its time limit; the others finish
# well inside theirs. A group is collected only when every job in it is done, so
# a job that finished early waits. Its own deadline must not still be armed
# during that wait: it would mark a complete job timed out and its result would
# be replaced, so one slow agent would fail every agent beside it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_agents_mixed.json

FYAI_PTY_INPUT="delegate both" \
FYAI_PTY_NEEDLE="Mixed agents done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set retry/max_attempts=1 --set agent/timeout_ms=1500 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

# The stored turn is the record: replay it and read the mark of each agent.
"$FYAI_BIN" --color on history --last 1 > "$TEST_DIR/replay.txt" 2>&1

"$PYTHON" - "$TEST_DIR/replay.txt" <<'PY' || fail "agent marks are wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
marks = {}
for m in re.finditer(rb"\x1b\[(3[12])m\xe2\x97\x8f([^\r\n]{0,60})", data):
    text = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", m.group(2)).decode("utf8", "replace")
    for name in ("alpha", "beta", "gamma"):
        if "[%s]" % name in text:
            marks[name] = "red" if m.group(1) == b"31" else "green"
want = {"alpha": "green", "beta": "red", "gamma": "green"}
if marks != want:
    raise SystemExit("agent marks %r, wanted %r" % (marks, want))
PY

mock_stop 5
pass
