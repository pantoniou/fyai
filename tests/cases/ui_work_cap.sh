#!/bin/bash
# SPDX-License-Identifier: MIT
# The cap row of the work pane accounts for the tiles while they are there.
#
# A terminal session opens a tile. With display/work_cap the pane draws its
# cap row above it: the height of the pane, the tiles, how many are shown,
# and the keys that move between them. Turned off, there is no cap.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session.json

FYAI_PTY_INPUT="drive the shell" FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/cap.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_cap=true --set display/work_zoom_rows=half \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/cap.out" <<'PYEOF' || fail "the cap row is wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
plain = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\\\)", b"", plain)
text = plain.decode("utf-8", "replace")

if not re.search(r"work\S*\s+half · 1 tile · 1 shown", text):
    raise SystemExit("the cap row never accounted for the session tile")
if "^T focus" not in text:
    raise SystemExit("the cap row never named the keys")
if "\x1b_fy" in data.decode("latin1"):
    raise SystemExit("a layout marker reached the terminal")
PYEOF

mock_stop 6

# The setting persists with the branch: turned off, the pane draws no cap.
mock_start shell_session.json
FYAI_PTY_INPUT="drive the shell" FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/nocap.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_cap=false \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/nocap.out" <<'PYEOF' || fail "a cap row was drawn without the setting"
import re
import sys

plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"",
               open(sys.argv[1], "rb").read()).decode("utf-8", "replace")
if "^T focus" in plain or "1 shown" in plain:
    raise SystemExit("the cap row was drawn")
PYEOF

mock_stop 6
pass
