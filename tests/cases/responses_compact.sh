#!/bin/bash
# SPDX-License-Identifier: MIT
# Responses /compact uses the native endpoint, preserves its opaque output
# window, and replays that window (not the original transcript) on continuation.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_compact.json

cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: responses
    endpoint: /v1/responses
  models:
  - canonical_id: foo
    provider_model_id: bar
EOF
run_fyai catalog import catalog.yaml
assert_status 0
export MOCKPROV_API_KEY=mock-secret

run_fyai --set display/stream=false -m foo "remember native compaction"
assert_status 0
assert_stdout_contains "Seeded Responses reply."

run_fyai --set display/stream=false -m foo compact preserve the task state
assert_status 0
assert_stdout_contains "conversation compacted (Responses API)"
assert_request 1 'r["path"] == "/v1/responses/compact"'
assert_request 1 'r["body"]["model"] == "bar"'
assert_request 1 'any("remember native compaction" in str(i) for i in r["body"]["input"])'
assert_request 1 '"preserve the task state" in r["body"]["instructions"]'

assert_state_contains "opaque-compact-state" dump state
assert_state_contains "compacted_from" dump anchors

run_fyai --set display/stream=false -m foo "continue"
assert_status 0
assert_stdout_contains "Continued after native compaction."
assert_request 2 'r["path"] == "/v1/responses"'
assert_request 2 \
	'any(i.get("type") == "compaction" and
	     i.get("encrypted_content") == "opaque-compact-state"
	     for i in r["body"]["input"])'
assert_request 2 'not any("remember native compaction" in str(i) for i in r["body"]["input"])'

mock_stop 3
pass
