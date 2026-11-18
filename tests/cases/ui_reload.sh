#!/bin/bash
# SPDX-License-Identifier: MIT
# A reload replaces the process and resumes the active stored branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
export OPENAI_API_KEY=test-key
mock_start chat_basic_twice.json

FYAI_TRACE="$TEST_DIR/trace.log" \
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" --theme dark -b main --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false -m mock-model -i <<'PTY'
steps:
  - type: first question
  - wait: Hello from the mock provider.
    timeout: session
  - wait-screen: Hello from the mock provider.
  - type: "/reload"
  - wait-screen: "↳ Hello from the mock provider."
  - type: second question
  - wait-screen: Hello again from the mock provider.
PTY

[ "$(grep -c 'start:' "$TEST_DIR/trace.log")" -ge 2 ] || \
    fail "reload did not start a second process image"
# A split session starts its worker again and keeps its client, so the process
# identity is not the subject there.
[ -n "${FYAI_TEST_SPLIT:-}" ] || \
"$PYTHON" - "$TEST_DIR/trace.log" <<'PY' || fail "reload changed process ID"
import re
import sys

starts = re.findall(r"start: pid (\d+)", open(sys.argv[1]).read())
# The first process starts again under its own PID: once per exec. With
# credential isolation the transport is one more start, under another PID.
if len(starts) < 2 or starts.count(starts[0]) < 2:
    raise SystemExit("reload did not exec in the same process")
PY
assert_request 1 'r["auth"] == "Bearer test-key"'
assert_request 1 'any("first question" in str(m.get("content")) for m in r["body"]["messages"])'
run_fyai -b main dump state
assert_status 0
assert_stdout_contains "Hello again from the mock provider."
mock_stop 2

# A branch selected by /resume differs from stored HEAD.
fyai_test_setup
export OPENAI_API_KEY=test-key
run_fyai branch create newer
assert_status 0
mock_start chat_basic_twice.json
run_fyai -b newer config set api_url "$MOCK_URL/v1/chat/completions"
assert_status 0
run_fyai -b newer config set model mock-model
assert_status 0
run_fyai -b newer config set api chat-completions
assert_status 0
run_fyai -b newer config set display/stream false
assert_status 0
FYAI_TRACE="$TEST_DIR/branch-trace.log" \
pty_run "$TEST_DIR/branch.out" \
    "$FYAI_BIN" --theme dark -b main \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false -m mock-model -i <<'PTY'
steps:
  - type: "/resume newer"
  - wait: "/resume newer"
    timeout: session
  - type: first question
  - wait-screen: Hello from the mock provider.
  - type: "/reload"
  - wait-screen: "↳ Hello from the mock provider."
  - type: second question
  - wait-screen: Hello again from the mock provider.
PTY

[ "$(grep -c 'start:' "$TEST_DIR/branch-trace.log")" -ge 2 ] || \
    fail "reload after /resume did not execute again"
assert_request 1 'r["auth"] == "Bearer test-key"'
assert_request 1 'any("first question" in str(m.get("content")) for m in r["body"]["messages"])'
run_fyai -b newer dump state
assert_status 0
assert_stdout_contains "second question"
assert_state_absent "second question" -b main dump state
mock_stop 2

# The first reload gives an unstored session a durable branch name.
fyai_test_setup
export OPENAI_API_KEY=test-key
# A process that starts again sets the title of the terminal again. A split
# session keeps its client: its worker says that it started again.
if [ -n "${FYAI_TEST_SPLIT:-}" ]; then
FYAI_TRACE="$TEST_DIR/empty-trace.log" \
pty_run "$TEST_DIR/empty.out" \
    "$FYAI_BIN" --theme dark -i <<'PTY'
steps:
  - type: "/reload"
  - wait-screen: the session restarted
PTY
else
FYAI_TRACE="$TEST_DIR/empty-trace.log" \
pty_run "$TEST_DIR/empty.out" \
    "$FYAI_BIN" --theme dark -i <<'PTY'
steps:
  - type: "/reload"
  - wait-count: fyai
    count: 2
    timeout: session
PTY
fi
[ "$(grep -c 'start:' "$TEST_DIR/empty-trace.log")" -ge 2 ] || \
    fail "reload did not restart the fresh session"
pass
