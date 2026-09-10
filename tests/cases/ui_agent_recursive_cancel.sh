#!/bin/bash
# SPDX-License-Identifier: MIT
# The browser can cancel a live grandchild while its parent is running.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_progress.json

FYAI_PTY_INPUT="delegate recursively" \
FYAI_PTY_PROGRESS_NEEDLE="Agent progress" \
FYAI_PTY_PROGRESS_TIMEOUT=8 \
FYAI_PTY_DURING_INPUT="/branches" \
FYAI_PTY_NEEDLE="Branches" \
FYAI_PTY_AFTER="raw:6a6a4b|wait:Recursive delegation complete.|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=responses \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

assert_request 3 'not any(i.get("output") == "NESTED_REPORT" for i in r["body"].get("input", []))'
mock_stop 5
pass
