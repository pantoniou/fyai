#!/bin/bash
# SPDX-License-Identifier: MIT
# Responses API, buffered: canned reply, instructions/input request shape.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_basic.json

run_fyai --set api=responses --no-stream -u "$MOCK_URL/v1/responses" \
	 -s "You are a test assistant." -m mock-model "hello mock"
assert_status 0
assert_stdout_contains "Hello from the mock responses provider."

assert_request 0 'r["path"] == "/v1/responses"'
assert_request 0 'r["auth"] == "Bearer test-key"'
assert_request 0 'r["body"]["model"] == "mock-model"'
assert_request 0 '"You are a test assistant." in r["body"]["instructions"]'
assert_request 0 'r["body"]["input"][-1]["role"] == "user"'
assert_request 0 'r["body"]["input"][-1]["content"] == "hello mock"'
assert_request 0 'all(i.get("role") != "system" for i in r["body"]["input"])'
assert_request 0 '"stream" not in r["body"]'

mock_stop 1
pass
