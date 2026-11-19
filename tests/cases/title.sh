#!/bin/bash
# SPDX-License-Identifier: MIT
# The title verb: one model request writes the title, which the branch keeps
# as its description. The summary is shown and not kept; neither request
# joins the conversation. A given text needs no request.
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

# The title request follows the conversation, with no tools.
run_bare -m foo title --summary
assert_status 0
assert_stdout_contains "Magic word handling."
assert_stdout_contains "A short summary of xyzzy."
assert_request 1 'any("Write a short title" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 'any("xyzzy" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 '"tools" not in r["body"]'
assert_request 2 'any("Summarize this conversation" in str(m.get("content","")) for m in r["body"]["messages"])'

# The branch keeps the title, quotes cut; the summary and the requests are
# not in the stored state.
run_bare -m foo branch list
assert_stdout_contains "Magic word handling"
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "A short summary" "$TEST_DIR/state.out" && fail "the summary was kept" || true
grep -qF "Write a short title" "$TEST_DIR/state.out" && fail "the request was kept" || true

# A given text makes no request.
run_bare -m foo title A title I typed
assert_status 0
assert_stdout_contains "A title I typed"
run_bare -m foo branch list
assert_stdout_contains "A title I typed"
