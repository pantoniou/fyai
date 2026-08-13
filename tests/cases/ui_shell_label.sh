#!/bin/bash
# SPDX-License-Identifier: MIT
# Test the shell invocation label.
#
# The model supplies a short `description` for the display label. Show it in
# brackets before the command, in the same position as a sub-agent name. For a
# failed call, show the cause beside the red margin mark and the label.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_shell_label.json

FYAI_PTY_INPUT="run a labelled command that does not end" \
FYAI_PTY_NEEDLE="Shell label round trip done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses \
    --set shell/timeout_ms=60000 \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || fail "shell label is wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)

if b"[probe the slow path]" not in plain:
    raise SystemExit("shell description missing from the label")
# The command is a frameless highlighted block below the label.
if not re.search(rb"\[probe the slow path\](?:[^\n]*\n){1,4}[^\n]*echo shell-label-marker",
                 plain):
    raise SystemExit("shell command is not below the label")
if not re.search(rb"\x1b\[[0-9;]*m(?:echo|sleep)", data):
    raise SystemExit("frameless shell command is not highlighted")

# The cause is beside the label. It identifies the requested limit instead of
# the longer configured default.
if b"timed out after 1500 ms" not in plain:
    raise SystemExit("failure cause missing from the label")

# The cause and the preceding mark are red and occur on the same row.
if not re.search(rb"\x1b\[31m[^\x1b]*timed out after 1500 ms", data):
    raise SystemExit("failure cause is not rendered in red")
if not re.search(rb"\x1b\[31m\xe2\x97\x8f[^\n]*probe the slow path", data):
    raise SystemExit("failed shell call is not marked as a failure")
EOF

mock_stop 2
pass
