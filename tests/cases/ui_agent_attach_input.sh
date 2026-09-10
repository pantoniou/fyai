#!/bin/bash
# SPDX-License-Identifier: MIT
# Attached input is processed by the existing grandchild before publication.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_recursive_attach_input.json

FYAI_PTY_INPUT="delegate recursively" \
FYAI_PTY_PROGRESS_NEEDLE="Agent progress" \
FYAI_PTY_PROGRESS_TIMEOUT=8 \
FYAI_PTY_DURING_INPUT=$'/branch attach main/agent:child/agent:grandchild\rATTACHED_INPUT' \
FYAI_PTY_NEEDLE="ATTACHED_REPLY" \
FYAI_PTY_AFTER="drain:0.5|send:/branch detach|wait:Recursive delegation complete." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=responses \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

assert_request 3 '"ATTACHED_INPUT" in json.dumps(r["body"])'
run_fyai -b main/agent:child/agent:grandchild transcript
assert_status 0
assert_stdout_contains "ATTACHED_REPLY"
mock_stop 6
pass
