#!/bin/bash
# SPDX-License-Identifier: MIT
# The progressive shell work-band must use the same header and indented,
# bounded fenced payload as the durable history renderer.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_shell_render.json

FYAI_PTY_INPUT="run it" \
FYAI_PTY_PROGRESS_NEEDLE="tool-progress" \
FYAI_PTY_PROGRESS_TIMEOUT="1.5" \
FYAI_PTY_NEEDLE="Interactive shell rendering done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/tool_preview_lines=5 \
    --set builtin_shell=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "live shell rendering diverged from canonical tool rendering"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if b"\xe2\x97\x8f tool" in plain:
    raise SystemExit("UI-only tool chrome leaked into transcript")
if b"shell printf" not in plain:
    raise SystemExit("canonical shell header missing")
if not re.search(rb"(?:^|\r?\n)    0\r?\n", plain):
    raise SystemExit("fenced output is not indented")
EOF

"$FYAI_BIN" --color off history --last 1 >"$TEST_DIR/history.out" 2>&1 ||
    fail "history replay of unified shell output failed"
grep -qF "shell" "$TEST_DIR/history.out" ||
    fail "stored shell call missing from history"
grep -qF "0" "$TEST_DIR/history.out" ||
    fail "stored shell output missing from history"
grep -qF "Interactive shell rendering done." "$TEST_DIR/history.out" ||
    fail "stored final answer missing from history"

assert_request 1 'any(i.get("type") == "shell_call_output" for i in r["body"]["input"])'
mock_stop 2
pass
