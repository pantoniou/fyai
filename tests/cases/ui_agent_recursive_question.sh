#!/bin/bash
# SPDX-License-Identifier: MIT
# A grandchild question is answered at the root prompt without a nested pump.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_question.json

FYAI_PTY_INPUT="delegate recursively" \
FYAI_PTY_PROGRESS_NEEDLE="NESTED_COLOUR?" \
FYAI_PTY_PROGRESS_TIMEOUT=8 \
FYAI_PTY_DURING_INPUT="2" \
FYAI_PTY_NEEDLE="Recursive delegation complete." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=responses \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

assert_request 3 '"green" in json.dumps(r["body"])'
mock_stop 6
pass
