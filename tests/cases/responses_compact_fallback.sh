#!/bin/bash
# SPDX-License-Identifier: MIT
# A Responses-compatible endpoint without native compaction uses a summary.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_compact_fallback.json

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

run_fyai --set display/stream=false -m foo "remember portable compaction"
assert_status 0
run_fyai --set display/stream=false -m foo compact preserve the task state
assert_status 0
assert_stdout_contains "conversation compacted"
assert_request 1 'r["path"] == "/v1/responses"'
assert_request 1 \
	'any("Summarize this conversation" in str(i) for i in r["body"]["input"])'
assert_request 1 '"tools" not in r["body"]'
assert_state_contains "SUMMARY: portable Responses endpoint." dump state
assert_state_contains "compacted_from" dump anchors

mock_stop 2
pass
