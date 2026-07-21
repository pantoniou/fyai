#!/bin/bash
# SPDX-License-Identifier: MIT
# Reasoning options map to the per-API wire fields and suppress temperature.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start reasoning_responses.json
run_fyai --set api=responses --set display/stream=false --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 --set reasoning/effort=high --set reasoning/summary=auto --set display/stats=true "think hard"
assert_status 0
assert_stdout_contains "Reasoned responses answer."
assert_request 0 'r["body"]["reasoning"] == {"effort": "high", "summary": "auto"}'
assert_request 0 '"temperature" not in r["body"]'
assert_stderr_contains "reasoning=18"
mock_stop 1

mock_start reasoning_chat.json
run_fyai --set api=chat-completions --set display/stream=false --set api_url="$MOCK_URL/v1/chat/completions" \
	 -m mock-model --set reasoning/effort=high --new "think hard"
assert_status 0
assert_stdout_contains "Reasoned chat answer."
assert_request 0 'r["body"]["reasoning_effort"] == "high"'
assert_request 0 '"reasoning" not in r["body"]'
assert_request 0 '"temperature" not in r["body"]'
mock_stop 1

pass
