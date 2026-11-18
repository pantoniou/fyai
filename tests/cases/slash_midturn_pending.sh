#!/bin/bash
# SPDX-License-Identifier: MIT
# A read-only slash typed while a turn runs and a message of the user waits
# for it runs beside the turn: the message in front of it does not hold it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start slash_midturn.json

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

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" --color off \
    --set display/markdown=false --set display/stream=false \
    --set tools=false --set builtin_shell=false -i -m foo <<'PTY'
steps:
  - type: first question
  - wait-file: "${TEST_DIR}/served"
    equals: "1"
    timeout: 1.5
  - type: waiting message
  - wait-screen: pending
  - write: "/status\n"
  - wait-screen: Usage / calls
    timeout: 5
  - touch: "${TEST_DIR}/release-1"
  - wait-screen: The follow-up answer.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || fail "the status waited behind the message"
import re
import sys

plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", open(sys.argv[1], "rb").read())
status = plain.find(b"Usage / calls")
answer = plain.find(b"The stalled answer.")
if status < 0 or answer < 0 or status > answer:
    raise SystemExit("status %d, answer %d" % (status, answer))
EOF
mock_stop 2
pass
