#!/bin/bash
# SPDX-License-Identifier: MIT
# Ctrl-C after a completed tool step persists the completed steps: step 1 is a
# read_file tool call (runs immediately), step 2 (the follow-up completion)
# stalls; a SIGINT during that wait aborts only the in-flight call. The turn
# committed to the arena therefore carries the assistant tool call and the
# tool result, but not the never-delivered final answer, and the REPL survives.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start interrupt_partial.json

printf 'mock data payload\n' > data.txt

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

set +e
"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false -i -t -m foo \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF' &
read data.txt and summarize
EOF
FPID=$!
sleep 1.5   # step 1 (tool call) done, blocked in the 5s step-2 wait
kill -INT "$FPID"
wait "$FPID"
FYAI_STATUS=$?
set -e

assert_status 0
assert_stderr_contains "interrupted"
assert_stdout_not_contains "This final answer should never arrive."

# Two requests: the tool call, then the aborted follow-up.
mock_stop 2
assert_request 1 'any(m.get("role") == "tool" and "mock data payload" in m.get("content","") for m in r["body"]["messages"])'

# The completed tool exchange persisted; the undelivered answer did not.
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "call_read_1" "$TEST_DIR/state.out" || \
	{ cat "$TEST_DIR/state.out" >&2; fail "completed tool step did not persist"; }
grep -qF "This final answer should never arrive." "$TEST_DIR/state.out" && \
	fail "aborted answer leaked into state" || true

pass
