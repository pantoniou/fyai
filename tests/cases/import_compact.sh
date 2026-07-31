#!/bin/bash
# SPDX-License-Identifier: MIT
# A compaction is an operation, not content. The export records what was asked
# for, never the opaque payload, and an import re-issues the operation against
# the provider in force now. --ignore-compact skips it instead.
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
run_fyai --set display/stream=false -m foo compact preserve the task state
assert_status 0

run_fyai export -o out.md
assert_status 0
mock_stop 2

# The operation is recorded with what was asked for.
grep -qx 'kind: compact' out.md || fail "the compaction was not recorded"
grep -qx 'instructions: preserve the task state' out.md || \
	fail "the compaction instructions were not recorded"

# The opaque payload is never serialized: it is bound to that provider and
# session, so a replay must re-issue rather than restore it.
! grep -q 'opaque-compact-state' out.md || \
	fail "the encrypted compaction payload was exported"

# Skipping the marker makes no request at all.
mkdir -p "$TEST_DIR/skip"
cd "$TEST_DIR/skip"
export HOME="$TEST_DIR/home_skip"
mkdir -p "$HOME"
run_fyai init
assert_status 0
run_fyai import -i ../out.md --ignore-compact
assert_status 0
run_fyai export -o skipped.md
assert_status 0
! grep -qx 'kind: compact' skipped.md || \
	fail "--ignore-compact still recorded a compaction"
grep -q 'remember native compaction' skipped.md || \
	fail "--ignore-compact dropped the conversation"

# Importing without the flag re-issues the operation against the mock.
cd "$TEST_DIR"
mock_start import_compact.json
mkdir -p "$TEST_DIR/replay"
cd "$TEST_DIR/replay"
export HOME="$TEST_DIR/home_replay"
mkdir -p "$HOME"
run_fyai init
assert_status 0
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

run_fyai --set display/stream=false -m foo import -i ../out.md
assert_status 0
assert_request 0 'r["path"] == "/v1/responses/compact"'
assert_request 0 '"preserve the task state" in r["body"]["instructions"]'

# The replayed conversation holds the new compaction, not the old one.
assert_state_contains "replayed-opaque-state" dump state
! grep -q 'opaque-compact-state' "$TEST_DIR/stdout" || \
	fail "the old compaction payload came back"

mock_stop 1
pass
