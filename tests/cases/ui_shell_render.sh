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
FYAI_PTY_RESIZE_COLS="72" \
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
if not re.search(rb"(?:^|[\r\n])\xe2\x97\x8f shell\s+printf", plain):
    raise SystemExit("live activity dot is not beside the shell invocation")
if b"\xe2\x97\x8f working" in plain:
    raise SystemExit("activity dot was rendered on a separate chrome row")
if b"shell printf" not in plain:
    raise SystemExit("canonical shell header missing")
if not re.search(rb"(?:^|[\r\n])    tool-progress\r?\n", plain):
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
"$PYTHON" - "$TEST_DIR/history.out" <<'EOF' ||
    fail "history tool fragment diverged from frameless live rendering"
import re
import sys

data = open(sys.argv[1], "rb").read()
if b"\xe2\x94\x80" * 8 in data:
    raise SystemExit("ordinary Markdown fence rules appeared in tool output")
if not re.search(rb"(?:^|\n)    0\n", data):
    raise SystemExit("history tool output lost the live indentation")
if not re.search(rb"(?:^|\n)    10\n", data):
    raise SystemExit("history tool output lost the live tail")
if "\u22ef".encode() not in data:
    raise SystemExit("history tool output lost the bounded omission row")
EOF

"$FYAI_BIN" --transient --color off \
    --set display/tool_detail=none transcript --last 1 \
    >"$TEST_DIR/transcript-none.out" 2>&1 ||
    fail "transcript with hidden tool bodies failed"
! grep -qE '^    0$' "$TEST_DIR/transcript-none.out" ||
    fail "tool-detail none rendered a shell body"

"$FYAI_BIN" --transient --color off \
    --set display/tool_detail=full transcript --last 1 \
    >"$TEST_DIR/transcript-full.out" 2>&1 ||
    fail "transcript with full tool bodies failed"
grep -qE '^    5$' "$TEST_DIR/transcript-full.out" ||
    fail "tool-detail full did not render the middle of shell output"
if grep -qF "more lines" "$TEST_DIR/transcript-full.out"; then
    fail "tool-detail full still bounded shell output"
fi

assert_request 1 'any(i.get("type") == "shell_call_output" for i in r["body"]["input"])'
mock_stop 2
pass
