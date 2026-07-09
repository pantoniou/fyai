#!/bin/bash
# SPDX-License-Identifier: MIT
# The compact verb: one summary model call restarts the chain from the summary
# (canonical user content), keeps the old head as compacted_from metadata, and
# a follow-up turn replays only the summary - never the original exchange.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start compact.json

cat > catalog.yaml <<EOF
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
EOF
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

# Seed one exchange.
run_bare -m foo "remember the magic word: xyzzy"
assert_status 0
assert_stdout_contains "Seeded reply."

# Compact with a hint; the summary request replays the history plus the
# summarize instruction (tools off).
run_bare -m foo compact focus on the magic word
assert_status 0
assert_stdout_contains "conversation compacted"
assert_request 1 'any("Summarize this conversation" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 'any("focus on the magic word" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 'any("xyzzy" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 1 '"tools" not in r["body"]'

# The new chain: system + summary user turn; provenance in metadata.
assert_state_contains "Summary of prior conversation:" dump state
assert_state_contains "SUMMARY: seeded exchange, all good." dump state
assert_state_contains "compacted_from" dump anchors
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "xyzzy" "$TEST_DIR/state.out" && fail "original exchange still canonical" || true

# A follow-up turn replays only the summary.
run_bare -m foo "carry on"
assert_status 0
assert_stdout_contains "Post-compact reply."
assert_request 2 'any("SUMMARY: seeded exchange" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 2 'not any("remember the magic word" in str(m.get("content","")) for m in r["body"]["messages"])'

mock_stop 3
pass
