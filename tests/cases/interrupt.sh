#!/bin/bash
# SPDX-License-Identifier: MIT
# Ctrl-C while a model request is in flight aborts just that turn and keeps the
# REPL alive: a SIGINT during the mock's pre-response delay makes curl abort
# (CURLE_ABORTED_BY_CALLBACK), the run surfaces an "interrupted" diagnostic,
# nothing is committed for the aborted call, and the loop reads the next line
# and exits cleanly. Batch mode keeps the default disposition (not tested here).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start interrupt.json

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

# Interactive over a pipe (non-tty): one prompt, whose request stalls 5s in the
# mock. Send SIGINT ~1.5s in, mid-wait, then let stdin hit EOF.
set +e
"$FYAI_BIN" --color off --no-markdown --no-stream -i -m foo \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF' &
please answer slowly
EOF
FPID=$!
sleep 1.5
kill -INT "$FPID"
wait "$FPID"
FYAI_STATUS=$?
set -e

# The interrupted turn is reported and the session exits cleanly at EOF.
assert_status 0
assert_stderr_contains "interrupted"
assert_stdout_not_contains "This reply should never be delivered."

# Exactly one request reached the wire and nothing was committed for it.
mock_stop 1
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "This reply should never be delivered." "$TEST_DIR/state.out" && \
	fail "aborted reply leaked into state" || true

pass
