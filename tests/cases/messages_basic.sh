#!/bin/bash
# SPDX-License-Identifier: MIT
# Anthropic Messages API, buffered: canned reply, request wire shape and the
# x-api-key/anthropic-version auth headers (no Bearer scheme).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_basic.json

run_fyai --set api=messages --set display/stream=false -u "$MOCK_URL/v1/messages" \
	 --set "system_prompt=You are a test assistant." -m mock-model "hello mock"
assert_status 0
assert_stdout_contains "Hello from the mock messages provider."

assert_request 0 'r["path"] == "/v1/messages"'
assert_request 0 'r["x_api_key"] == "test-key"'
assert_request 0 'r["anthropic_version"] != ""'
assert_request 0 'r["auth"] == ""'
assert_request 0 'r["body"]["model"] == "mock-model"'
assert_request 0 'r["body"]["max_tokens"] > 0'
assert_request 0 '"You are a test assistant." in r["body"]["system"][0]["text"]'
# Anthropic caching is opt-in: the system block carries a breakpoint, and the
# history's last content block carries the moving one.
assert_request 0 'r["body"]["system"][0]["cache_control"] == {"type": "ephemeral"}'
assert_request 0 'r["body"]["messages"][-1]["content"][-1]["cache_control"] == {"type": "ephemeral"}'
assert_request 0 'r["body"]["messages"][-1]["role"] == "user"'
assert_request 0 'all(m.get("role") != "system" for m in r["body"]["messages"])'
assert_request 0 '"stream" not in r["body"]'

mock_stop 1
pass
