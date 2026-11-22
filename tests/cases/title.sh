#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the `title` subcommand: generates and stores the branch description,
# displays `--summary` without persisting either request in conversation state,
# and sets an explicit title without calling the model.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start title.json

cat > catalog.yaml <<CAT
models:
- name: foo
  capabilities: []
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: foo
    provider_model_id: bar
CAT
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret

run_bare() {
	set +e
	"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

run_bare -m foo "remember the magic word: xyzzy"
assert_status 0

# Title and summary requests include conversation context with tools disabled.
run_bare -m foo title --summary
assert_status 0
assert_stdout_contains "Magic word handling."
assert_stdout_contains "A short summary of xyzzy."
assert_request 1 'any("Write a short title" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 'any("xyzzy" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 '"tools" not in r["body"]'
assert_request 2 'any("Summarize this conversation" in str(m.get("content","")) for m in r["body"]["messages"])'

# Branch stores the unquoted title; summary and prompts are not persisted.
run_bare -m foo branch list
assert_stdout_contains "Magic word handling"
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "A short summary" "$TEST_DIR/state.out" && fail "the summary was kept" || true
grep -qF "Write a short title" "$TEST_DIR/state.out" && fail "the request was kept" || true

# Explicit title text updates the branch directly without a model request.
run_bare -m foo title A title I typed
assert_status 0
assert_stdout_contains "A title I typed"
run_bare -m foo branch list
assert_stdout_contains "A title I typed"
