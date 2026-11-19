#!/bin/bash
# SPDX-License-Identifier: MIT
# title/auto: the first exchange of a session/ branch with no description
# starts a detached title request, which stores the title. A branch that has
# a title keeps it and makes no request.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start title_auto.json

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
export FYAI_TRACE="$TEST_DIR/trace.log"

run_bare() {
	set +e
	"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

run_bare -m foo branch new session/named
assert_status 0
run_bare -m foo branch new session/plain
assert_status 0

# A titled branch: the exchange is the only request.
run_bare -m foo -b session/named title My own title
assert_status 0
run_bare -m foo -b session/named --set title/auto=true "hello"
assert_status 0
assert_stdout_contains "Seeded reply."
! grep -q "titled by process" "$TEST_DIR/trace.log" 2>/dev/null ||
	fail "a titled branch was titled again"

# An untitled one: a detached process asks for the title, which stays.
run_bare -m foo -b session/plain --set title/auto=true "hello"
assert_status 0
assert_stdout_contains "Plain reply."
pid=$(sed -n "s/.*'session\/plain' titled by process \([0-9]*\).*/\1/p" "$TEST_DIR/trace.log")
[ -n "$pid" ] || fail "no title process was started"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" $((60 * FYAI_TIMEOUT_SCALE)) ||
	fail "the title process did not end"
assert_request 2 'any("Write a short title" in str(m.get("content","")) for m in r["body"]["messages"])'
assert_request 2 '"tools" not in r["body"]'
run_bare -m foo branch list
assert_stdout_contains "Auto titled session"
assert_stdout_contains "My own title"

# The titled branch is never touched, and the model got no more requests.
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "unexpected requests"
import sys
n = sum(1 for _ in open(sys.argv[1]))
sys.exit(0 if n == 3 else 1)
PY
