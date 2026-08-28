#!/bin/bash
# SPDX-License-Identifier: MIT
# A shell a sub-agent opens is shown on the terminal of the user.
#
# A sub-agent draws on a terminal of its own and the parent shows that
# terminal. A running shell is drawn by the display, so a sub-agent without one
# shows nothing of what it does: the screen of the program never reaches the
# user, only the text the sub-agent writes about it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_session.json

FYAI_PTY_INPUT="open a shell in a sub-agent" FYAI_PTY_NEEDLE="Delegated and done." \
FYAI_PTY_TIMEOUT=60 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses --set shell/timeout_ms=40000 \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "the shell of the sub-agent was not shown"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

# Inspect complete frames because the shell retires before the final screen.
shown = frames(open(sys.argv[1], "rb").read(), 40, 100)

# Require the session name and call description in the tile title.
if not any(any("shell [box]" in row and "a shell" in row for row in f)
           for f in shown):
    raise SystemExit("the shell of the sub-agent was never on the screen")

# Require the configured tile gutter without an extra rule.
marked = [row for f in shown for row in f if "AGENT_SESSION_MARK" in row]
if not marked:
    raise SystemExit("what the shell printed never reached the terminal")
if not any(row.startswith("  ") and not row.lstrip().startswith("\u2502")
           for row in marked):
    raise SystemExit("the shell was shown as text, not as the screen it is")

if not any("Delegated and done." in row for row in shown[-1]):
    raise SystemExit("the parent never reported")
PY

mock_stop 6
pass
