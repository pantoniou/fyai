#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_responses_shell_parallel.json

FYAI_PTY_INPUT="run both" \
FYAI_PTY_PROGRESS_NEEDLE="native-early-a" \
FYAI_PTY_PROGRESS_TIMEOUT="0.3" \
FYAI_PTY_NEEDLE="Native interactive tools done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set display/tool_preview_lines=5 --set tools=true \
	--set api=responses --set builtin_shell=true \
	--set "api_url=$MOCK_URL/v1/responses" \
	-m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
	fail "native shell workbands did not update progressively"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
for marker in (b"native-early-a", b"native-early-b",
               b"native-late-a", b"native-late-b"):
    if marker not in plain:
        raise SystemExit("missing native progress marker: %r" % marker)
EOF

assert_request 1 \
	'all(any(i.get("type") == "shell_call_output" and '\
'i.get("call_id") == call for i in r["body"]["input"]) for call in '\
'("call_ui_native_a", "call_ui_native_b"))'
mock_stop 2
pass
