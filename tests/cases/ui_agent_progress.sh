#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent shows a running command while it runs.
#
# A sub-agent has a terminal of its own but no display: it shows a command the
# way this program does without one, as a live region on that terminal, and the
# parent shows that terminal. Without it, a command that takes a while shows
# nothing at all until it ends and its whole output arrives at once.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_progress.json

FYAI_PTY_INPUT="count in a sub-agent" FYAI_PTY_NEEDLE="Delegated and done." \
FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses --set shell/timeout_ms=40000 \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || fail "the sub-agent showed nothing while it ran"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

data = open(sys.argv[1], "rb").read()

# The command prints a line a second. Its early lines must reach the terminal
# before the sub-agent reports, which is what "while it ran" means: a command
# whose output only arrives with its result puts everything after the report.
first = data.find(b"LINE-0")
report = data.find(b"SUBAGENT_REPORT")
if first < 0:
    raise SystemExit("the command's output was never shown")
if report < 0:
    raise SystemExit("the sub-agent never reported")
if first > report:
    raise SystemExit("the output arrived only with the result, not while it ran")

# The last lines are on screen at the end, under one invocation - the live
# region showed the call already, so it is not drawn a second time beneath it.
screen = Screen(30, 100)
screen.feed(data)
shown = "\n".join(screen.lines())
if "LINE-4" not in shown:
    raise SystemExit("the end of the output is not on screen")
if shown.count("count to four") > 1:
    raise SystemExit("the invocation was shown twice")
EOF

mock_stop 4
pass
