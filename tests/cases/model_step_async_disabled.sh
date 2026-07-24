#!/bin/bash
# SPDX-License-Identifier: MIT
# The compatibility switch uses the synchronous model-step request path.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_basic.json

run_fyai --set api=responses --set async_model_step=false \
	--set display/stream=false --set api_url="$MOCK_URL/v1/responses" \
	-m mock-model "hello mock"
assert_status 0
assert_stdout_contains "Hello from the mock responses provider."
assert_request 0 'r["body"]["model"] == "mock-model"'

mock_stop 1

mock_start responses_stream.json

run_fyai --new --set api=responses --set async_model_step=false \
	--set display/stream=true --set api_url="$MOCK_URL/v1/responses" \
	-m mock-model "hello stream"
assert_status 0
assert_stdout_contains "Streamed responses answer."
assert_request 0 'r["body"]["stream"] is True'

mock_stop 1

mock_start responses_stream.json

FYAI_PTY_INPUT="fallback interactive" \
FYAI_PTY_NEEDLE="Streamed responses answer." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --set async_model_step=false \
	--set display/markdown=true --set display/stream=true \
	--set api=responses --set "api_url=$MOCK_URL/v1/responses" \
	-m mock-model -i
assert_request 0 \
	'r["body"]["input"][-1]["content"] == "fallback interactive"'

mock_stop 1
pass
