#!/bin/bash
# SPDX-License-Identifier: MIT
# A reload replaces the process and resumes the active stored branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
export OPENAI_API_KEY=test-key
mock_start chat_basic_twice.json

FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="Hello from the mock provider." \
FYAI_PTY_AFTER="wait-screen:Hello from the mock provider.|"\
"send:/reload|wait-screen:↳ Hello from the mock provider.|"\
"send:second question|wait-screen:Hello again from the mock provider." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" --theme dark -b main --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false -m mock-model -i

[ "$(grep -c 'start:' "$TEST_DIR/trace.log")" -ge 2 ] || \
    fail "reload did not start a second process image"
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
FYAI_PTY_INPUT="/resume newer" \
FYAI_PTY_NEEDLE="/resume newer" \
FYAI_PTY_AFTER="send:first question|"\
"wait-screen:Hello from the mock provider.|send:/reload|"\
"wait-screen:↳ Hello from the mock provider.|send:second question|"\
"wait-screen:Hello again from the mock provider." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/branch.out" \
    "$FYAI_BIN" --theme dark -b main \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false -m mock-model -i

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
FYAI_TRACE="$TEST_DIR/empty-trace.log" \
FYAI_PTY_INPUT="/reload" FYAI_PTY_NEEDLE="fyai" \
FYAI_PTY_NEEDLE_COUNT=2 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/empty.out" \
    "$FYAI_BIN" --theme dark -i
[ "$(grep -c 'start:' "$TEST_DIR/empty-trace.log")" -ge 2 ] || \
    fail "reload did not restart the fresh session"
pass
